#include <switch.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "boot_diagnostics.h"

/* Follow devkitPro/examples/switch/exception-handler. A separate translation
 * unit avoids the libnx and embedded NV2A kernel header name collisions. */
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern void _start(void);
extern __thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp;
extern ptrdiff_t xbox_GetMemoryOffset(void);

static char crash_report[8192];

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    const uintptr_t base = (uintptr_t)&_start;
    size_t used = 0;
    unsigned i;
    int fd;
#define CRASH_APPEND(...) do { \
    if (used < sizeof(crash_report) - 1) { \
        int written = snprintf(crash_report + used, sizeof(crash_report) - used, __VA_ARGS__); \
        if (written > 0) { \
            size_t available = sizeof(crash_report) - used - 1; \
            used += (size_t)written < available ? (size_t)written : available; \
        } \
    } \
} while (0)

    CRASH_APPEND("\n[CRASH] build=" SECOND_SIGHT_BUILD_ID " desc=%08X esr=%08X far=%016llX\n",
                 ctx->error_desc, ctx->esr, (unsigned long long)ctx->far.x);
    CRASH_APPEND("[CRASH] base=%016llX pc=%016llX pc_offset=%llX lr=%016llX lr_offset=%llX sp=%016llX fp=%016llX\n",
                 (unsigned long long)base, (unsigned long long)ctx->pc.x,
                 (unsigned long long)(ctx->pc.x - base),
                 (unsigned long long)ctx->lr.x,
                 (unsigned long long)(ctx->lr.x - base),
                 (unsigned long long)ctx->sp.x, (unsigned long long)ctx->fp.x);
    for (i = 0; i < 29; ++i)
        CRASH_APPEND("[CRASH] x%u=%016llX\n", i,
                     (unsigned long long)ctx->cpu_gprs[i].x);

    /* Send the native register record before attempting any memory walk. */
    (void)write(STDERR_FILENO, crash_report, used);
    {
        size_t native_size = used;
        uintptr_t frame = (uintptr_t)ctx->fp.x;
        for (i = 0; i < 12 && frame && (frame & 15u) == 0; ++i) {
            MemoryInfo info;
            u32 page_info;
            const uintptr_t *words;
            if (R_FAILED(svcQueryMemory(&info, &page_info, frame)) ||
                !(info.perm & Perm_R) || frame < info.addr ||
                info.size < 16 || frame - info.addr > info.size - 16)
                break;
            words = (const uintptr_t *)frame;
            CRASH_APPEND("[CRASH-BT] n=%u return=%016llX offset=%llX\n", i,
                         (unsigned long long)words[1],
                         (unsigned long long)(words[1] - base));
            if (words[0] <= frame)
                break;
            frame = words[0];
        }
        CRASH_APPEND("[CRASH-GUEST] eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                     g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
        if (g_esp >= 0x00780000u && g_esp < 0x00F80000u) {
            ptrdiff_t guest_base = xbox_GetMemoryOffset();
            for (i = 0; i < 48 && g_esp + i * 4u < 0x00F80000u; ++i) {
                uint32_t address = g_esp + i * 4u;
                CRASH_APPEND("[CRASH-GS] %08X %08X\n", address,
                    *(const volatile uint32_t *)((uintptr_t)address + guest_base));
            }
        }
        (void)write(STDERR_FILENO, crash_report + native_size, used - native_size);
    }
    fd = open("sdmc:/switch/secondsight/crash-" SECOND_SIGHT_BUILD_REV ".log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, crash_report, used);
        close(fd);
    }
    svcSleepThread(200000000);
    svcExitProcess();
#undef CRASH_APPEND
}

