#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__SWITCH__)
#include <switch/result.h>
#include <switch/runtime/nxlink.h>
/* Keep libnx's socket service declarations local: including socket.h pulls
 * kernel service headers that collide with the embedded QEMU MemoryRegion. */
extern Result socketInitialize(const void *config);
extern void socketExit(void);
static int g_nxlink_log_fd = -1;
static bool g_nxlink_socket_initialized = false;
#endif

#include <xbox/xboxrecomp.h>
#include "recomp_funcs.h"
#include "boot_diagnostics.h"
#include "language_config.h"

#define SECOND_SIGHT_GAME_DIR "sdmc:/switch/secondsight/game/e"
#define SECOND_SIGHT_SAVE_DIR "sdmc:/switch/secondsight/save"
#define SECOND_SIGHT_XBE_PATH SECOND_SIGHT_GAME_DIR "/ss_ntsc.xbe"
#define SECOND_SIGHT_ENTRY_POINT 0x002722B9u

extern void xbe_entry_point(void);
extern void second_sight_d3d_install(void);
extern void xbox_OhciInit(void);
#if defined(SECOND_SIGHT_VULKAN)
extern void nv2a_vk_install(void);
#else
extern void nv2a_gl_install(void);
#endif
extern RECOMP_TLS uint32_t g_esp;
void second_sight_profile_start(void);
#if defined(__SWITCH__)
void second_sight_clock_start(void);
void second_sight_clock_stop(void);
#endif

/* APU lifecycle/MMIO hookup follows FlatOut 2's MIT-licensed main.c; notice:
 * ../../../licenses/third-party-MIT.txt. The device model itself is
 * already supplied by our linked xboxrecomp runtime. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern void mcpx_apu_shutdown(MCPXAPUState *state);
extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *, uint64_t, unsigned);
extern void mcpx_apu_mmio_write(MCPXAPUState *, uint64_t, uint64_t, unsigned);

static uint32_t second_sight_apu_read(void *opaque, uint32_t offset, unsigned size)
{
    static unsigned seen;
    uint32_t value = (uint32_t)mcpx_apu_mmio_read(opaque, offset, size);
    if (!seen++)
        fprintf(stderr, "[APU-MMIO] first read offset=%05X size=%u value=%08X\n",
                offset, size, value);
    return value;
}

static void second_sight_apu_write(void *opaque, uint32_t offset,
                                    uint32_t value, unsigned size)
{
    static unsigned seen;
    if (!seen++)
        fprintf(stderr, "[APU-MMIO] first write offset=%05X size=%u value=%08X\n",
                offset, size, value);
    mcpx_apu_mmio_write(opaque, offset, value, size);
}

static void *load_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    long size;
    void *data;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)size;
    return data;
}

static void open_switch_log(void)
{
#if defined(__SWITCH__)
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/secondsight", 0777);
    (void)freopen("sdmc:/switch/secondsight/secondsight.log", "w", stdout);
    /* When launched through hbmenu's Netloader, stream diagnostics directly
     * back to nxlink. libnx requires its socket service to be initialized
     * before nxlinkConnectToHost(); without it the client never reaches the
     * nxlink listener and stderr silently falls back to the SD log. */
    g_nxlink_socket_initialized = R_SUCCEEDED(socketInitialize(NULL));
    if (g_nxlink_socket_initialized)
        g_nxlink_log_fd = nxlinkConnectToHost(false, true);
    if (g_nxlink_log_fd >= 0)
        fprintf(stderr, "[boot] nxlink stderr stream connected\n");
    else if (fileno(stdout) >= 0)
        (void)dup2(fileno(stdout), fileno(stderr));
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

int main(void)
{
    void *xbe_data;
    size_t xbe_size = 0;

    open_switch_log();
    fprintf(stderr, "[boot] Second Sight recomp starting [" SECOND_SIGHT_BUILD_ID "]\n");

#if defined(__SWITCH__)
    /* D3D pushbuffer execution is the runtime's guest GPU submission path. */
    setenv("RECOMP_PB_EXEC", "1", 0);
#if defined(SECOND_SIGHT_VULKAN)
    /* Performance diagnostic build: preserve FPS/perf/error output while
     * suppressing high-volume per-operation kernel logging over nxlink. */
    setenv("RECOMP_QUIET", "1", 1);
    setenv("RECOMP_VK_PERF", "1", 1);
    setenv("RECOMP_FRAME_PROFILE", "1", 1);
    {
        extern void recomp_spin_configure(unsigned us);
#if defined(SECOND_SIGHT_HW_PROGRESS)
        recomp_spin_configure(250);
#else
        recomp_spin_configure(0);
#endif
    }
#if defined(SECOND_SIGHT_PB_BULK)
    setenv("RECOMP_PB_BULK", "1", 1);
#else
    setenv("RECOMP_PB_BULK", "0", 1);
#endif
#if defined(SECOND_SIGHT_VK_RAW_CMP)
    setenv("RECOMP_VK_RAW_CMP", "1", 1);
#else
    setenv("RECOMP_VK_RAW_CMP", "0", 1);
#endif
    fprintf(stderr, "[boot] Vulkan raw CMP normals=%s\n", getenv("RECOMP_VK_RAW_CMP"));
#if defined(SECOND_SIGHT_VK_FEEDBACK_COPY)
    setenv("RECOMP_VK_FEEDBACK_COPY", "1", 1);
#else
    setenv("RECOMP_VK_FEEDBACK_COPY", "0", 1);
#endif
    fprintf(stderr, "[boot] Vulkan feedback copies=%s\n", getenv("RECOMP_VK_FEEDBACK_COPY"));
#if defined(SECOND_SIGHT_VK_RT_ALIASES)
    setenv("RECOMP_VK_RT_ALIASES", "1", 1);
#else
    setenv("RECOMP_VK_RT_ALIASES", "0", 1);
#endif
    fprintf(stderr, "[boot] Vulkan render-target aliases=%s\n", getenv("RECOMP_VK_RT_ALIASES"));
#if defined(SECOND_SIGHT_GAME_LTO)
    fprintf(stderr, "[boot] experimental game-only LTO=1\n");
#else
    fprintf(stderr, "[boot] experimental game-only LTO=0\n");
#endif
#if defined(SECOND_SIGHT_VK_UNIFORM_CACHE)
    setenv("RECOMP_VK_UNIFORM_CACHE", "1", 1);
#else
    setenv("RECOMP_VK_UNIFORM_CACHE", "0", 1);
#endif
    fprintf(stderr, "[boot] Vulkan immutable uniform reuse=%s\n",
            getenv("RECOMP_VK_UNIFORM_CACHE"));
    fprintf(stderr, "[boot] Vulkan lazy instance matching=%s (default 1)\n",
            getenv("RECOMP_VK_LAZY_MATCH") ? getenv("RECOMP_VK_LAZY_MATCH") : "1");
    unsetenv("RECOMP_DRAW_STATS");
#endif
    unsetenv("RECOMP_GL_OVERLAY_TRACE");
    /* Swap's mode setup waits on device+0x1DBC. The runtime's timer delivers
     * the real guest ISR/DPC path only when explicitly enabled, before its
     * first tick caches this setting. Without it that event stays unsignaled. */
    setenv("RECOMP_VBLANK", "1", 0);
    /* The log shows DirectSound polling AC97 codec status at 0xFEC00130,
     * which is zero in the Switch's plain MCPX aperture. The runtime's
     * documented plain mode reports codec-ready and acknowledges reset bits. */
    setenv("RECOMP_AC97_READY", "plain", 0);
    /* FUN_00268A30 advances its stream offset only through the pending-read
     * completion state. The r11b trace repeats a successful 256000-byte read
     * at offset 1583104 because immediate success leaves that state at 1.
     * Enable the runtime's existing async-handle completion contract. */
    setenv("RECOMP_ASYNC_IO", "1", 1);
    /* XPP discovers the Xbox pad through MCPX OHCI. Its lifted MMIO
     * accesses need the USB device model, just as in FlatOut 2. */
    setenv("RECOMP_USB", "1", 0);
    unsetenv("RECOMP_INPUT_DIAG");
    setenv("RECOMP_FPS_LOG", "1", 1);
    setenv("RECOMP_GL_PERF", "1", 1);
    /* Comparison build: no shader/vertex dumps, framebuffer readbacks or
     * full-frame PVIDEO hashes. Keep bounded copy/wait messages and FPS/audio. */
    unsetenv("RECOMP_GL_TRACE");
    unsetenv("RECOMP_GL_SMALL_RT_TRACE");
    unsetenv("RECOMP_GL_WATCH");
    unsetenv("RECOMP_GL_WATCH_POINTS");
#if defined(SECOND_SIGHT_GFX_DIAGNOSTICS)
    setenv("RECOMP_TITLE_GFX_TRACE", "1", 1);
#else
    unsetenv("RECOMP_TITLE_GFX_TRACE");
#endif
    unsetenv("RECOMP_TITLE_SHADOW_CAPTURE");
    fprintf(stderr, "[boot] runtime PB_EXEC=%s VBLANK=%s AC97_READY=%s\n",
            getenv("RECOMP_PB_EXEC"), getenv("RECOMP_VBLANK"),
            getenv("RECOMP_AC97_READY"));
    fprintf(stderr, "[boot] runtime ASYNC_IO=%s\n", getenv("RECOMP_ASYNC_IO"));
#if defined(SECOND_SIGHT_GFX_DIAGNOSTICS)
    fprintf(stderr, "[boot] bounded NV2A draw-state capture enabled (64 distinct states per mission)\n");
#else
    fprintf(stderr, "[boot] GPU bulk diagnostics off; copy/wait and FPS/audio summaries retained\n");
#endif
#endif

    if (SDL_Init(0) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "[boot] SDL initialized\n");

    xbe_data = load_file(SECOND_SIGHT_XBE_PATH, &xbe_size);
    if (!xbe_data) {
        fprintf(stderr, "Cannot open %s\n", SECOND_SIGHT_XBE_PATH);
        SDL_Quit();
        return 1;
    }

    fprintf(stderr, "Second Sight Xbox XBE: %zu bytes\n", xbe_size);
    fprintf(stderr, "[boot] initializing Xbox memory layout\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Xbox memory layout initialization failed\n");
        free(xbe_data);
        SDL_Quit();
        return 1;
    }
    fprintf(stderr, "[boot] Xbox memory layout ready\n");

    fprintf(stderr, "[boot] initializing kernel and game paths\n");
    xbox_kernel_init();
    xbox_path_init(second_sight_game_directory(), SECOND_SIGHT_SAVE_DIR);
    xbox_kernel_bridge_init();
    fprintf(stderr, "[DIAG] MmAllocateContiguousMemoryEx thunk 0x2FCEB0 after bridge = 0x%08X\n",
            MEM32(0x002FCEB0u));
    fprintf(stderr, "[boot] kernel bridge ready\n");
    /* Plain MCPX memory alone lets DirectSound initialize but cannot decode
     * or advance voices. DSOUND's generated MMIO accessors need a registered
     * device, as in the donor ports. Install it before the guest starts. */
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    if (g_apu_state) {
        int rc = xbox_MmioRegister(0xFE800000u, 0xFE880000u,
                                  second_sight_apu_read, second_sight_apu_write,
                                  g_apu_state);
        fprintf(stderr, "[boot] APU initialized; MMIO registration=%d\n", rc);
    } else {
        fprintf(stderr, "[boot] APU initialization failed\n");
    }
    xbox_OhciInit();
    fprintf(stderr, "[boot] USB input model initialized USB=%s INPUT_DIAG=%s\n",
            getenv("RECOMP_USB"), getenv("RECOMP_INPUT_DIAG"));

    /* Second Sight XDK D3D device singleton and shared XDK fence layout:
     * PUT sequence at device +0x2C, pointer to GPU progress at +0x30. */
    if (xbox_Nv2aMirrorFence(0x002A2E58u, 0x2C, 0x30) != 0)
        fprintf(stderr, "Could not register Second Sight NV2A fence mirror\n");
    second_sight_d3d_install();

    g_esp = XBOX_STACK_TOP;

#if defined(SECOND_SIGHT_VULKAN)
    nv2a_vk_install();
#else
    nv2a_gl_install();
#endif
    fprintf(stderr, "[boot] graphics backend installed\n");
    if (!recomp_dispatch_init())
        fprintf(stderr, "Generated dispatch table unavailable; using fallback lookup\n");

    fprintf(stderr, "[DIAG] MmAllocateContiguousMemoryEx thunk 0x2FCEB0 before guest = 0x%08X\n",
            MEM32(0x002FCEB0u));
#if defined(__SWITCH__)
    second_sight_clock_start();
#endif
    fprintf(stderr, "Starting guest at 0x%08X\n", SECOND_SIGHT_ENTRY_POINT);
    fprintf(stderr, "[boot] entering translated game code\n");
#if defined(__SWITCH__) && defined(SECOND_SIGHT_CPU_PROFILE)
    second_sight_profile_start();
#endif
    second_sight_diagnostics_start();
    xbe_entry_point();

    second_sight_diagnostics_stop();
#if defined(__SWITCH__)
    second_sight_clock_stop();
#endif
    if (g_apu_state) {
        mcpx_apu_shutdown(g_apu_state);
        g_apu_state = NULL;
    }
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    SDL_Quit();
#if defined(__SWITCH__)
    if (g_nxlink_log_fd >= 0)
        close(g_nxlink_log_fd);
    if (g_nxlink_socket_initialized)
        socketExit();
#endif
    return 0;
}

