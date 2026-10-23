#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

/* The XBE's CRT memcpy also implements backward overlapping copies. Its
 * negative-index copy sled reaches 0x27A734, which is not a function entry.
 * Lowering that local jump as an unresolved tail call loses the epilogue,
 * corrupting ESP/ESI/EDI before D3D starts. Preserve the original cdecl ABI.
 * tools.recomp --exclude-manual routes direct calls and dispatch here. */
static void second_sight_crt_memmove(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t size = MEM32(g_esp + 12);
    if (size)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), size);
    g_eax = dst;
    g_esp += 4;
}

void sub_0027A490(void) { second_sight_crt_memmove(); }

/* D3D interrupt serialization adapted from antoxa2584x/flatout2-sw,
 * src/recomp_manual.c (MIT, Copyright (c) 2026 sp00nz; full notice in
 * ../../../licenses/third-party-MIT.txt). Original Second Sight bodies
 * remain generated: 00290140 matches FlatOut's 00243060 (52 instructions),
 * 0028FBB0 matches 00242AF0 (79 instructions), after relocated references
 * are normalized and identical MMIO displacements are preserved.
 *
 * Serialize DPC and game-thread calls at DISPATCH_LEVEL, and acknowledge
 * software-method traps under the executor's lock. A NOP(5) then reaches
 * Second Sight's own event setter at hardware-block +0x1A4, rather than
 * the MKSM fallback's fixed event address. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);
extern void nv2a_pb_flip_retired(unsigned n);
extern void sub_00290140_gen(void);
extern void sub_0028FBB0_gen(void);

static uint8_t second_sight_d3d_enter(void)
{
    uint8_t old = xbox_CurrentIrql();
    if (old < 2) {
        int depth = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(depth);
    }
    return old;
}

static void second_sight_d3d_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

void second_sight_d3d_install(void)
{
    /* Select the acknowledged-trap protocol before the first GPU submission.
     * Otherwise its first NOP can still take the runtime's MKSM fallback. */
    nv2a_pb_trap_lock();
    nv2a_pb_trap_unlock();
    fprintf(stderr, "[D3D] Second Sight PGRAPH/VBlank handlers installed (FlatOut 2 protocol)\n");
}

void sub_00290140(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t block = g_ecx;
    uint8_t old = second_sight_d3d_enter();
    uint32_t head = MEM32(block + 0x1BC);
    uint32_t source;

    nv2a_pb_trap_lock();
    source = nv[0x400108 / 4];
    sub_00290140_gen();
    nv2a_pb_flip_retired(MEM32(block + 0x1BC) - head);
    if (source)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    second_sight_d3d_leave(old);
}

void sub_0028FBB0(void)
{
    uint32_t block = g_ecx;
    uint8_t old = second_sight_d3d_enter();
    uint32_t head = MEM32(block + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_0028FBB0_gen();
    nv2a_pb_flip_retired(MEM32(block + 0x1BC) - head);
    second_sight_d3d_leave(old);
}

extern void sub_0028A550_gen(void);
extern void sub_0011BC80_gen(void);
extern void sub_001E2C80_gen(void);
extern void sub_001E0A30_gen(void);
extern void sub_001E1100_gen(void);
extern void sub_001E0700_gen(void);
extern void sub_00162EF0_gen(void);
extern void sub_001DF390_gen(void);
extern void sub_0005A610_gen(void);

static int ss_probe_span(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    return (va >= 0x10000u && end <= 0x04000000u) ||
           (va >= 0x80000000u && end <= 0x84000000u);
}

/* r11q: follow the original targeting pipeline without supplying inputs or
 * changing eligibility, projection, selected handles, or player state.
 * All counters are owned by the main guest thread inside 001E2C80. */
static struct {
    unsigned active, scan, auto_scan, filter, eligible, psi, psi_ok;
    unsigned project, projected, screen, on_screen;
    uint32_t sample, sample_result, sample_type, sample_model;
    float clip[4];
} ss_target;

/* Read-only first-mission callback trace, before any original instructions.
 * Animation ids come from the same 0x20-byte records used by 00071F90. */
void second_sight_script_observe(uint32_t address)
{
    static uint32_t addresses[16], last_vblank[16];
    static unsigned reports[16], used;
    uint32_t player = MEM32(0x641C5C), vb = MEM32(0x458CDC);
    if (MEM32(0x62DFBC) != 60 || !ss_probe_span(player, 0x1360)) return;
    unsigned i;
    for (i = 0; i < used && addresses[i] != address; ++i) {}
    if (i == used) {
        if (used == 16) return;
        addresses[used++] = address;
    }
    if (reports[i] && (reports[i] >= 10 || (uint32_t)(vb - last_vblank[i]) < 180)) return;
    last_vblank[i] = vb; ++reports[i];
    uint32_t object = MEM32(player + 0x20);
    uint32_t body = ss_probe_span(object, 0x210) ? MEM32(object + 0xBC) : 0;
    int32_t anim = ss_probe_span(body, 0xC60) ? (int32_t)MEM32(body + 0x4A0) : -1;
    uint32_t record = 0, anim_id = ~0u;
    if (anim >= 0 && anim < 7999) record = MEM32(0x3DC60C) + (uint32_t)anim * 0x20;
    else if (anim > 7999 && anim < 0x2116) record = MEM32(0x3DC614) + (uint32_t)(anim - 8000) * 0x20;
    if (ss_probe_span(record, 0x20)) anim_id = MEM32(record);
    uint32_t script = MEM32(0x433DF8), esp = g_esp;
    fprintf(stderr, "[SCRIPT-STAGE] fn=%08X vb=%u player=%08X body=%08X state=%08X anim=%d id=%08X tutorial=%08X health=%g/%g script=%08X timer=%d hud_state=%u args=%08X,%08X,%08X\n",
            address, vb, player, body, MEM32(player + 0x1B0), anim, anim_id,
            MEM32(0x438108), ss_probe_span(object, 0x210) ? (double)MEMF(object + 0x208) : -1.,
            ss_probe_span(object, 0x210) ? (double)MEMF(object + 0x20C) : -1., script,
            ss_probe_span(script, 0x30) ? (int32_t)MEM32(script) : -1,
            ss_probe_span(body, 0x960) ? MEM32(body + 0x958) : ~0u,
            ss_probe_span(esp, 16) ? MEM32(esp + 4) : 0,
            ss_probe_span(esp, 16) ? MEM32(esp + 8) : 0,
            ss_probe_span(esp, 16) ? MEM32(esp + 12) : 0);
}

void second_sight_caption_observe(uint32_t address)
{
    static uint32_t hashes[2] = {~0u, ~0u}, last_vblank[2];
    static unsigned reports[2];
    unsigned slot = address == 0x001FFE20u ? 0 : 1;
    uint32_t index = MEM32(0x43D1F4), vb = MEM32(0x458CDC);
    uint32_t text = slot == 0 ? MEM32(g_esp + 4) :
                    index < 2 ? 0x43D070 + index * 0x80 : 0;
    if (MEM32(0x62DFBC) != 60 || reports[slot] >= 60) return;
    char preview[97];
    uint32_t hash = 2166136261u;
    unsigned n = 0;
    if (ss_probe_span(text, sizeof preview)) {
        for (; n < sizeof preview - 1 && MEM8(text+n); ++n) {
            unsigned c = MEM8(text+n);
            hash = (hash ^ c) * 16777619u;
            preview[n] = c >= 32 && c < 127 ? (char)c : '.';
        }
    }
    preview[n] = 0;
    if (hash == hashes[slot] && (uint32_t)(vb-last_vblank[slot]) < 300) return;
    hashes[slot] = hash; last_vblank[slot] = vb; ++reports[slot];
    fprintf(stderr, "[CAPTION] fn=%08X ptr=%08X text=\"%s\" queued=%u indices=%u,%u shown_chars=%u fade=%g hidden=%08X language_table=%08X vb=%u\n",
            address, text, preview, MEM32(0x43D068), index, MEM32(0x43D1F8),
            MEM32(0x43D1FC), (double)MEMF(0x43D200), MEM32(0x651A9C)&0x18000u,
            MEM32(0x385714), vb);
}

void second_sight_vm_wait_observe(uint32_t vm)
{
    static uint32_t keys[24];
    static unsigned used;
    if (!ss_probe_span(vm, 0x258)) return;
    uint32_t flags = MEM32(vm + 8), flags_hi = MEM32(vm + 12);
    uint32_t key = flags ^ (flags_hi << 16);
    unsigned i;
    for (i = 0; i < used && keys[i] != key; ++i) {}
    if (i < used || used == 24) return;
    keys[used++] = key;
    fprintf(stderr, "[VM-WAIT-FIX] original JNE now waits: vm=%08X flags=%08X,%08X ip=%08X actor=%08X cutscene=%08X mission=%u vb=%u\n",
            vm, flags, flags_hi, MEM32(vm + 0x1C), MEM32(vm + 0x210),
            MEM32(0x61D204), MEM32(0x62DFBC), MEM32(0x458CDC));
}

void second_sight_cutscene_end_observe(void)
{
    static unsigned reports;
    if (reports++ >= 16) return;
    uint32_t scene = MEM32(0x61D204), slot = scene >> 24;
    uint32_t vm = slot < 50 ? 0x44E980 + slot * 600 : 0;
    uint32_t player = MEM32(0x641C5C), esp = g_esp;
    uint32_t object = ss_probe_span(player, 0x1360) ? MEM32(player + 0x20) : 0;
    fprintf(stderr, "[CUTSCENE-END] scene=%08X caller=%08X stack=%08X,%08X,%08X flags=%08X skip_state=%u actors=%u vm_flags=%08X,%08X vm_ip=%08X pos=%g,%g,%g vb=%u\n",
            scene, ss_probe_span(esp, 16) ? MEM32(esp) : 0,
            ss_probe_span(esp, 16) ? MEM32(esp+4) : 0,
            ss_probe_span(esp, 16) ? MEM32(esp+8) : 0,
            ss_probe_span(esp, 16) ? MEM32(esp+12) : 0,
            MEM32(0x61D208), MEM32(0x61D5F0), MEM32(0x61D39C),
            vm ? MEM32(vm+8) : 0, vm ? MEM32(vm+12) : 0, vm ? MEM32(vm+0x1C) : 0,
            ss_probe_span(object, 0xAC) ? (double)MEMF(object+0xA0) : 0.,
            ss_probe_span(object, 0xAC) ? (double)MEMF(object+0xA4) : 0.,
            ss_probe_span(object, 0xAC) ? (double)MEMF(object+0xA8) : 0., MEM32(0x458CDC));
    unsigned count = MEM32(0x61D39C);
    for (unsigned i = 0; i < count && i < 20; ++i) {
        uint32_t row = 0x61D20C + i * 20, handle = MEM32(row + 16);
        uint32_t actor = MEM32(0x3F68E0) + (handle >> 22) * 0x2D8;
        uint32_t body = handle && ss_probe_span(actor, 0xD0) && MEM32(actor+0x14) == handle ? MEM32(actor+0xBC) : 0;
        fprintf(stderr, "[CUTSCENE-ACTOR] tag=%08X options=%u,%u,%u handle=%08X body=%08X state=%u,%u anim=%u\n",
                MEM32(row), MEM32(row+4), MEM32(row+8), MEM32(row+12), handle, body,
                ss_probe_span(body, 0x798) ? MEM32(body+0x790) : ~0u,
                ss_probe_span(body, 0x798) ? MEM32(body+0x794) : ~0u,
                ss_probe_span(body, 0x4A4) ? MEM32(body+0x4A0) : ~0u);
    }
}

void sub_001E0700(void)
{
    uint32_t object = MEM32(g_esp + 4);
    sub_001E0700_gen();
    if (ss_target.active) {
        ++ss_target.filter;
        if (g_eax) ++ss_target.eligible;
        if (!ss_target.sample && ss_probe_span(object, 0xD2)) {
            ss_target.sample = object;
            ss_target.sample_result = g_eax;
            ss_target.sample_type = MEM32(object + 8);
            ss_target.sample_model = MEM16(object + 0xD0);
        }
    }
}

void sub_00162EF0(void)
{
    sub_00162EF0_gen();
    if (ss_target.active) {
        ++ss_target.psi;
        if (g_eax & 1u) ++ss_target.psi_ok;
    }
}

void sub_001DF390(void)
{
    uint32_t entry_esp = g_esp;
    sub_001DF390_gen();
    if (ss_target.active) {
        ++ss_target.screen;
        if (g_eax) ++ss_target.on_screen;
        /* Original 001DF390 leaves its transformed x,y,z,w at S-16..S-4. */
        if (ss_probe_span(entry_esp - 16, 16))
            for (unsigned i = 0; i < 4; ++i)
                ss_target.clip[i] = MEMF(entry_esp - 16 + i * 4);
    }
}

void sub_0005A610(void)
{
    sub_0005A610_gen();
    if (ss_target.active) {
        ++ss_target.project;
        if (!g_eax) ++ss_target.projected;
    }
}

void sub_001E0A30(void)
{
    if (ss_target.active) ++ss_target.auto_scan;
    sub_001E0A30_gen();
}

void sub_001E1100(void)
{
    if (ss_target.active) ++ss_target.scan;
    sub_001E1100_gen();
}

void sub_001E2C80(void)
{
    static unsigned reports;
    static uint32_t last_key = ~0u, last_vb;
    uint32_t player = MEM32(0x641C5C);
    memset(&ss_target, 0, sizeof ss_target);
    ss_target.active = 1;
    sub_001E2C80_gen();
    ss_target.active = 0;
    if (reports >= 180 || !ss_probe_span(player, 0x1360)) return;
    uint32_t index = MEM32(player), port = index < 4 ? MEM32(0x38DBA8 + index * 4) : 4;
    uint32_t object = port < 4 ? 0x38DBB8 + port * 0x3C : 0;
    uint32_t held = object ? MEM32(object + 0xC) : 0;
    uint32_t blocked = object ? MEM32(object + 0x38) : 0;
    uint32_t aim = MEM32(player + 0xEC4), power = MEM32(player + 0xFB4);
    uint32_t handle = MEM32(player + 0x12E0), vb = MEM32(0x458CDC);
    uint32_t key = ((held & ~blocked) & 0xC00) ^ (aim << 12) ^ (power << 16) ^ handle;
    if (key == last_key && (uint32_t)(vb - last_vb) < 120u) return;
    last_key = key; last_vb = vb; ++reports;
    fprintf(stderr, "[TARGET-STATE] player=%08X index=%u port=%u held=%08X blocked=%08X aim_bind=%08X use_bind=%08X aim=%u power=%u available=%u state=%08X flags=%08X,%08X handle=%08X tutorial=%08X global=%08X time_double=%u vb=%u\n",
            player, index, port, held, blocked, MEM32(player + 0x24),
            MEM32(player + 0xD4), aim, power, MEM32(player + 0xFC8),
            MEM32(player + 0x1B0), MEM32(player + 0x10), MEM32(player + 0x14),
            handle, MEM32(0x438108), MEM32(0x651A98), MEM32(0x3856F8), vb);
    fprintf(stderr, "[TARGET-STAGES] auto=%u scan=%u eligible=%u/%u psi_ok=%u/%u screen_ok=%u/%u project_ok=%u/%u auto_count=%u scan_count=%u scan_phase=%u sample=%08X type=%08X model=%04X result=%u\n",
            ss_target.auto_scan, ss_target.scan, ss_target.eligible, ss_target.filter,
            ss_target.psi_ok, ss_target.psi, ss_target.on_screen, ss_target.screen,
            ss_target.projected, ss_target.project, MEM32(0x437618), MEM32(0x437610),
            MEM32(0x437630), ss_target.sample, ss_target.sample_type,
            ss_target.sample_model, ss_target.sample_result);
    if (ss_target.screen)
        fprintf(stderr, "[TARGET-CLIP] last_xyzw=%g,%g,%g,%g\n",
                (double)ss_target.clip[0], (double)ss_target.clip[1],
                (double)ss_target.clip[2], (double)ss_target.clip[3]);
}

/* Observe both sides of the title's own trigger-to-action conversion.
 * XInput state is captured at original 0011BCBC, before its mapping loop.
 * Wrapping the completed routine captures the game's held/edge bits without
 * replacing any Xbox input or changing guest register/stack behavior. */
void second_sight_input_raw(uint32_t port, uint32_t status,
                            uint32_t lt, uint32_t rt)
{
    static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
    static unsigned reports;
    uint32_t value = lt | (rt << 8);
    if (reports >= 120 || (port < 4 && value == last[port])) return;
    if (port < 4) last[port] = value;
    ++reports;
    fprintf(stderr, "[PAD-GUEST-RAW] port=%u status=%08X LT=%u RT=%u mission=%u vblank=%u\n",
            port, status, lt, rt, MEM32(0x62DFBC), MEM32(0x458CDC));
}

void sub_0011BC80(void)
{
    static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
    static unsigned reports;
    uint32_t object = MEM32(g_esp + 4), port = MEM32(g_esp + 8);
    sub_0011BC80_gen();
    if (port < 4 && ss_probe_span(object, 0x34)) {
        uint32_t held = MEM32(object + 0xC), triggers = held & 0xC00u;
        uint32_t connected = MEM32(object + 4);
        uint32_t key = triggers | (connected << 16);
        if (key != last[port] && reports < 120) {
            last[port] = key;
            ++reports;
            fprintf(stderr, "[PAD-GUEST-ACTION] port=%u object=%08X connected=%u held=%08X pressed=%08X released=%08X LTaxis=%u RTaxis=%u mission=%u vblank=%u\n",
                    port, object, connected, held, MEM32(object + 0x10),
                    MEM32(object + 0x14), MEM32(object + 0x2C),
                    MEM32(object + 0x30), MEM32(0x62DFBC), MEM32(0x458CDC));
        }
    }
}

/* Read-only producer-side evidence for the invalid UV array. The GPU trace
 * sees attribute 3 at 03FFFFFE; capture the original declaration and stream
 * objects before the XDK routine computes and emits that address. */
void sub_0028A550(void)
{
    static uint32_t mission_seen = ~0u, keys[32];
    static unsigned count;
    uint32_t declaration = MEM32(g_esp + 4), nstream = MEM32(g_esp + 8);
    uint32_t streams = MEM32(g_esp + 12), mission = MEM32(0x0062DFBCu);
    if (declaration && nstream <= 16 && ss_probe_span(declaration, 0x114) &&
        ss_probe_span(streams, nstream * 12)) {
        uint32_t si = MEM32(declaration + 0x43);
        if (si < nstream) {
            uint32_t record = streams + si * 12, object = MEM32(record);
            uint32_t data = ss_probe_span(object, 12) ? MEM32(object + 4) : 0;
            uint32_t key = data ^ MEM32(g_esp) ^ MEM32(declaration + 0x47);
            unsigned i;
            if (mission != mission_seen) { mission_seen = mission; count = 0; }
            for (i = 0; i < count && keys[i] != key; ++i) {}
            if (i == count && count < 32) {
                unsigned a;
                keys[count++] = key;
                fprintf(stderr, "[STREAM-PROBE] mission=%u return=%08X declaration=%08X streams=%08X count=%u\n",
                        mission, MEM32(g_esp), declaration, streams, nstream);
                for (a = 0; a < nstream; ++a) {
                    uint32_t rec = streams + a * 12, ob = MEM32(rec);
                    fprintf(stderr, "[STREAM-OBJECT] stream=%u object=%08X common=%08X data=%08X stride=%u offset=%08X\n",
                            a, ob, ss_probe_span(ob, 12) ? MEM32(ob) : 0,
                            ss_probe_span(ob, 12) ? MEM32(ob + 4) : 0,
                            MEM32(rec + 4), MEM32(rec + 8));
                }
                for (a = 0; a < 4; ++a) {
                    uint32_t at = declaration + 0x13 + a * 16;
                    fprintf(stderr, "[STREAM-DECL] attr=%u stream=%u offset=%08X format=%08X\n",
                            a, MEM32(at), MEM32(at + 4), MEM32(at + 8));
                }
            }
        }
    }
    sub_0028A550_gen();
}

void second_sight_stub_observe(uint32_t address)
{
    fprintf(stderr, "[STUB-HIT] address=%08X return=%08X eax=%08X ecx=%08X edx=%08X esp=%08X mission=%u\n",
            address, MEM32(g_esp), g_eax, g_ecx, g_edx, g_esp, MEM32(0x0062DFBCu));
}

/* XPP static-table callbacks missed by the original function inventory.
 * Bodies are lifted from the XBE by recover_xpp_callbacks.py. */
extern void sub_002F758B(void);
extern void sub_002F7F66(void);
extern void sub_002F7FB8(void);
extern void sub_002F83AC(void);
extern void sub_002F89CB(void);
extern void sub_002FCD02(void);
/* Additional original callbacks observed unresolved on the console. */
extern void sub_000847F0(void);
extern void sub_001F6040(void);
extern void sub_0013A9B0_recovered(void);
extern void sub_001B5BF0(void);

/* Original one-shot callback was registered by PUSH 0013A9B0 at 0013D894.
 * Exported from the pinned XBE with upstream v0.14, with all direct callees
 * present in the current dispatch. Observe registration removal, not AI state. */
void sub_0013A9B0(void)
{
    static unsigned reports;
    unsigned matches = 0, i;
    uint32_t event_state = MEM32(0x651AC8u);
    sub_0013A9B0_recovered();
    if (reports >= 4) return;
    for (i = 0; i < 25; ++i)
        if (MEM32(0x438138u + i * 4) == 0x0013A9B0u) ++matches;
    fprintf(stderr, "[CALLBACK-RESTORED] va=0013A9B0 n=%u mission=%u event=%08X remaining_slots=%u vb=%u\n",
            ++reports, MEM32(0x62DFBCu), event_state, matches, MEM32(0x458CDCu));
}

/* Game-specific overrides are added here only when runtime evidence requires
 * them. Returning NULL makes the generated dispatcher use its normal path. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x000847F0u: return sub_000847F0;
    case 0x001F6040u: return sub_001F6040;
    case 0x0013A9B0u: return sub_0013A9B0;
    case 0x001B5BF0u: return sub_001B5BF0;
    case 0x002F758Bu: return sub_002F758B;
    case 0x002F7F66u: return sub_002F7F66;
    case 0x002F7FB8u: return sub_002F7FB8;
    case 0x002F83ACu: return sub_002F83AC;
    case 0x002F89CBu: return sub_002F89CB;
    case 0x002FCD02u: return sub_002FCD02;
    }
    return NULL;
}

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp;

void recomp_icall_fail_log(uint32_t xbox_va)
{
    static uint32_t targets[64], hits[64];
    static unsigned used;
    unsigned i, count;
    uint32_t return_va = g_esp ? MEM32(g_esp) : 0;
    for (i = 0; i < used && targets[i] != xbox_va; ++i) {}
    if (i == used) {
        if (used == 64) return;
        targets[used] = xbox_va;
        hits[used++] = 0;
    }
    count = ++hits[i];
    /* One noisy unresolved address must not hide every later missing callback.
     * Bound distinct targets and report repeated hits only at sparse counts. */
    if (count <= 2 || count == 10 || count == 100 || count == 1000) {
        fprintf(stderr,
                "[ICALL] unresolved target 0x%08X return=0x%08X; regs eax=%08X ecx=%08X "
                "edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X hits=%u\n",
                xbox_va, return_va, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, count);
    }
    fflush(stderr);
}

static void recomp_icall_not_code_log_impl(uint32_t xbox_va, uint32_t site)
{
    static uint32_t seen[16];
    static unsigned hits[16];
    static unsigned used;
    unsigned i;

    for (i = 0; i < used && seen[i] != xbox_va; ++i) {}
    if (i == used) {
        if (used == 16)
            return;
        seen[used] = xbox_va;
        hits[used++] = 0;
    }
    ++hits[i];
    if (hits[i] == 1 || (hits[i] % 100) == 0)
        fprintf(stderr,
                "[ICALL] target 0x%08X is not code at site 0x%08X (hit %u) "
                "eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                xbox_va, site, hits[i], g_eax, g_ecx, g_edx, g_ebx,
                g_esi, g_edi, g_esp);
}

void recomp_icall_not_code_log(uint32_t xbox_va)
{
    recomp_icall_not_code_log_impl(xbox_va, 0);
}

void recomp_icall_not_code_log_at(uint32_t xbox_va, uint32_t site)
{
    recomp_icall_not_code_log_impl(xbox_va, site);
}

void recomp_unimpl(const char *text, uint32_t xbox_va)
{
    /* The XDK D3D vblank ISR reads the video encoder field pin with
     * in al, dx from port 0x80C0. The lifter currently emits an unresolved
     * instruction trampoline for x86 port I/O; leaving AL untouched makes
     * D3D believe it is always on the same field, so the XMV intro waits
     * forever for the even field. Alternate bit 5 on each read until the
     * generic runtime port model is available in this Switch runtime fork. */
    if (text && strcmp(text, "in al, dx") == 0 &&
        (g_edx & 0xFFFFu) == 0x80C0u) {
        static volatile uint32_t field_reads;
        uint32_t read_number =
            __atomic_add_fetch(&field_reads, 1u, __ATOMIC_RELAXED);
        uint32_t field_bit = (read_number & 1u) ? 0x20u : 0u;
        g_eax = (g_eax & ~0xFFu) | field_bit;
        if (read_number == 1u)
            fprintf(stderr,
                    "[PORT] emulating Xbox video field pin at 0x80C0; "
                    "alternating bit 5 for D3D/XMV startup\n");
        return;
    }
    if (text && strcmp(text, "out dx, al") == 0 &&
        (g_edx & 0xFFFFu) == 0x80C0u)
        return;

    /* A noisy early instruction must not hide a later, different site.
     * Atomic slot claims/counts avoid races between game and device threads;
     * no lock is held while printing. Key VA+1 also represents address zero.
     * Repeat reports at powers of two keep long runs bounded and informative. */
    static struct { uint64_t key, hits; } sites[128];
    static unsigned overflow_reported;
    uint64_t key = (uint64_t)xbox_va + 1u;
    uint64_t hits = 0;
    unsigned i;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");

    for (i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        uint64_t found = __atomic_load_n(&sites[i].key, __ATOMIC_RELAXED);
        if (found == 0) {
            uint64_t empty = 0;
            if (__atomic_compare_exchange_n(&sites[i].key, &empty, key, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                found = key;
            else
                found = empty;
        }
        if (found == key) {
            hits = __atomic_add_fetch(&sites[i].hits, 1u, __ATOMIC_RELAXED);
            break;
        }
    }
    if (hits && !(hits & (hits - 1u))) {
        fprintf(stderr, "[UNIMPL] %s at 0x%08X hits=%llu\n",
                text ? text : "unknown instruction", xbox_va,
                (unsigned long long)hits);
    } else if (!hits && !__atomic_exchange_n(&overflow_reported, 1u, __ATOMIC_RELAXED)) {
        fprintf(stderr, "[UNIMPL-LIMIT] 128 distinct sites recorded; "
                "additional sites cannot be retained (first overflow at 0x%08X)\n",
                xbox_va);
    }
    if (trap && trap[0] && trap[0] != '0')
        abort();
}

