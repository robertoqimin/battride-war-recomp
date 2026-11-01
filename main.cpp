/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif
#ifdef __linux__
#include <link.h>       /* dl_iterate_phdr: the code segment, see remap_text_huge */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     port_fs_register_extensions(void);
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);
void     cellGame_init_from_paramsfo(const char*);
void     port_register_hle(void);
void     port_register_hle_early(void);
void     port_sail_poll(void);
void     port_register_spu(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */
void port_ppu_store_gate_update(void);        /* loader_bridge.cpp: after ppu_vm_size */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
void     cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken when a guest waits on a fence */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32) && !defined(PS3RECOMP_RSX_VULKAN)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#elif defined(PS3RECOMP_RSX_VULKAN)
extern "C" int rsx_vulkan_backend_init(uint32_t, uint32_t, const char*);
extern "C" void rsx_vulkan_backend_present_begin(void);
extern "C" void rsx_vulkan_backend_present_poll(void);
extern "C" int rsx_vulkan_backend_pump_messages(void);
#  define rsx_backend_init rsx_vulkan_backend_init
/* Only the first half: the GPU renders the frame while this thread walks the
 * next one. The poll (or the next begin) shows it once it is done. */
#  define rsx_backend_present rsx_vulkan_backend_present_begin
#  define rsx_backend_poll rsx_vulkan_backend_present_poll
#  define rsx_backend_pump rsx_vulkan_backend_pump_messages
#  define RSX_BACKEND_NAME "Vulkan"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif
#ifndef rsx_backend_poll
#  define rsx_backend_poll() ((void)0)
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
    /* Guest frame completion intervals, not GPU display timings. Buffered
     * CSV logging is opt-in and does no file I/O on normal runs. */
    static FILE* timings = []() -> FILE* {
        const char* path = getenv("PORT_FRAME_TIMES");
        if (!path || !*path) return nullptr;
        FILE* f = fopen(path, "w");
        if (f) { setvbuf(f, nullptr, _IOFBF, 65536); fprintf(f, "frame,elapsed_ms,interval_ms\n"); }
        return f;
    }();
    if (timings) {
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        static auto last = start;
        static bool have_last = false;
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(now-start).count();
        const double interval = have_last ? std::chrono::duration<double, std::milli>(now-last).count() : 0.0;
        have_last = true;
        fprintf(timings, "%u,%.6f,%.6f\n", ppu_boot_frames_presented(), elapsed, interval);
        last = now;
        if (ppu_boot_frames_presented() % 60u == 0) fflush(timings);
    }
}

/* Set once the frame clock has created the window (or failed to). The guest
 * must not start before then: its first cellPadInit initialises SDL's
 * controller subsystem on the main thread while window creation dispatches
 * SDL events on the frame clock's, and the two take SDL's joystick and
 * event-watch locks in opposite orders -- a startup deadlock, seen with
 * MangoHud's timing but possible on any run. */
static HANDLE g_backend_ready;

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    SetEvent(g_backend_ready);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();

    for (;;) {
        /* Woken early when a guest thread waits on a fence the drain writes. */
        cellGcm_fifo_kick_wait(4);
        port_sail_poll();
        rsx_backend_poll();
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            /* A drain stops at each flip. Present it and keep draining in this
             * pass instead of after the next wait, a few frames at most. */
            for (int pass = 0; pass < 4; pass++) {
                if (cellGcm_take_flip_pending()) {
                    present_guest_frame();
                    last_flip = cellGcm_flip_request_count();
                } else if (pass) {
                    break;
                }
                cellGcm_rsx_process_fifo();
            }

            if (rsx_backend_pump() != 0) {
                rsx_ok = 0;              /* window closed */
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if defined(__linux__) && defined(MADV_HUGEPAGE) && defined(MREMAP_FIXED)
/* The translated PPU code is close to 100 MB, and the busy game thread jumps
 * all over it, so 4 KB code pages keep missing the instruction TLB. Most
 * kernels cannot back a file's code with huge pages, so copy the 2 MB-aligned
 * part of the text into anonymous huge pages and move that over the original
 * with one atomic mremap. The bytes and addresses are the same, so this code,
 * symbols and profiles are unaffected. Runs before any other thread starts;
 * any failure leaves the original mapping in place. PORT_HUGEPAGES=0 skips it.
 *
 * Not for guest memory: MADV_HUGEPAGE on the arena in alloc_guest_vm made the
 * STG02 combat slower on a Ryzen 5 4600H (45.7 against 49.9 fps over six and
 * five runs, twice the frames over 33 ms), cause not established. */
static int find_text_segment(struct dl_phdr_info* info, size_t, void* out)
{
    for (int i = 0; i < info->dlpi_phnum; i++) {   /* first object: the executable */
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
            ((uintptr_t*)out)[0] = info->dlpi_addr + ph.p_vaddr;
            ((uintptr_t*)out)[1] = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
            break;
        }
    }
    return 1;
}
static void remap_text_huge(void)
{
    const char* huge = getenv("PORT_HUGEPAGES");
    if (huge && *huge == '0') return;
    uintptr_t text[2] = {0, 0};
    dl_iterate_phdr(find_text_segment, text);
    const uintptr_t huge_page = 2u << 20;
    const uintptr_t start = (text[0] + huge_page - 1) & ~(huge_page - 1);
    const uintptr_t end = text[1] & ~(huge_page - 1);
    if (end <= start) return;
    const size_t len = end - start;
    /* Huge pages only form at 2 MB-aligned addresses: over-map and trim. */
    uint8_t* raw = (uint8_t*)mmap(NULL, len + huge_page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return;
    uint8_t* tmp = (uint8_t*)(((uintptr_t)raw + huge_page - 1) & ~(huge_page - 1));
    if (tmp > raw) munmap(raw, tmp - raw);
    if (raw + huge_page > tmp) munmap(tmp + len, raw + huge_page - tmp);
    madvise(tmp, len, MADV_HUGEPAGE);
    memcpy(tmp, (const void*)start, len);
    if (mprotect(tmp, len, PROT_READ | PROT_EXEC) != 0 ||
        mremap(tmp, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, (void*)start) == MAP_FAILED) {
        munmap(tmp, len);
        return;
    }
    fprintf(stderr, "[port] %zu MiB of code moved to huge pages\n", len >> 20);
}
#endif

#ifdef PORT_PGO_GENERATE
/* Training builds (PORT_PGO=generate): GCC writes its profiles at a normal
 * exit, which the runner does not reliably reach, so SIGTERM and SIGINT write
 * them and end the process. Not in ordinary builds. */
#include <csignal>
#include <unistd.h>
extern "C" void __gcov_dump(void);
static void pgo_dump_and_exit(int)
{
    __gcov_dump();
    _exit(0);
}
#endif
/* Out of line, so main's control flow is the same in training and use builds. */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static void pgo_training_hooks(void)
{
#ifdef PORT_PGO_GENERATE
    signal(SIGTERM, pgo_dump_and_exit);
    signal(SIGINT, pgo_dump_and_exit);
#endif
}

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    port_ppu_store_gate_update();
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
    /* SDL's RawInput driver took this game's controllers and delivered only
     * their triggers: buttons and sticks arrive as window messages it did not
     * get here. Its XInput driver reads everything. Before SDL starts. */
    if (!getenv("SDL_JOYSTICK_RAWINPUT")) _putenv("SDL_JOYSTICK_RAWINPUT=0");
#endif

    printf("=== ps3recomp game runner ===\n");
    pgo_training_hooks();
#if defined(__linux__) && defined(MADV_HUGEPAGE) && defined(MREMAP_FIXED)
    remap_text_huge();
#endif

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    char sfo_path[4096];
    snprintf(sfo_path, sizeof sfo_path, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
    cellGame_init_from_paramsfo(sfo_path);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    port_register_hle_early();  /* overrides of the CRT below: first registration wins */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    port_fs_register_extensions();
    port_register_hle();
    port_register_spu();

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    g_backend_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    WaitForSingleObject(g_backend_ready, INFINITE);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
