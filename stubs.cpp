/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void ps3_load_prx_modules(void)
{
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* BLJS10220 import adapters; all pointer arguments remain guest EAs. */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdint>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
extern "C" {
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
int32_t cellKbGetData(uint32_t, void*);
int32_t cellFontFTInit(const void*, void*);
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
}
static void port_kb_read(ppu_context* c)
{
    c->gpr[3] = (uint64_t)(int64_t)cellKbGetData((uint32_t)c->gpr[3],
        (void*)(uintptr_t)(uint32_t)c->gpr[4]);
}
static void port_font_init(ppu_context* c)
{
    /* Revision is r3; config and library-out are r4/r5. Uses the runtime's
     * fallback font library (not a full FreeType rasterizer). */
    c->gpr[3] = (uint64_t)(int64_t)cellFontFTInit(
        (void*)(uintptr_t)(uint32_t)c->gpr[4],
        (void*)(uintptr_t)(uint32_t)c->gpr[5]);
}
/* The initialize API registers four guest callback OPDs. Attach/detach APIs
 * invoke the corresponding OPD through the runtime's nested guest dispatcher. */
static std::atomic<uint32_t> port_printf_callbacks[4];
static void port_spu_printf_init(ppu_context* c)
{
    for (unsigned i=0;i<4;i++) port_printf_callbacks[i].store((uint32_t)c->gpr[3+i]);
    std::fprintf(stderr, "[port] SPU printf callbacks registered\n");
    c->gpr[3] = 0;
}
static void port_spu_printf_finalize(ppu_context* c)
{
    for (auto& cb:port_printf_callbacks) cb.store(0);
    c->gpr[3] = 0;
}
template<unsigned Slot> static void port_spu_printf_callback(ppu_context* c)
{
    uint32_t opd=port_printf_callbacks[Slot].load();
    c->gpr[3] = opd ? ppu_guest_call(opd,c->gpr[3],0,0,0,0,0,0,0)
                       : (uint64_t)(int64_t)(int32_t)0x8001000Fu;
}
extern "C" int32_t cellSysutilQueueGuestCallbackArgs(uint32_t, const uint64_t[8]);
static std::atomic<uint32_t> port_search_callback{0}, port_search_userdata{0};
static void port_search_init(ppu_context* c)
{
    int32_t error=0;
    uint32_t cb=(uint32_t)c->gpr[5], expected=0;
    if (c->gpr[3]!=0) error=(int32_t)0x8002C804u;
    else if (!cb) error=(int32_t)0x8002C801u;
    else if (!port_search_callback.compare_exchange_strong(expected,cb)) error=(int32_t)0x8002C805u;
    else {
        port_search_userdata.store((uint32_t)c->gpr[6]);
        uint64_t args[8]={1,0,0,c->gpr[6],0,0,0,0};
        error=cellSysutilQueueGuestCallbackArgs(cb,args);
        if (error) port_search_callback.store(0);
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
static void port_search_finalize(ppu_context* c)
{
    uint32_t cb=port_search_callback.load();
    int32_t error=(int32_t)0x8002C806u;
    if (cb) {
        uint64_t args[8]={2,0,0,port_search_userdata.load(),0,0,0,0};
        error=cellSysutilQueueGuestCallbackArgs(cb,args);
        if (!error) port_search_callback.store(0);
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
/* This port has no mounted user-media catalog. List searches therefore
 * complete with a real zero-entry result, asynchronously through sysutil. */
extern "C" void* sys_heap_malloc(uint32_t, uint32_t);
extern "C" int32_t sys_heap_free(uint32_t, void*);
void vm_write32(uint64_t, uint32_t);
static std::atomic<uint32_t> port_search_result{0};
static void port_search_list(ppu_context* c)
{
    int32_t error=(int32_t)0x8002C801u;
    uint32_t out=(uint32_t)c->gpr[5], cb=port_search_callback.load();
    if (!cb) error=(int32_t)0x8002C806u;
    else if (c->gpr[3]>=1 && c->gpr[3]<=9 && (c->gpr[4]==1 || c->gpr[4]==2) && out) {
        if (port_search_result.load()) error=(int32_t)0x8002C802u;
        else {
            uint32_t result=(uint32_t)(uintptr_t)sys_heap_malloc(0,8);
            if (!result) error=(int32_t)0x8002C803u;
            else {
                vm_write32(out,1); vm_write32(result,1); vm_write32(result+4,0);
                uint64_t args[8]={3,0,result,port_search_userdata.load(),0,0,0,0};
                error=cellSysutilQueueGuestCallbackArgs(cb,args);
                if (!error) port_search_result.store(result);
                else sys_heap_free(0,(void*)(uintptr_t)result);
            }
        }
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
static void port_search_end(ppu_context* c)
{
    uint32_t result=port_search_result.load();
    int32_t error=(int32_t)0x8002C80Du;
    if (c->gpr[3]==1 && result) {
        sys_heap_free(0,(void*)(uintptr_t)result);
        port_search_result.store(0); error=0;
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
static std::atomic<uint32_t> port_music_callback{0}, port_music_userdata{0};
static void port_music_init(ppu_context* c)
{
    int32_t error=(int32_t)0x8002C102u;
    if (c->gpr[3]==0 && c->gpr[4]>=16 && c->gpr[4]<=255 && c->gpr[5]) {
        uint64_t args[8]={1,0,c->gpr[6],0,0,0,0,0};
        error=cellSysutilQueueGuestCallbackArgs((uint32_t)c->gpr[5],args);
        if (!error) {
            port_music_callback.store((uint32_t)c->gpr[5]);
            port_music_userdata.store((uint32_t)c->gpr[6]);
        }
    }
    c->gpr[3]=(uint64_t)(int64_t)error;
}
uint32_t vm_read32(uint64_t);
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
extern "C" int32_t cellGameContentErrorDialog(int32_t,int32_t,const char*);
static void port_content_error_trace(ppu_context* c)
{
    std::fprintf(stderr,"[port-data-error] lr=0x%08X r3=%08X r4=%08X r5=%08X\n",
        (uint32_t)c->lr,(uint32_t)c->gpr[3],(uint32_t)c->gpr[4],(uint32_t)c->gpr[5]);
    uint32_t slot=vm_read32(c->gpr[2]-0x3C2C), manager=vm_read32(slot);
    std::fprintf(stderr,"[install-state] toc=%08X slot=%08X manager=%08X flags=%08X\n",
        (uint32_t)c->gpr[2],slot,manager,vm_read32(manager+0x48));
    ppu_dump_guest_stack(c,"content-error");
    c->gpr[3]=(uint64_t)(int64_t)cellGameContentErrorDialog((int32_t)c->gpr[3],
        (int32_t)c->gpr[4],(const char*)(uintptr_t)(uint32_t)c->gpr[5]);
}
/* ListAutoSave differs from FixedSave2 by its error-dialog parameter and
 * ninth PPC64 argument; both use the same fixed-directory callback contract. */
extern "C" int32_t cellSaveDataFixedSave2(uint32_t,void*,void*,void*,void*,void*,uint32_t,void*);
uint64_t vm_read64(uint64_t);
static std::atomic<bool> port_save_overlay{false};
static void port_save_overlay_set(ppu_context* c)
{
    port_save_overlay.store(c->gpr[3]!=0);
}
static void port_save_list_auto(ppu_context* c)
{
    if(c->gpr[4]>2) {c->gpr[3]=(uint64_t)(int64_t)(int32_t)0x8002B404u;return;}
    uint32_t userdata=(uint32_t)vm_read64((uint32_t)c->gpr[1]+112u);
    int32_t result=cellSaveDataFixedSave2((uint32_t)c->gpr[3],
        (void*)(uintptr_t)(uint32_t)c->gpr[5],(void*)(uintptr_t)(uint32_t)c->gpr[6],
        (void*)(uintptr_t)(uint32_t)c->gpr[7],(void*)(uintptr_t)(uint32_t)c->gpr[8],
        (void*)(uintptr_t)(uint32_t)c->gpr[9],(uint32_t)c->gpr[10],(void*)(uintptr_t)userdata);
    std::fprintf(stderr,"[port-save] ListAutoSave -> 0x%08X userdata=0x%08X\n",(uint32_t)result,userdata);
    c->gpr[3]=(uint64_t)(int64_t)result;
}
/* sys_lwcond with real blocking. The SDK models lwcond as a 1 ms poll with
 * no-op signals, which is only correct for waiters that re-check a predicate
 * in a loop. This title's frame event (renderer task: lock; if (!flag)
 * lwcond_wait; unlock) waits exactly once, so the render thread fell through
 * every 1 ms and re-rendered the whole frame dozens of times per flip: that
 * is where the ~19 MiB command arenas and the frame-time spiral came from.
 * Waiters sleep on a host condition variable keyed by the lwcond EA; the
 * paired guest lwmutex is released and re-taken through the SDK's own
 * lwmutex handlers so its ownership bookkeeping stays in one place.
 * PORT_LWCOND_SDK=1 keeps the SDK behaviour. */
extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx);
namespace {
struct PortLwcond { std::mutex m; std::condition_variable cv; uint64_t gen = 0; uint32_t waiters = 0, tokens = 0; };
std::mutex s_lwcond_map_lock;
std::unordered_map<uint32_t, PortLwcond*> s_lwconds;
PortLwcond& port_lwcond(uint32_t ea)
{
    std::lock_guard<std::mutex> g(s_lwcond_map_lock);
    PortLwcond*& c = s_lwconds[ea];
    if (!c) c = new PortLwcond;   /* lives for the process: guests reuse lwcond storage */
    return *c;
}
void port_lwmutex_call(const ppu_context* from, const char* name, uint32_t lwm)
{
    ppu_context call = *from;   /* same thread id: the lwmutex stamps its owner from it */
    call.gpr[3] = lwm; call.gpr[4] = 0;
    ps3_hle_call(ps3_compute_nid(name), &call);
}
}
static void port_lwcond_signal(ppu_context* c)
{
    PortLwcond& lc = port_lwcond((uint32_t)c->gpr[3]);
    { std::lock_guard<std::mutex> g(lc.m); if (lc.waiters > lc.tokens) lc.tokens++; }
    lc.cv.notify_all();
    c->gpr[3] = 0;
}
static void port_lwcond_signal_all(ppu_context* c)
{
    PortLwcond& lc = port_lwcond((uint32_t)c->gpr[3]);
    { std::lock_guard<std::mutex> g(lc.m); lc.gen++; lc.tokens = 0; }
    lc.cv.notify_all();
    c->gpr[3] = 0;
}
static void port_lwcond_wait(ppu_context* c)
{
    const uint32_t ea = (uint32_t)c->gpr[3], lwm = vm_read32(ea);
    const uint64_t timeout_us = c->gpr[4];
    PortLwcond& lc = port_lwcond(ea);
    std::unique_lock<std::mutex> lk(lc.m);
    /* The generation is taken while the guest lwmutex is still held, so a
     * signaller that takes it after the release below cannot be missed. */
    const uint64_t gen = lc.gen;
    lc.waiters++;
    const uint32_t recursion = vm_read32(lwm + 0x0C);
    vm_write32(lwm + 0x0C, 1);
    port_lwmutex_call(c, "sys_lwmutex_unlock", lwm);
    auto woken = [&] { return lc.gen != gen || lc.tokens > 0; };
    bool ok = true;
    if (timeout_us) ok = lc.cv.wait_for(lk, std::chrono::microseconds(timeout_us), woken);
    else while (!lc.cv.wait_for(lk, std::chrono::seconds(10), woken))
        std::fprintf(stderr, "[port-lwcond] tid=%llu still waiting on lwcond %08X (lwmutex %08X)\n",
                     (unsigned long long)c->thread_id, ea, lwm);
    if (ok && lc.gen == gen) lc.tokens--;
    lc.waiters--;
    lk.unlock();
    port_lwmutex_call(c, "sys_lwmutex_lock", lwm);
    vm_write32(lwm + 0x0C, recursion ? recursion : 1);
    c->gpr[3] = ok ? 0 : (uint64_t)(int64_t)(int32_t)0x8001000Bu;   /* ETIMEDOUT */
}
/* Called before ppu_sysprx_register: HLE dispatch takes the first handler
 * registered for a NID, so overrides of the SDK's CRT must come first. */
extern "C" void port_register_hle_early(void)
{
    const char* sdk = std::getenv("PORT_LWCOND_SDK");
    if (sdk && *sdk && *sdk != '0') return;
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal"), "sys_lwcond_signal", port_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_to"), "sys_lwcond_signal_to", port_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_all"), "sys_lwcond_signal_all", port_lwcond_signal_all);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_wait"), "sys_lwcond_wait", port_lwcond_wait);
    std::fprintf(stderr, "[port] blocking sys_lwcond installed\n");
}

/* PORT_PAD_STICK_FILE=<path>: test automation for the left analog stick,
 * which this title moves the player with (the d-pad does nothing in play).
 * The SDK can only hold a stick fixed from launch (PAD_STICK); this rereads
 * "<lx> <ly>" (0-255, 128 centre) from the file on every read the title makes
 * and writes it into the reported data, so a driver outside the process can
 * steer; "<lx> <ly> <rx> <ry>" also sets the right stick (the camera). Off
 * unless the variable is set; only affects reports with sticks. */
extern "C" int32_t cellPadGetData(uint32_t, void*);
static void port_pad_get_data(ppu_context* c)
{
    const uint32_t port = (uint32_t)c->gpr[3], ea = (uint32_t)c->gpr[4];
    const int32_t rc = cellPadGetData(port, (void*)(uintptr_t)ea);
    static const char* path = std::getenv("PORT_PAD_STICK_FILE");
    if (rc == 0 && path && port == 0 && ea && (int32_t)vm_read32(ea) >= 8) {
        if (FILE* f = std::fopen(path, "rb")) {
            int v[4] = {128, 128, 128, 128};   /* lx ly, then optionally rx ry */
            const int n = std::fscanf(f, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
            static const int slot[4] = {6, 7, 4, 5};
            for (int i = 0; i < (n >= 4 ? 4 : n >= 2 ? 2 : 0); i++)
                vm_write16(ea + 4 + slot[i] * 2, (uint16_t)(v[i] < 0 ? 0 : v[i] > 255 ? 255 : v[i]));
            std::fclose(f);
        }
    }
    c->gpr[3] = (uint64_t)(int64_t)rc;
}

/* PORT_PROFILE=<file>: CPU-time sampling profiler that does not stop the
 * game (a debugger attach pauses every thread). A process CPU-time timer
 * raises SIGPROF about PORT_PROFILE_HZ times a second (default 1000); the
 * thread that was running records its id, instruction pointer and the first
 * of the top 32 stack words that points into the executable's code (usually
 * the caller's return address, also from inside libc or the vDSO), and a
 * helper thread appends "<tid> <hex rip> <hex caller>" lines to the file
 * every second (caller 0 when none was found).
 * Symbolise with nm/addr2line against the (non-PIE) executable. */
#ifdef _WIN32
/* Not ported yet: docs/WINDOWS.md, stage 4. */
extern "C" void port_profiler_start(void)
{
    if (std::getenv("PORT_PROFILE")) std::fprintf(stderr, "[port-profile] not available on Windows yet\n");
}
#else
#include <csignal>
#include <ctime>
#include <thread>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace {
struct ProfSample { uint32_t tid; uint64_t rip, tos; };
constexpr uint32_t kProfRing = 1u << 20;
ProfSample* s_prof_ring;
std::atomic<uint32_t> s_prof_head{0};
uintptr_t s_prof_text_lo, s_prof_text_hi;
void port_prof_signal(int, siginfo_t*, void* uc_)
{
    const ucontext_t* uc = static_cast<const ucontext_t*>(uc_);
    const uint32_t i = s_prof_head.fetch_add(1, std::memory_order_relaxed);
    ProfSample& s = s_prof_ring[i & (kProfRing - 1)];
    s.tid = (uint32_t)syscall(SYS_gettid);
    s.rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    const uint64_t* sp = (const uint64_t*)uc->uc_mcontext.gregs[REG_RSP];
    s.tos = 0;
    for (int k = 0; k < 32; k++)
        if (sp[k] >= s_prof_text_lo && sp[k] < s_prof_text_hi) { s.tos = sp[k]; break; }
}
}
extern "C" void port_profiler_start(void)
{
    const char* path = std::getenv("PORT_PROFILE");
    if (!path || !*path) return;
    s_prof_ring = new ProfSample[kProfRing]();
    extern char __executable_start[], etext[];
    s_prof_text_lo = (uintptr_t)__executable_start; s_prof_text_hi = (uintptr_t)etext;
    struct sigaction sa = {};
    sa.sa_sigaction = port_prof_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    const char* hz_s = std::getenv("PORT_PROFILE_HZ");
    long hz = hz_s ? std::atol(hz_s) : 1000; if (hz < 10) hz = 10;
    timer_t t;
    struct sigevent ev = {};
    ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGPROF;
    if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &ev, &t) != 0) { std::perror("[port-profile] timer_create"); return; }
    struct itimerspec its = {};
    its.it_interval.tv_nsec = 1000000000L / hz; its.it_value = its.it_interval;
    timer_settime(t, 0, &its, nullptr);
    std::string out = path;
    std::thread([out] {
        uint32_t tail = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint32_t head = s_prof_head.load(std::memory_order_relaxed);
            if (head - tail > kProfRing) tail = head - kProfRing;
            if (FILE* f = std::fopen(out.c_str(), "a")) {
                for (; tail != head; tail++) {
                    const ProfSample& s = s_prof_ring[tail & (kProfRing - 1)];
                    std::fprintf(f, "%u %lx %lx\n", s.tid, (unsigned long)s.rip, (unsigned long)s.tos);
                }
                std::fclose(f);
            }
        }
    }).detach();
    std::fprintf(stderr, "[port-profile] sampling at %ld Hz of process CPU time -> %s\n", hz, path);
}
#endif /* !_WIN32 */

extern "C" void port_register_sail(void);
extern "C" void port_register_hle(void)
{
    port_profiler_start();
    if (std::getenv("PORT_PAD_STICK_FILE"))
        ps3_hle_register_ctx(0x8B72CDA1u, "cellPadGetData", port_pad_get_data);
    port_register_sail();
    ps3_hle_register_ctx(0xE7FA820Bu,"cellSaveDataEnableOverlay",port_save_overlay_set);
    ps3_hle_register_ctx(0x4DD03A4Eu,"cellSaveDataListAutoSave",port_save_list_auto);
    ps3_hle_register_ctx(0xB0A1F8C6u,"cellGameContentErrorDialog",port_content_error_trace);
    ps3_hle_register_ctx(0xFF0A21B7u, "cellKbRead", port_kb_read);
    ps3_hle_register_ctx(0x7A0A83C4u, "cellFontInitLibraryFreeTypeWithRevision", port_font_init);
    ps3_hle_register_ctx(0x45FE2FCEu, "_sys_spu_printf_initialize", port_spu_printf_init);
    ps3_hle_register_ctx(0xDD3B27ACu, "_sys_spu_printf_finalize", port_spu_printf_finalize);
    ps3_hle_register_ctx(0xDD0C1E09u, "_sys_spu_printf_attach_group", port_spu_printf_callback<0>);
    ps3_hle_register_ctx(0x5FDFB2FEu, "_sys_spu_printf_detach_group", port_spu_printf_callback<1>);
    ps3_hle_register_ctx(0x1AE10B92u, "_sys_spu_printf_attach_thread", port_spu_printf_callback<2>);
    ps3_hle_register_ctx(0xB3BBCF2Au, "_sys_spu_printf_detach_thread", port_spu_printf_callback<3>);
    ps3_hle_register_ctx(0x0A4C8295u, "cellSearchStartListSearch", port_search_list);
    ps3_hle_register_ctx(0x774033D6u, "cellSearchEnd", port_search_end);
    ps3_hle_register_ctx(0xBE50B11Eu, "cellMusicInitialize2", port_music_init);
    ps3_hle_register_ctx(0xC81CCF8Au, "cellSearchInitialize", port_search_init);
    ps3_hle_register_ctx(0xBFAB7616u, "cellSearchFinalize", port_search_finalize);
    std::fprintf(stderr, "[port] keyboard/font/SPU printf/search lifecycle adapters installed\n");
}

/* Before the blocking sys_lwcond above, the render thread redrew the opening
 * mission's frame dozens of times per flip and filled >18 MiB, running the
 * title's two 4 MiB command arenas into its CPU render metadata. This gives
 * empty contexts separate mapped storage instead. With the lwcond fix a
 * mission frame peaks near 0.5 MiB, so the launcher leaves it off; it stays
 * as an opt-in (PORT_GCM_EXPAND_BUFFERS=1) safety net. */
extern "C" int64_t sys_memory_allocate(ppu_context*);
extern "C" int64_t sys_memory_free(ppu_context*);
extern "C" int32_t cellGcmMapMainMemory(uint32_t,uint32_t,uint32_t*);
/* The title's two command-arena contexts, for PORT_GCM_FRAME_STATS in
 * gcm_bridge.c. Recorded whether or not the arenas are relocated. */
extern "C" { uint32_t port_arena_ctx[2] = {0, 0}; }
extern "C" void port_expand_graphics_buffers(ppu_context* original, uint32_t renderer)
{
    const uint32_t a=vm_read32(renderer+0x100), b=vm_read32(renderer+0x104);
    if (a && b) { port_arena_ctx[0]=a; port_arena_ctx[1]=b; }
    const char* enabled=std::getenv("PORT_GCM_EXPAND_BUFFERS");
    if (!enabled || !*enabled || *enabled=='0') return;
    if (!a || !b || vm_read32(a+8)!=vm_read32(a) || vm_read32(b+8)!=vm_read32(b) ||
        vm_read32(a+4)-vm_read32(a)!=0x3fffc0u ||
        vm_read32(b+4)-vm_read32(b)!=0x3fffc0u) return;
    const uint32_t size=0x2000000u, out=(uint32_t)original->gpr[1]+0x30;
    const uint32_t saved=vm_read32(out);
    ppu_context call=*original;call.gpr[3]=2*size;call.gpr[4]=0x400;call.gpr[5]=out;
    if (sys_memory_allocate(&call)!=0) {
        std::fprintf(stderr,"[port-buffers] cannot allocate 64 MiB for RSX commands; contexts unchanged\n");
        vm_write32(out,saved);return;
    }
    const uint32_t base=vm_read32(out);
    if (cellGcmMapMainMemory(base,2*size,(uint32_t*)(uintptr_t)out)!=0) {
        std::fprintf(stderr,"[port-buffers] cannot map RSX commands; allocation released\n");
        call.gpr[3]=base;sys_memory_free(&call);vm_write32(out,saved);return;
    }
    const uint32_t io=vm_read32(out);vm_write32(out,saved);
    for (unsigned i=0;i<2;i++) {
        uint32_t c=i?b:a, begin=base+i*size;
        uint32_t old=vm_read32(c);
        /* The initial FIFO jump was emitted before the contexts existed.
         * Preserve that entry point as a forwarding jump to the new stream. */
        vm_write32(old,0x20000000u | (io+i*size));
        vm_write32(c,begin);vm_write32(c+4,begin+size-64);vm_write32(c+8,begin);
    }
    std::fprintf(stderr,"[port-buffers] contexts %08X/%08X relocated to %08X, 32 MiB each, IO %08X\n",a,b,base,io);
}
