#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A short PC histogram for the actual Second Sight target. The sampler in
 * the upstream mksm-switch launcher is not part of this project's CMake
 * target, so this diagnostic belongs to second_sight_recomp itself. */
#define PROFILE_MAX_THREADS 64
#define PROFILE_BUCKET_COUNT 8192
#define PROFILE_TOP_COUNT 16

typedef struct {
    u32 pc;
    u32 count;
    u16 slot;
    u16 _pad;
} ProfileBucket;

typedef struct {
    u32 pc;
    u32 count;
} ProfileHotspot;

static ProfileBucket s_profile_buckets[PROFILE_BUCKET_COUNT];
static u32 s_profile_samples;
static u32 s_profile_overflow;

extern int xbox_nx_thread_at(int i, Handle *handle, uintptr_t *entry);
extern void _start(void);

static u32 profile_offset(u64 address)
{
    u64 base = (u64)(uintptr_t)&_start;
    return address >= base && address - base < 0xFFFFFFFFull
        ? (u32)(address - base) : 0xFFFFFFFFu;
}

static void profile_record(u16 slot, u32 pc)
{
    u32 index, probe;

    if (pc == 0xFFFFFFFFu)
        return;
    index = (pc * 2654435761u ^ (u32)slot * 2246822519u)
        & (PROFILE_BUCKET_COUNT - 1);
    for (probe = 0; probe < PROFILE_BUCKET_COUNT; probe++) {
        ProfileBucket *bucket = &s_profile_buckets[index];
        if (bucket->count == 0) {
            bucket->pc = pc;
            bucket->slot = slot;
            bucket->count = 1;
            s_profile_samples++;
            return;
        }
        if (bucket->pc == pc && bucket->slot == slot) {
            bucket->count++;
            s_profile_samples++;
            return;
        }
        index = (index + 1) & (PROFILE_BUCKET_COUNT - 1);
    }
    s_profile_overflow++;
}

static void profile_emit(void)
{
    ProfileHotspot top[PROFILE_MAX_THREADS][PROFILE_TOP_COUNT] = {{{0}}};
    u32 totals[PROFILE_MAX_THREADS] = {0};
    Handle handle;
    uintptr_t entry;
    char line[1024];
    int i, slot, len;

    len = snprintf(line, sizeof line, "[prof] slots:");
    for (i = 0; i < PROFILE_MAX_THREADS &&
                xbox_nx_thread_at(i, &handle, &entry) &&
                len < (int)sizeof line - 32; i++) {
        len += snprintf(line + len, sizeof line - (size_t)len, " %d=%08X",
                        i, entry ? profile_offset((uintptr_t)entry) : 0u);
    }
    fprintf(stderr, "%s\n", line);

    for (i = 0; i < PROFILE_BUCKET_COUNT; i++) {
        ProfileBucket *bucket = &s_profile_buckets[i];
        ProfileHotspot *list;
        int rank;
        if (!bucket->count || bucket->slot >= PROFILE_MAX_THREADS)
            continue;
        totals[bucket->slot] += bucket->count;
        list = top[bucket->slot];
        for (rank = 0; rank < PROFILE_TOP_COUNT; rank++) {
            if (bucket->count > list[rank].count) {
                int move;
                for (move = PROFILE_TOP_COUNT - 1; move > rank; move--)
                    list[move] = list[move - 1];
                list[rank].pc = bucket->pc;
                list[rank].count = bucket->count;
                break;
            }
        }
    }

    for (slot = 0; slot < PROFILE_MAX_THREADS; slot++) {
        ProfileHotspot *list = top[slot];
        if (!totals[slot])
            continue;
        len = snprintf(line, sizeof line, "[prof] hot slot=%d samples=%u",
                       slot, totals[slot]);
        for (i = 0; i < PROFILE_TOP_COUNT && list[i].count &&
                    len < (int)sizeof line - 32; i++) {
            len += snprintf(line + len, sizeof line - (size_t)len,
                            " %06X:%u", list[i].pc, list[i].count);
        }
        fprintf(stderr, "%s\n", line);
    }
    fprintf(stderr, "[prof] window samples=%u histogram_overflow=%u\n",
            s_profile_samples, s_profile_overflow);
    memset(s_profile_buckets, 0, sizeof s_profile_buckets);
    s_profile_samples = 0;
    s_profile_overflow = 0;
}

static void profile_thread(void *arg)
{
    Handle handles[PROFILE_MAX_THREADS];
    u64 last_ticks[PROFILE_MAX_THREADS] = {0};
    int busy[PROFILE_MAX_THREADS] = {0};
    int thread_count = 0;
    u64 last_list = 0;
    u64 last_report = armGetSystemTick();
    u64 frequency = armGetSystemTickFreq();
    (void)arg;

    for (;;) {
        u64 now;
        svcSleepThread(40000000ull);
        now = armGetSystemTick();

        if (now - last_list > frequency / 10) {
            u64 span = last_list ? now - last_list : 1;
            uintptr_t entry;
            int i, counted = 0;
            for (thread_count = 0;
                 thread_count < PROFILE_MAX_THREADS &&
                 xbox_nx_thread_at(thread_count, &handles[thread_count], &entry);
                 thread_count++) {
                u64 ticks = 0;
                if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount,
                                        handles[thread_count], (u64)-1))) {
                    if (R_FAILED(svcGetInfo(&ticks,
                                            InfoType_ThreadTickCountDeprecated,
                                            handles[thread_count], (u64)-1)))
                        ticks = 0;
                }
                counted |= ticks != 0;
                busy[thread_count] = last_list &&
                    (ticks - last_ticks[thread_count]) * 10 > span;
                last_ticks[thread_count] = ticks;
            }
            if (!counted) {
                for (i = 0; i < thread_count; i++)
                    busy[i] = i == 0;
            }
            last_list = now;
        }

        {
            int i;
            for (i = 0; i < thread_count; i++) {
                ThreadContext context;
                if (!busy[i] || R_FAILED(svcSetThreadActivity(
                                      handles[i], ThreadActivity_Paused)))
                    continue;
                if (R_SUCCEEDED(svcGetThreadContext3(&context, handles[i])))
                    profile_record((u16)i, profile_offset(context.pc.x));
                svcSetThreadActivity(handles[i], ThreadActivity_Runnable);
            }
        }

        if (now - last_report > frequency * 10) {
            profile_emit();
            last_report = armGetSystemTick();
        }
    }
}

void second_sight_profile_start(void)
{
    static Thread sampler;
    if (R_FAILED(threadCreate(&sampler, profile_thread, NULL, NULL,
                              0x4000, 0x2A, -2)) ||
        R_FAILED(threadStart(&sampler))) {
        fprintf(stderr, "[prof] could not start the CPU sampler\n");
        return;
    }
    fprintf(stderr, "[prof] 25 Hz CPU sampler active; PCs are offsets from NRO _start; diagnostic timings include sampling overhead\n");
}

