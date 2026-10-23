/*
 * Optional CPU boost for the Switch port.
 * Clock service handling follows the MIT-licensed FlatOut 2 Switch port;
 * see ../../../licenses/third-party-MIT.txt.
 */
#include <switch.h>

#include <stdio.h>
#include <string.h>

#define SECOND_SIGHT_CPU_TARGET_HZ 1785000000u

static int s_clock_api = -1; /* 1 = clkrst, 0 = pcv, -1 = unavailable */
static ClkrstSession s_cpu_session;
static u32 s_cpu_old_hz;
static u32 s_cpu_set_hz;
static volatile int s_clock_stop;
static Thread s_clock_thread;
static int s_clock_thread_up;

void second_sight_clock_stop(void);

static Result cpu_clock_get(u32 *hz)
{
    return s_clock_api ? clkrstGetClockRate(&s_cpu_session, hz)
                       : pcvGetClockRate(PcvModule_CpuBus, hz);
}

static Result cpu_clock_set(u32 hz)
{
    return s_clock_api ? clkrstSetClockRate(&s_cpu_session, hz)
                       : pcvSetClockRate(PcvModule_CpuBus, hz);
}

#if defined(SECOND_SIGHT_CPU_BOOST)
static u32 cpu_clock_pick(u32 requested)
{
    u32 rates[64], best = 0;
    s32 count = 0;
    PcvClockRatesListType type;
    Result rc = s_clock_api
        ? clkrstGetPossibleClockRates(&s_cpu_session, rates, 64, &type, &count)
        : pcvGetPossibleClockRates(PcvModule_CpuBus, rates, 64, &type, &count);
    s32 i;

    if (R_FAILED(rc) || count <= 0 || type != PcvClockRatesListType_Discrete)
        return requested;
    for (i = 0; i < count; i++)
        if (rates[i] <= requested && rates[i] > best)
            best = rates[i];
    return best ? best : requested;
}
#endif

/* Respect sys-clk and Horizon OC; do not compete with a clock manager. */
static int clock_service_running(const char *name)
{
    Handle handle;
    Result rc = smRegisterService(&handle, smEncodeName(name), false, 1);
    if (R_SUCCEEDED(rc)) {
        smUnregisterService(smEncodeName(name));
        svcCloseHandle(handle);
        return 0;
    }
    return rc == MAKERESULT(21, 4); /* sm: already registered */
}

static int external_clock_manager_running(void)
{
    return clock_service_running("hoc:clk") || clock_service_running("sys:clk");
}

#if defined(SECOND_SIGHT_CPU_BOOST)
static void cpu_clock_keeper(void *arg)
{
    (void)arg;
    while (!s_clock_stop) {
        u32 current = 0;
        svcSleepThread(1000000000ull);
        if (s_clock_stop || appletGetFocusState() != AppletFocusState_InFocus)
            continue;
        if (!s_cpu_set_hz || R_FAILED(cpu_clock_get(&current)) || current == s_cpu_set_hz)
            continue;
        if (current > s_cpu_set_hz) {
            fprintf(stderr, "[clock] CPU raised to %u MHz externally; leaving it alone\n",
                    current / 1000000u);
            s_cpu_set_hz = 0;
            continue;
        }
        if (external_clock_manager_running()) {
            fprintf(stderr, "[clock] sys-clk/Horizon OC detected; leaving CPU profile to it\n");
            s_cpu_set_hz = 0;
            continue;
        }
        if (R_SUCCEEDED(cpu_clock_set(s_cpu_set_hz)))
            fprintf(stderr, "[clock] CPU reset by system; restored %u MHz\n",
                    s_cpu_set_hz / 1000000u);
    }
}
#endif

void second_sight_clock_start(void)
{
#if defined(SECOND_SIGHT_CPU_BOOST)
    u32 target, current = 0;
#endif
    Result rc;
    int external_manager = external_clock_manager_running();

    if (external_manager) {
        fprintf(stderr, "[clock] external clock manager found; using its profile\n");
    }
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize()))
        s_clock_api = 1;
    else if (R_SUCCEEDED(pcvInitialize()))
        s_clock_api = 0;
    else {
        fprintf(stderr, "[clock] clkrst/pcv unavailable; CPU clock unchanged\n");
        return;
    }

    if (s_clock_api) {
        rc = clkrstOpenSession(&s_cpu_session, PcvModuleId_CpuBus, 3);
        if (R_FAILED(rc)) {
            fprintf(stderr, "[clock] CPU clock session failed: 0x%X\n", rc);
            clkrstExit();
            s_clock_api = -1;
            return;
        }
    }
    if (R_FAILED(cpu_clock_get(&s_cpu_old_hz))) {
        fprintf(stderr, "[clock] cannot read current CPU rate\n");
        second_sight_clock_stop();
        return;
    }
#if !defined(SECOND_SIGHT_CPU_BOOST)
    fprintf(stderr, "[clock] CPU observed at %u MHz (read only; boost disabled)\n",
            s_cpu_old_hz / 1000000u);
    second_sight_clock_stop();
    return;
#else
    if (external_manager) {
        fprintf(stderr, "[clock] CPU observed at %u MHz (read only; external profile)\n",
                s_cpu_old_hz / 1000000u);
        second_sight_clock_stop();
        return;
    }
    target = cpu_clock_pick(SECOND_SIGHT_CPU_TARGET_HZ);
    if (s_cpu_old_hz > target) {
        fprintf(stderr, "[clock] CPU already at %u MHz; leaving it alone\n",
                s_cpu_old_hz / 1000000u);
        second_sight_clock_stop();
        return;
    }
    if (s_cpu_old_hz == target) {
        s_cpu_set_hz = target;
        fprintf(stderr, "[clock] CPU already at target %u MHz\n", target / 1000000u);
    } else {
        rc = cpu_clock_set(target);
        (void)cpu_clock_get(&current);
        if (R_FAILED(rc) || current < target) {
            fprintf(stderr, "[clock] CPU boost %u -> %u MHz refused (result 0x%X)\n",
                    s_cpu_old_hz / 1000000u, current / 1000000u, rc);
            second_sight_clock_stop();
            return;
        }
        s_cpu_set_hz = current;
        fprintf(stderr, "[clock] CPU %u -> %u MHz; restores on exit\n",
                s_cpu_old_hz / 1000000u, current / 1000000u);
    }

    s_clock_stop = 0;
    rc = threadCreate(&s_clock_thread, cpu_clock_keeper, NULL, NULL, 0x4000, 0x2C, -2);
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&s_clock_thread))) {
        s_clock_thread_up = 1;
    } else if (R_SUCCEEDED(rc)) {
        fprintf(stderr, "[clock] resume keeper unavailable; initial boost remains active\n");
        threadClose(&s_clock_thread);
    } else {
        fprintf(stderr, "[clock] resume keeper could not start: 0x%X\n", rc);
    }
#endif
}

void second_sight_clock_stop(void)
{
    u32 current = 0;
    if (s_clock_api < 0)
        return;
    s_clock_stop = 1;
    if (s_clock_thread_up) {
        threadWaitForExit(&s_clock_thread);
        threadClose(&s_clock_thread);
        s_clock_thread_up = 0;
    }
    if (s_cpu_set_hz && s_cpu_old_hz && R_SUCCEEDED(cpu_clock_get(&current))
        && current == s_cpu_set_hz && s_cpu_old_hz < s_cpu_set_hz) {
        if (R_SUCCEEDED(cpu_clock_set(s_cpu_old_hz)))
            fprintf(stderr, "[clock] restored CPU to %u MHz\n", s_cpu_old_hz / 1000000u);
    }
    if (s_clock_api) {
        clkrstCloseSession(&s_cpu_session);
        clkrstExit();
    } else {
        pcvExit();
    }
    s_clock_api = -1;
    s_cpu_set_hz = 0;
}

