#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
#include "boot_diagnostics.h"
#include "nv2a_backend.h"
#include "apu.h"
#include "apu_xaudio2.h"

extern void apu_vp_get_stats(int *voices, float *mix_peak);

extern void xbox_WatchdogDump(const char *why);
extern RECOMP_TLS uint32_t g_esp;
static uint32_t *main_guest_esp;
static SDL_Thread *snapshot_thread;
static SDL_atomic_t stopping;
static uint32_t last_va, last_value, last_width;
static uintptr_t last_native_pc;
static uint64_t read_count;
static uint32_t player_model_submits, player_model_last_caller;
static void player_model_snapshot(void);

extern void (*nv2a_raw_batch_hook)(Nv2aRawBatch *batch, float *attributes);
extern void (*nv2a_vertex_offset_observe)(uint32_t, uint32_t, uint32_t);

/* The address is invalid before conversion. Capture its actual store site,
 * independently of which XDK entry point or declaration layout was used.
 * The delta and matching ELF permit symbolization despite Switch ASLR.
 * Never replace the value, relax the range guard, or read beyond guest RAM. */
__attribute__((noinline))
void recomp_vertex_write_observe(uint32_t va, uint32_t value)
{
    static uint32_t mission_seen = ~0u;
    static uintptr_t sites[32];
    static unsigned count;
    uint32_t mission = MEM32(0x0062DFBCu);
    uintptr_t pc = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    unsigned i;
    if (mission != mission_seen) { mission_seen = mission; count = 0; }
    for (i = 0; i < count; ++i) if (sites[i] == pc) return;
    if (count == 32) return;
    sites[count++] = pc;
    fprintf(stderr, "[UV-WRITER] mission=%u va=%08X value=%08X native_delta_from_observer=%lld\n",
            mission, va, value, (long long)((intptr_t)pc - (intptr_t)&recomp_vertex_write_observe));
    if ((va >= 0x10020u && va <= 0x03FFFFDCu) ||
        (va >= 0x80000020u && va <= 0x83FFFFDCu)) {
        fprintf(stderr, "[UV-WRITER-CONTEXT] base=%08X", va - 32);
        for (i = 0; i < 17; ++i) fprintf(stderr, " %08X", MEM32(va - 32 + i * 4));
        fprintf(stderr, "\n");
    }
}

static void uv_command_probe(uint32_t va, uint32_t header, uint32_t value)
{
    static uint32_t mission_seen = ~0u;
    static unsigned count;
    uint32_t mission = MEM32(0x0062DFBCu);
    unsigned i;
    if (mission != mission_seen) { mission_seen = mission; count = 0; }
    if (count++ >= 8) return;
    fprintf(stderr, "[UV-CONSUMER] mission=%u word_va=%08X header=%08X value=%08X\n",
            mission, va, header, value);
    for (i = 0; i < 4; ++i) {
        uint32_t rec = 0x2A5300u + i * 12, ob = MEM32(rec + 8);
        int valid = (ob >= 0x10000u && ob <= 0x03FFFFF4u) ||
                    (ob >= 0x80000000u && ob <= 0x83FFFFF4u);
        fprintf(stderr, "[UV-CACHED-STREAM] i=%u stride=%u offset=%08X object=%08X data=%08X\n",
                i, MEM32(rec), MEM32(rec + 4), ob, valid ? MEM32(ob + 4) : 0);
    }
}

static uint32_t gfx_trace_hash_word(uint32_t hash, uint32_t value)
{
    return (hash ^ value) * 16777619u;
}

/* Observe distinct materials after the intro. The original early GL watch
 * exhausts its allowance on the loading bar, before the menu/world exists.
 * This hook does not change the batch or the title's memory. */
static void graphics_probe(Nv2aRawBatch *b, float *attributes)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_TITLE_GFX_TRACE") != NULL;
    if (!enabled) return;
    extern int nv2a_gl_world_trace_active;
    extern uint32_t nv2a_gl_world_trace_region;
    static uint32_t mission_seen = ~0u, keys[64];
    static unsigned count;
    uint32_t mission = MEM32(0x0062DFBCu), state = MEM32(0x00651924u);
    uint32_t key = 2166136261u;
    unsigned i, a, v;
    (void)attributes;
    nv2a_gl_world_trace_active = mission == 60 && state == 5 && MEM32(0x61D204) == 0;
    if (nv2a_gl_world_trace_active) {
        uint32_t player = MEM32(0x641C5C);
        int valid = (player >= 0x10000 && player < 0x03FFF000) ||
                    (player >= 0x80000000 && player < 0x83FFF000);
        uint32_t object = valid ? MEM32(player+0x20) : 0;
        valid = (object >= 0x10000 && object < 0x03FFFF00) ||
                (object >= 0x80000000 && object < 0x83FFFF00);
        if (!valid) nv2a_gl_world_trace_active = 0;
        if (valid) {
            float x = MEMF(object+0xA0), z = MEMF(object+0xA8);
            if (x > -10000 && x < 10000 && z > -10000 && z < 10000)
                nv2a_gl_world_trace_region = ((uint32_t)(int32_t)floorf(x/8.f) & 0xFFFFu) |
                    ((uint32_t)(int32_t)floorf(z/8.f) << 16);
        }
    }
    if (mission != 101 && mission >= 100)
        return;
    if (mission != mission_seen) {
        mission_seen = mission;
        count = 0;
    }
    if (count == 64)
        return;
    key = gfx_trace_hash_word(key, b->surface.color_va);
    key = gfx_trace_hash_word(key, b->zeta_va);
    key = gfx_trace_hash_word(key, b->surface.clip_x);
    key = gfx_trace_hash_word(key, b->surface.clip_y);
    key = gfx_trace_hash_word(key, b->surface.width);
    key = gfx_trace_hash_word(key, b->surface.height);
    key = gfx_trace_hash_word(key, b->surface.pitch);
    key = gfx_trace_hash_word(key, b->prim);
    key = gfx_trace_hash_word(key, b->vertex_count);
    key = gfx_trace_hash_word(key, b->index_count);
    key = gfx_trace_hash_word(key, b->attr_present);
    key = gfx_trace_hash_word(key, b->attr_direct);
    key = gfx_trace_hash_word(key, b->xform);
    key = gfx_trace_hash_word(key, b->vp_start);
    key = gfx_trace_hash_word(key, b->vp_slots);
    key = gfx_trace_hash_word(key, b->vp_prog_gen);
    key = gfx_trace_hash_word(key, b->vp_const_gen);
    key = gfx_trace_hash_word(key, b->regs[0x1E70 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x288 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x28C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x29C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x300 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x304 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x30C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x324 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x32C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x344 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x348 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x350 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x354 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x358 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x35C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x364 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x368 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x36C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x370 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x374 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x378 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x384 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x388 / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x39C / 4]);
    key = gfx_trace_hash_word(key, b->regs[0x3A0 / 4]);
    for (i = 0; i < 4; ++i) {
        uint32_t base = (0x1B00u + i * 0x40u) / 4;
        key = gfx_trace_hash_word(key, b->tex_va[i]);
        key = gfx_trace_hash_word(key, b->pal_va[i]);
        key = gfx_trace_hash_word(key, b->regs[base]);
        key = gfx_trace_hash_word(key, b->regs[base + 1]);
        key = gfx_trace_hash_word(key, b->regs[base + 3]);
        key = gfx_trace_hash_word(key, b->regs[base + 4]);
        key = gfx_trace_hash_word(key, b->regs[base + 5]);
        key = gfx_trace_hash_word(key, b->regs[base + 7]);
    }
    for (i = 0; i < count; ++i)
        if (keys[i] == key)
            return;
    keys[count++] = key;
    fprintf(stderr, "[GFX-PROBE] sample=%u mission=%u state=%u vb=%u surf=%08X zeta=%08X clip=%u,%u+%ux%u pitch=%u bpp=%u prim=%u verts=%u indices=%u attrs=%04X direct=%04X xform=%u vp=%u/%u vpgen=%u modes=%08X final=%08X/%08X\n",
            count, mission, state, MEM32(0x00458CDCu), b->surface.color_va,
            b->zeta_va, b->surface.clip_x,
            b->surface.clip_y, b->surface.width, b->surface.height,
            b->surface.pitch, b->surface.bytes_per_pixel, b->prim, b->vertex_count,
            b->index_count, b->attr_present, b->attr_direct, b->xform,
            b->vp_start, b->vp_slots, b->vp_prog_gen,
            b->regs[0x1E70 / 4], b->regs[0x288 / 4], b->regs[0x28C / 4]);
    for (i = 0; i < 4; ++i) {
        uint32_t base = (0x1B00u + i * 0x40u) / 4;
        if (!(b->regs[base + 3] & 0x40000000u))
            continue;
        fprintf(stderr, "[GFX-TEX] t%u va=%08X raw=%08X format=%08X ctl=%08X pitch=%08X rect=%08X\n",
                i, b->tex_va[i], b->regs[base], b->regs[base + 1],
                b->regs[base + 3], b->regs[base + 4], b->regs[base + 7]);
    }
    for (a = 0; a < NV2A_RAW_ATTRS; ++a) {
        if (!(b->attr_present & (1u << a)))
            continue;
        for (v = 0; v < b->vertex_count && v < 2; ++v) {
            if (b->attr_direct & (1u << a)) {
                const uint8_t *p = b->direct[a].ptr + (size_t)v * b->direct[a].stride;
                uint32_t elem = b->direct[a].size;
                unsigned k;
                if (b->direct[a].type == 0) elem = 4;
                else if (b->direct[a].type == 2) elem *= 4;
                else if (b->direct[a].type != 4) elem *= 2;
                fprintf(stderr, "[GFX-ATTR] a%u v%u va=%08X type=%u size=%u stride=%u bytes=",
                        a, v, (uint32_t)((uintptr_t)p - xbox_GetMemoryOffset()),
                        b->direct[a].type, b->direct[a].size, b->direct[a].stride);
                for (k = 0; k < elem && k < 16; ++k)
                    fprintf(stderr, "%02X", p[k]);
                fprintf(stderr, "\n");
            } else {
                const float *p = b->attrs + ((size_t)v * NV2A_RAW_ATTRS + a) * 4;
                fprintf(stderr, "[GFX-ATTR] a%u v%u converted=%g,%g,%g,%g\n",
                        a, v, p[0], p[1], p[2], p[3]);
            }
        }
    }
    if (b->xform == 2) {
        for (i = b->vp_start; i < b->vp_slots && i < b->vp_start + 8; ++i)
            fprintf(stderr, "[GFX-VP] slot=%u %08X %08X %08X %08X\n", i,
                    b->vp_program[i][0], b->vp_program[i][1],
                    b->vp_program[i][2], b->vp_program[i][3]);
    }
}

/* Called after a normal MMIO read; filter out interrupt/worker threads.
 * Fields are sampled independently, so adjacent changing reads can appear
 * together. The guest register/stack snapshot is also asynchronous. */
void recomp_mmio_read_observe(uint32_t va, uint32_t value, unsigned width,
                             uintptr_t native_pc)
{
    if (&g_esp != main_guest_esp)
        return;
    __atomic_store_n(&last_va, va, __ATOMIC_RELAXED);
    __atomic_store_n(&last_value, value, __ATOMIC_RELAXED);
    __atomic_store_n(&last_width, width, __ATOMIC_RELAXED);
    __atomic_store_n(&last_native_pc, native_pc, __ATOMIC_RELAXED);
    __atomic_fetch_add(&read_count, 1, __ATOMIC_RELAXED);
}

static int snapshot_boot(void *unused)
{
    unsigned tick;
    uint64_t tempo_start = SDL_GetPerformanceCounter();
    uint64_t tempo_frequency = SDL_GetPerformanceFrequency();
    uint32_t tempo_mission = MEM32(0x62DFBC), tempo_vblank = MEM32(0x458CDC);
    uint32_t tempo_sim = MEM32(0x4164E8);
    uint64_t cpu_start = tempo_start;
    (void)unused;
    for (tick = 1; ; ++tick) {
        SDL_Delay(100);
        if (SDL_AtomicGet(&stopping))
            return 0;
#if defined(__SWITCH__)
        if ((tick % 100u) == 0) {
            extern void xbox_nx_thread_report(double);
            uint64_t now = SDL_GetPerformanceCounter();
            double seconds = tempo_frequency ? (double)(now-cpu_start)/tempo_frequency : 0;
            if (seconds > 0) xbox_nx_thread_report(seconds);
            cpu_start = now;
        }
#endif
        if ((tick % 50u) == 0)
            player_model_snapshot();
        /* Sample every 5 s during startup, then keep low-rate telemetry
         * alive for long play sessions instead of silently stopping at 5 min. */
        if ((tick <= 3000 && (tick % 50u) == 0) ||
            (tick > 3000 && (tick % 500u) == 0)) {
            Xa2Stats audio;
            int voices = 0;
            float mix_peak = 0;
            xa2_get_stats(&audio);
            apu_vp_get_stats(&voices, &mix_peak);
            fprintf(stderr, "[AUDIO-STATE] active=%d apu_fps=%d busy=%.3f voices=%d mix_peak=%g queued=%d submitted=%llu dropped=%llu underruns=%llu output_peak=%d\n",
                    xa2_is_active(), mcpx_apu_frames_per_second(),
                    (double)mcpx_apu_utilization(), voices, (double)mix_peak,
                    xa2_queued(), (unsigned long long)audio.submitted,
                    (unsigned long long)audio.dropped,
                    (unsigned long long)audio.underruns, audio.peak);
            uint64_t now = SDL_GetPerformanceCounter();
            uint32_t mission = MEM32(0x62DFBC), vb = MEM32(0x458CDC);
            uint32_t sim = MEM32(0x4164E8);
            double seconds = tempo_frequency ? (double)(now - tempo_start) / tempo_frequency : 0;
            if (mission == tempo_mission && seconds > 0) {
                uint32_t delta_sim = sim - tempo_sim;
                fprintf(stderr, "[TEMPO] mission=%u state=%u wall=%.3fs guest_vblank=%.2fHz sim_units=%.2f/s counter_discontinuity=%u step=%u dt=%g pause_state=%08X\n",
                        mission, MEM32(0x651924), seconds,
                        (double)(uint32_t)(vb - tempo_vblank) / seconds,
                        delta_sim < 10000u ? (double)delta_sim / seconds : -1.0,
                        delta_sim >= 10000u,
                        MEM32(0x32A9D4), (double)MEMF(0x32A9DC), MEM32(0x4164EC));
            }
            fprintf(stderr, "[PAD-ROUTE] connected_mask=%08X player_ports=%u,%u,%u,%u input_mode=%08X held=%08X,%08X,%08X,%08X\n",
                    MEM32(0x4323A4), MEM32(0x38DBA8), MEM32(0x38DBAC),
                    MEM32(0x38DBB0), MEM32(0x38DBB4), MEM32(0x457CC4),
                    MEM32(0x38DBC4), MEM32(0x38DC00), MEM32(0x38DC3C), MEM32(0x38DC78));
            tempo_start = now; tempo_mission = mission;
            tempo_vblank = vb; tempo_sim = sim;
        }
        if (tick == 100 || tick == 200 || tick == 300 || tick == 600 ||
            tick == 900 || tick == 1200 || tick == 1800 || tick == 3000) {
            char why[64];
            uintptr_t pc = __atomic_load_n(&last_native_pc, __ATOMIC_RELAXED);
            fprintf(stderr,
                "[BOOT-SAMPLE] at %us; main-thread MMIO reads=%llu "
                "last_va=%08X value=%08X width=%u "
                "native_return_delta_from_xbox_mmio_read=%lld\n",
                tick / 10,
                (unsigned long long)__atomic_load_n(&read_count, __ATOMIC_RELAXED),
                __atomic_load_n(&last_va, __ATOMIC_RELAXED),
                __atomic_load_n(&last_value, __ATOMIC_RELAXED),
                __atomic_load_n(&last_width, __ATOMIC_RELAXED),
                pc ? (long long)((intptr_t)pc - (intptr_t)&xbox_mmio_read) : 0);
            fprintf(stderr,
                "[BOOT-STATE] exit=%08X state=%u mission=%u vblank=%u base=%u\n",
                MEM32(0x313CE4), MEM32(0x651924), MEM32(0x62DFBC),
                MEM32(0x458CDC), MEM32(0x633BCC));
            snprintf(why, sizeof why, "boot sample %us", tick / 10);
            xbox_WatchdogDump(why);
        }
    }
    return 0;
}

void second_sight_diagnostics_start(void)
{
#if defined(__SWITCH__)
    extern void xbox_nx_track_thread(void *);
    xbox_nx_track_thread(NULL);
#endif
    nv2a_raw_batch_hook = graphics_probe;
    nv2a_vertex_offset_observe = uv_command_probe;
    /* These are observations, not forced register/fence/counter values. */
    setenv("RECOMP_PEEK",
        "[0x2A2E58],[0x2A2E58]+0x2C,[0x2A2E58]+0x30,"
        "[0x2A2E58]+0x934,[0x2A2E58]+0x1C20,0x80740000,0xFD003240,0xFD003244,"
        "0xFD400700,0xFD400B10,0xFD400720,0xFD100410", 1);
    /* Capture this thread's TLS register addresses without spawning the
     * runtime's optional terminating watchdog. Our sampler never aborts. */
    unsetenv("RECOMP_WATCHDOG_SECS");
    xbox_WatchdogStart();
    main_guest_esp = &g_esp;
    SDL_AtomicSet(&stopping, 0);
    snapshot_thread = SDL_CreateThread(snapshot_boot, "ss-boot-sample", NULL);
    if (!snapshot_thread)
        fprintf(stderr, "[BOOT-SAMPLE] could not start: %s\n", SDL_GetError());
    else
        fprintf(stderr, "[BOOT-SAMPLE] guest stack and MMIO snapshots scheduled through 300s\n");
}

void second_sight_diagnostics_stop(void)
{
    SDL_AtomicSet(&stopping, 1);
    if (snapshot_thread) {
        SDL_WaitThread(snapshot_thread, NULL);
        snapshot_thread = NULL;
    }
}

/* r11z: bounded observations of original guard event/state transitions. */
static int guard_span_valid(uint32_t va, uint32_t size)
{
    return size <= 0x04000000u &&
        ((va >= 0x10000u && va <= 0x04000000u-size) ||
         (va >= 0x80000000u && va <= 0x84000000u-size));
}

/* Mission 67: the independent sampler still reports a missing submit path.
 * At most 16 state records and 16 submission records; observations only.
 * Original actor/model links: [641C5C]+20 -> actor, actor+4 -> model,
 * model+0 -> actor. The masks are raw original render gates, not overrides. */
static void player_model_report(const char *category, unsigned count,
                                uint32_t caller, uint32_t submitted_model)
{
    uint32_t player = MEM32(0x641C5Cu), actor = 0, model = 0, context;
    uint32_t flags = 0, view_c4 = 0, view_c8 = 0, view_d2 = 0, passes = 0;
    uint32_t matrices = 0, owner = 0, view_bit = 0;
    if (guard_span_valid(player, 0x24)) actor = MEM32(player+0x20);
    if (guard_span_valid(actor, 0xD4)) {
        model = MEM32(actor+4); flags = MEM32(actor+0xC);
        view_c4 = MEM32(actor+0xC4); view_c8 = MEM32(actor+0xC8);
        view_d2 = MEM16(actor+0xD2); passes = MEM32(actor+0xCC);
    }
    if (guard_span_valid(model, 0x24)) {
        owner = MEM32(model); matrices = MEM32(model+0x20);
    }
    context = MEM32(0x641CA0u);
    if (guard_span_valid(context, 0x218))
        view_bit = 1u << (MEM32(context+0x214) & 31u);
    fprintf(stderr, "[%s] n=%u mission=67 state=%u player=%08X actor=%08X "
        "model=%08X owner=%08X matrices=%08X flags_C=%08X view_bit=%08X "
        "C4=%08X C8=%08X D2=%04X CC=%08X submits=%u "
        "caller=%08X submitted_model=%08X vb=%u\n", category, count,
        MEM32(0x651924u), player, actor, model, owner, matrices, flags,
        view_bit, view_c4, view_c8, view_d2, passes,
        __atomic_load_n(&player_model_submits, __ATOMIC_RELAXED), caller,
        submitted_model, MEM32(0x458CDCu));
}

static void player_model_snapshot(void)
{
    static unsigned count;
    uint32_t player=MEM32(0x641C5C), pa=guard_span_valid(player,0x24)?MEM32(player+0x20):0;
    if (count >= 16 || MEM32(0x62DFBCu) != 67 || MEM32(0x651924u) != 5)
        return;
    /* Loading can report state 5 while the new player is still absent. Do
     * not exhaust the gameplay samples/census on those transitional frames. */
    if (!guard_span_valid(pa,0xAC)) return;
    player_model_report("PLAYER-MODEL-STATE", ++count,
        __atomic_load_n(&player_model_last_caller, __ATOMIC_RELAXED), 0);
    /* Original 7F940 scans the active pool at [3F68E0], stride 2D8,
     * high-water count [643E9C]; 7FCC0 records capacity at [3F68DC].
     * Distinct live handles help separate actual actors from image trails. */
    uint32_t pool=MEM32(0x3F68E0), slots=MEM32(0x643E9C), capacity=MEM32(0x3F68DC);
    unsigned active=0, bodies=0, nearby=0, listed=0, i;
    if (!slots || slots>capacity || capacity>600 || !guard_span_valid(pool,slots*0x2D8) ||
        !guard_span_valid(pa,0xAC)) return;
    for (i=0;i<slots;i++) {
        uint32_t a=pool+i*0x2D8, flags=MEM32(a+0xC), body=MEM32(a+0xBC);
        if (flags&0x80000000u) continue;
        active++;
        if (!(flags&1) || a==pa || !guard_span_valid(body,0xC60) || MEM32(body)!=a) continue;
        bodies++;
        float dx=MEMF(a+0xA0)-MEMF(pa+0xA0), dy=MEMF(a+0xA4)-MEMF(pa+0xA4), dz=MEMF(a+0xA8)-MEMF(pa+0xA8);
        if (dx*dx+dy*dy+dz*dz>1024.f) continue;
        nearby++;
        if (count<=4 && listed++<12)
            fprintf(stderr,"[NPC-CENSUS] sample=%u slot=%u actor=%08X handle=%08X model_id=%u flags=%08X pos=%g,%g,%g target=%08X ai=%08X:%08X\n",
                    count,i,a,MEM32(a+0x14),MEM16(a+0xD0),flags,MEMF(a+0xA0),MEMF(a+0xA4),MEMF(a+0xA8),
                    MEM32(body+4),MEM32(body+0x14),MEM32(body+0x10));
    }
    fprintf(stderr,"[NPC-COUNT] sample=%u slots=%u capacity=%u active=%u bodies=%u nearby32=%u\n",
            count,slots,capacity,active,bodies,nearby);
}

/* Preserve the current fallback result while making reached lost conditions
 * visible, following TSFP's diagnostic approach. No guessed condition/AI rule. */
void recomp_unresolved_condition_observe(uint32_t function, uint32_t site)
{
    static uint32_t sites[32];
    static unsigned used;
    unsigned i;
    for (i=0; i<used; ++i) if (sites[i]==site) return;
    if (used==32) return;
    sites[used++]=site;
    fprintf(stderr,"[FLAGS-UNRESOLVED] function=%08X site=%08X mission=%u state=%u vb=%u fallback=0\n",
            function,site,MEM32(0x62DFBC),MEM32(0x651924),MEM32(0x458CDC));
}

void recomp_flag_join_observe(uint32_t function, uint32_t site, uint32_t decision)
{
    static uint32_t sites[16], values[16];
    static unsigned used;
    unsigned i;
    uint32_t bit=1u << !!decision;
    for (i=0; i<used && sites[i]!=site; ++i) {}
    if (i==used) {
        if (used==16) return;
        sites[used]=site; values[used++]=0;
    }
    if (values[i]&bit) return;
    values[i]|=bit;
    fprintf(stderr,"[FLAGS-JOIN] function=%08X site=%08X decision=%u mission=%u vb=%u\n",
            function,site,!!decision,MEM32(0x62DFBC),MEM32(0x458CDC));
}

/* One finite and one non-finite observation per original bounds stage.
 * Keep original values; diagnose source mesh/bone inputs without sanitizing. */
void recomp_bounds_observe(uint32_t stage, uint32_t actor, uint32_t half,
                          uint32_t center, uint32_t matrix)
{
    static uint32_t stages[16], observed[16];
    static unsigned used;
    uint32_t player, model=0, header=0, palette=0, bad=0;
    unsigned i, k;
    if (MEM32(0x62DFBC)!=67 || MEM32(0x651924)!=5) return;
    player=MEM32(0x641C5C);
    if (!guard_span_valid(player,0x24) || MEM32(player+0x20)!=actor ||
        !guard_span_valid(actor,0x1D0)) return;
    if (half && guard_span_valid(half,12)) {
        for(i=0;i<3;++i) if(!isfinite(MEMF(half+i*4))) bad|=1u<<i;
    } else if(half) bad|=0x100u;
    if (center && guard_span_valid(center,12)) {
        for(i=0;i<3;++i) if(!isfinite(MEMF(center+i*4))) bad|=8u<<i;
    } else if(center) bad|=0x200u;
    if (matrix && guard_span_valid(matrix,64)) {
        for(i=0;i<16;++i) if(!isfinite(MEMF(matrix+i*4))) bad|=0x40u;
    } else if(matrix) bad|=0x400u;
    for(k=0;k<used && stages[k]!=stage;++k) {}
    if(k==used) {
        if(used==16) return;
        stages[used]=stage;observed[used++]=0;
    }
    uint32_t bit=bad?2u:1u;
    if(observed[k]&bit) return;
    observed[k]|=bit;
    model=MEM32(actor+4);
    if(guard_span_valid(model,0x18)) {header=MEM32(model+4);palette=MEM32(model+0x14);}
    fprintf(stderr,"[BOUNDS-STAGE] site=%08X phase=%s actor=%08X bad=%03X model=%08X header=%08X palette=%08X callback=%08X flags=%08X pose_flags=%08X vb=%u\n",
            stage&0x7FFFFFFFu,stage&0x80000000u?"before":"after",actor,bad,model,header,palette,
            MEM32(actor+0x1CC),MEM32(actor+0xC),MEM32(actor+0x10),MEM32(0x458CDC));
    uint32_t vectors[2]={half,center};
    for(k=0;k<2;++k) if(vectors[k] && guard_span_valid(vectors[k],12))
        fprintf(stderr,"[BOUNDS-VECTOR] site=%08X kind=%s ptr=%08X f=%g,%g,%g bits=%08X,%08X,%08X\n",
                stage,k?"center":"half",vectors[k],MEMF(vectors[k]),MEMF(vectors[k]+4),MEMF(vectors[k]+8),
                MEM32(vectors[k]),MEM32(vectors[k]+4),MEM32(vectors[k]+8));
    if(matrix && guard_span_valid(matrix,64)) {
        fprintf(stderr,"[BOUNDS-MATRIX] site=%08X ptr=%08X",stage,matrix);
        for(i=0;i<16;++i) fprintf(stderr," %g",MEMF(matrix+i*4));
        fprintf(stderr,"\n");
    }
    if(guard_span_valid(header,8)) {
        uint32_t count=MEM32(header), bones=MEM32(header+4);
        fprintf(stderr,"[BOUNDS-MODEL] site=%08X meshes=%u bones=%u actor_half=%g,%g,%g actor_center=%g,%g,%g\n",
                stage,count,bones,MEMF(actor+0x88),MEMF(actor+0x8C),MEMF(actor+0x90),
                MEMF(actor+0x94),MEMF(actor+0x98),MEMF(actor+0x9C));
        if(count>0 && count<=512) for(i=0;i<count && i<3;++i) {
            uint32_t descriptor=header-count*200u+i*200u+0x90u;
            if(!guard_span_valid(descriptor-0x48,0x60)) break;
            fprintf(stderr,"[BOUNDS-MESH] site=%08X i=%u bone=%d,%d,%d center=%g,%g,%g half=%g,%g,%g\n",
                    stage,i,(int)(int8_t)MEM8(descriptor-0x47),(int)(int8_t)MEM8(descriptor-0x46),
                    (int)(int8_t)MEM8(descriptor-0x45),MEMF(descriptor),MEMF(descriptor+4),MEMF(descriptor+8),
                    MEMF(descriptor+12),MEMF(descriptor+16),MEMF(descriptor+20));
        }
    }
}

void recomp_player_cull_observe(uint32_t actor, uint32_t site, uint32_t stack)
{
    static unsigned count[4];
    static uint32_t last_vb[4];
    unsigned which=site==0x7AF18u?0:site==0x7AF63u?1:site==0x7B1CCu?2:3, i;
    uint32_t player, context, matrix=0, vb;
    if (count[which]>=4 || MEM32(0x62DFBC)!=67 || MEM32(0x651924)!=5) return;
    player=MEM32(0x641C5C);
    if (!guard_span_valid(player,0x24) || MEM32(player+0x20)!=actor ||
        !guard_span_valid(actor,0x178)) return;
    vb=MEM32(0x458CDC);
    if (count[which] && (uint32_t)(vb-last_vb[which])<120u) return;
    last_vb[which]=vb;
    context=MEM32(0x641CA0);
    if (guard_span_valid(context,0x210)) matrix=MEM32(context+0x20C);
    fprintf(stderr,"[PLAYER-CULL] n=%u site=%08X actor=%08X sector=%u bbox_id=%08X flags=%08X rect=%g,%g,%g,%g pos=%g,%g,%g matrix=%08X vb=%u\n",
            ++count[which],site,actor,MEM8(actor+0x58),MEM32(actor+0xE4),MEM32(actor+0xC),
            MEMF(actor+0x168),MEMF(actor+0x16C),MEMF(actor+0x170),MEMF(actor+0x174),
            MEMF(actor+0xA0),MEMF(actor+0xA4),MEMF(actor+0xA8),matrix,vb);
    if (which>=2 && guard_span_valid(stack+0x44,0x100)) {
        for (unsigned part=0; part<2; ++part) {
            uint32_t p=stack+(part?0xC4:0x44);
            fprintf(stderr,"[PLAYER-CULL-BOX] site=%08X kind=%s",site,part?"clip":"world");
            for (i=0;i<32;++i) fprintf(stderr," %g",MEMF(p+i*4));
            fprintf(stderr,"\n");
        }
    }
    if (guard_span_valid(matrix,0x40)) {
        fprintf(stderr,"[PLAYER-CULL-MATRIX] site=%08X",site);
        for (i=0;i<16;++i) fprintf(stderr," %g",MEMF(matrix+i*4));
        fprintf(stderr,"\n");
    }
}

void recomp_shadow_camera_observe(uint32_t actor, uint32_t parameters)
{
    static unsigned count;
    static uint32_t last_vb;
    unsigned i;
    uint32_t vb;
    if (count>=8 || MEM32(0x62DFBC)!=60 || MEM32(0x651924)!=5 || MEM32(0x61D204)) return;
    if (!guard_span_valid(actor,0xC0) || !guard_span_valid(parameters,0x110)) return;
    vb=MEM32(0x458CDC);
    if (count && (uint32_t)(vb-last_vb)<120u) return;
    last_vb=vb;
    fprintf(stderr,"[SHADOW-CAMERA] n=%u actor=%08X first_actor=%08X id=%u model=%08X center=%g,%g,%g light=%g,%g,%g bounds=%g,%g,%g,%g,%g,%g vb=%u\n",
            ++count,actor,MEM32(0x433D4C),MEM32(actor+0x14),MEM32(actor+4),
            MEMF(parameters+0xC0),MEMF(parameters+0xC4),MEMF(parameters+0xC8),
            MEMF(parameters+0xCC),MEMF(parameters+0xD0),MEMF(parameters+0xD4),
            MEMF(parameters+0xD8),MEMF(parameters+0xDC),MEMF(parameters+0xE0),
            MEMF(parameters+0xE4),MEMF(parameters+0xE8),MEMF(parameters+0xEC),vb);
    fprintf(stderr,"[SHADOW-CAMERA-MATRICES] n=%u view_then_projection",count);
    for (i=0;i<32;++i) fprintf(stderr," %g",MEMF(parameters+i*4));
    fprintf(stderr,"\n");
}

void recomp_player_render_gate_observe(uint32_t actor, uint32_t site, uint32_t decision)
{
    static unsigned count[3];
    static uint32_t last_vb[3];
    uint32_t player, context, vb;
    unsigned slot=site==0x7B600u?0:site==0x7B830u?1:2;
    if (count[slot]>=16 || MEM32(0x62DFBC)!=67 || MEM32(0x651924)!=5) return;
    player=MEM32(0x641C5C);
    if (!guard_span_valid(player,0x24) || MEM32(player+0x20)!=actor ||
        !guard_span_valid(actor,0x28C)) return;
    vb=MEM32(0x458CDC);
    if (count[slot] && (uint32_t)(vb-last_vb[slot])<120u) return;
    last_vb[slot]=vb;
    context=MEM32(0x641CA0);
    fprintf(stderr,"[PLAYER-RENDER-GATE] n=%u site=%08X actor=%08X decision=%08X type=%08X flags=%08X visibility=%08X id=%u model=%08X context=%08X vb=%u\n",
            ++count[slot],site,actor,decision,MEM32(actor+8),MEM32(actor+0xC),
            MEM32(actor+0x288),MEM32(actor+0x5C),MEM32(actor+4),context,vb);
}

void recomp_player_model_observe(uint32_t caller, uint32_t model)
{
    static unsigned count;
    static uint32_t last_ms;
    uint32_t player, actor, now;
    if (MEM32(0x62DFBCu) != 67 || MEM32(0x651924u) != 5) return;
    player = MEM32(0x641C5Cu);
    if (!guard_span_valid(player, 0x24)) return;
    actor = MEM32(player+0x20);
    if (!guard_span_valid(actor, 8) || MEM32(actor+4) != model) return;
    __atomic_fetch_add(&player_model_submits, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&player_model_last_caller, caller, __ATOMIC_RELAXED);
    if (count >= 16) return;
    now = SDL_GetTicks();
    if (count && (uint32_t)(now-last_ms) < 1000u) return;
    last_ms = now;
    player_model_report("PLAYER-MODEL-SUBMIT", ++count, caller, model);
}

void recomp_guard_event_observe(uint32_t actor, uint32_t event,
                               uint32_t kind, uint32_t caller)
{
    static unsigned count;
    uint32_t body;
    float original_increment;
    if (count >= 32 || MEM32(0x62DFBCu) != 60 ||
        !guard_span_valid(actor, 0xC0) || !guard_span_valid(event, 8)) return;
    body = MEM32(actor+0xBC);
    if (!guard_span_valid(body, 0xC60)) return;
    original_increment = (kind == 1 || kind == 8 || kind == 0x2000) ? .5f : .3f;
    ++count;
    fprintf(stderr, "[GUARD-EVENT] n=%u caller=%08X actor=%08X body=%08X "
        "event=%08X handle=%08X mask=%08X kind=%08X increment=%g "
        "fear=%g state=%08X:%08X substate=%u pending=%08X:%08X "
        "pos=%g,%g,%g vb=%u\n", count, caller, actor, body, event,
        MEM32(event), MEM32(event+4), kind, original_increment,
        MEMF(body+0xA60), MEM32(body+0x14), MEM32(body+0x10), MEM32(body+0xC),
        MEM32(body+0x794), MEM32(body+0x790), MEMF(actor+0xA0),
        MEMF(actor+0xA4), MEMF(actor+0xA8), MEM32(0x458CDCu));
}

void recomp_guard_transition_observe(uint32_t caller, uint32_t body,
                                    uint32_t next_low, uint32_t next_high)
{
    static unsigned count;
    uint32_t current_low, current_high, actor;
    if (count >= 32 || MEM32(0x62DFBCu) != 60 ||
        !guard_span_valid(body, 0xC60)) return;
    current_low = MEM32(body+0x10); current_high = MEM32(body+0x14);
    /* Sparse: only entering or leaving the original 00800000 fear handler. */
    if (!((next_low == 0x00800000u && next_high == 0) ||
          (current_low == 0x00800000u && current_high == 0))) return;
    actor = MEM32(body);
    ++count;
    fprintf(stderr, "[GUARD-TRANSITION] n=%u caller=%08X body=%08X "
        "actor=%08X from=%08X:%08X to=%08X:%08X fear=%g substate=%u "
        "target=%08X source=%u vb=%u\n", count, caller, body, actor,
        current_high, current_low, next_high, next_low, MEMF(body+0xA60),
        MEM32(body+0xC), MEM32(body+4), MEM32(body+0x7CC), MEM32(0x458CDCu));
}

void recomp_guard_animation_observe(uint32_t caller, uint32_t actor, uint32_t animation)
{
    static unsigned count;
    uint32_t body;
    if (count >= 32 || MEM32(0x62DFBCu) != 60 ||
        !(animation == 0x95 || animation == 0x96 || animation == 0x97 || animation == 0x98 ||
          animation == 0x2AE || animation == 0x1B4 || animation == 0x2B1 ||
          animation == 0x2B2 || animation == 0x2EF) ||
        !guard_span_valid(actor, 0xC0)) return;
    body = MEM32(actor+0xBC);
    if (!guard_span_valid(body,0xC60)) return;
    const char *name = animation == 0x95 ? "cowr_loop_scrd" :
        animation == 0x96 ? "stnd_cowr_scrd" : animation == 0x97 ? "cowr_stnd" :
        animation == 0x98 ? "cowridle_scrd" : animation == 0x2AE ? "getscrd" :
        animation == 0x1B4 ? "scared_" : animation == 0x2B1 ? "scrdtocower" :
        animation == 0x2B2 ? "scrdrun" : "overwrite_scaredpose";
    fprintf(stderr,"[GUARD-ANIMATION] n=%u caller=%08X actor=%08X body=%08X anim=%X name=%s fear=%g state=%08X:%08X encoded=%08X vb=%u\n",
            ++count,caller,actor,body,animation,name,MEMF(body+0xA60),MEM32(body+0x14),MEM32(body+0x10),MEM32(body+0xC),MEM32(0x458CDCu));
}

/* Doctors/civilians also encode their behavior in body+C directly, bypassing
 * the generic guard state request at 00013AC0. Observe only the first decision
 * and encoded-state changes for each of at most 16 NPCs, at most 64 lines.
 * The classification result is deliberately logged raw: its visibility/hearing
 * meaning has not yet been established. No guest fields are changed. */
void recomp_doctor_state_observe(uint32_t caller, uint32_t actor, uint32_t body,
                                uint32_t previous, uint32_t next, uint32_t classification)
{
    static struct { uint32_t actor, state; } observed[16];
    static unsigned actors, count;
    unsigned i;
    uint32_t player, player_actor = 0, target;
    float distance = -1.f;
    if (count >= 64 || MEM32(0x62DFBCu) != 60 || MEM32(0x651924u) != 5 ||
        !guard_span_valid(actor, 0xC0) || !guard_span_valid(body, 0xC60) ||
        MEM32(body) != actor) return;
    player = MEM32(0x641C5C);
    if (guard_span_valid(player, 0x24)) player_actor = MEM32(player+0x20);
    if (actor == player_actor) return;
    for (i = 0; i < actors; ++i) if (observed[i].actor == actor) break;
    if (i == actors) {
        if (actors == 16) return;
        observed[actors++].actor = actor;
    } else if (observed[i].state == next && previous == next) return;
    observed[i].state = next;
    if (guard_span_valid(player_actor, 0xAC)) {
        float dx = MEMF(actor+0xA0)-MEMF(player_actor+0xA0);
        float dy = MEMF(actor+0xA4)-MEMF(player_actor+0xA4);
        float dz = MEMF(actor+0xA8)-MEMF(player_actor+0xA8);
        distance = sqrtf(dx*dx+dy*dy+dz*dz);
    }
    target = MEM32(body+4);
    fprintf(stderr, "[DOCTOR-STATE] n=%u caller=%08X actor=%08X body=%08X "
        "encoded=%08X->%08X ai=%08X:%08X classification=%08X flags=%08X/%08X "
        "fear=%g timer=%g elapsed_since944=%d target=%08X source=%u "
        "player_distance=%g pos=%g,%g,%g vb=%u\n", ++count, caller, actor, body,
        previous, next, MEM32(body+0x14), MEM32(body+0x10), classification,
        MEM32(body+0x8E8), MEM32(body+0x8E0), MEMF(body+0xA60), MEMF(body+0x7B4),
        (int32_t)(MEM32(0x651930u)-MEM32(body+0x944)), target, MEM32(body+0x7CC),
        distance, MEMF(actor+0xA0), MEMF(actor+0xA4), MEMF(actor+0xA8),
        MEM32(0x458CDCu));
}


