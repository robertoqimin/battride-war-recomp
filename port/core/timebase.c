/* The guest timebase (mftb) from the CPU's time-stamp counter.
 *
 * The SDK's ppu_timebase_now() scales clock_gettime(CLOCK_MONOTONIC) to the
 * PS3's 79.8 MHz. With the kernel's clock source on HPET (the 0.6.0 test
 * laptop booted that way) every read is a slow hardware access: 11-13% of the
 * runTaskMain thread was in the timebase reads of the game's frame limiter
 * and timers (docs/PERFORMANCE.md). On a CPU whose TSC runs at a constant rate
 * in every power state (CPUID 0x80000007 EDX bit 8, "invariant TSC") the
 * linker routes the translated code's reads here (-Wl,--wrap) and they become
 * one rdtsc and a multiply, calibrated once against CLOCK_MONOTONIC.
 *
 * The first calibration (20 ms) is good to about 1e-4; once a second the rate
 * is measured again over the whole time since then and the counter rebased on
 * CLOCK_MONOTONIC, so the guest clock stays within a fraction of a millisecond
 * of the one the OS sleeps, audio and movies use.
 *
 * PORT_TIMEBASE=os keeps the SDK's path; so does a CPU without an invariant
 * TSC. Each thread never sees the value go back (a guard against any skew
 * between cores' counters and the rebasing). Linux on x86-64 only; Windows'
 * QueryPerformanceCounter is already this cheap. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PS3_TIMEBASE_FREQ 79800000ULL

uint64_t __real_ppu_timebase_now(void);

static int      s_mode = -1;        /* -1 unknown, 0 the SDK's clock, 1 TSC */
/* tb = tb0 + (tsc - tsc0) * mult >> 32. Two copies: the refinement writes the
 * one readers are not using, then publishes it (it runs once a second, so no
 * reader still holds the older one). */
struct params { uint64_t tsc0, tb0, mult, next; };
static struct params s_params[2];
static int      s_current;
static uint64_t s_cal_tsc, s_cal_ns; /* the first calibration's start: the base of every later one */
static uint64_t s_tsc_hz_guess;
static __thread uint64_t t_last;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int invariant_tsc(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000007u)
        return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000007u), "c"(0));
    return (d >> 8) & 1u;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The SDK clock's value at CLOCK_MONOTONIC time ns. */
static uint64_t tb_of_ns(uint64_t ns)
{
    return ns / 1000000000ull * PS3_TIMEBASE_FREQ + ns % 1000000000ull * PS3_TIMEBASE_FREQ / 1000000000ull;
}

/* The multiplier for a counter of hz ticks per second (exposed for the test). */
uint64_t port_timebase_mult(double hz)
{
    return (uint64_t)((double)PS3_TIMEBASE_FREQ * 4294967296.0 / hz);
}

static void calibrate(void)
{
    const char* e = getenv("PORT_TIMEBASE");
    if ((e && !strcmp(e, "os")) || !invariant_tsc()) {
        s_mode = 0;
        return;
    }
    /* 20 ms against CLOCK_MONOTONIC, each end read between two TSC reads. */
    uint64_t a0 = rdtsc(), n0 = mono_ns(), a1 = rdtsc();
    const struct timespec wait = {0, 20 * 1000 * 1000};
    nanosleep(&wait, NULL);
    uint64_t b0 = rdtsc(), n1 = mono_ns(), b1 = rdtsc();
    const double hz = (double)((b0 + b1) / 2 - (a0 + a1) / 2) * 1e9 / (double)(n1 - n0);
    if (!(hz > 1e8 && hz < 2e10)) {     /* nothing sensible: keep the OS clock */
        s_mode = 0;
        return;
    }
    s_cal_tsc = (a0 + a1) / 2;
    s_cal_ns = n0;
    s_tsc_hz_guess = (uint64_t)hz;
    struct params* p = &s_params[0];
    p->mult = port_timebase_mult(hz);
    /* The same epoch as the SDK's clock, so values before and after match. */
    p->tsc0 = (b0 + b1) / 2;
    p->tb0 = tb_of_ns(n1);
    p->next = p->tsc0 + s_tsc_hz_guess;   /* refine after a second */
    __atomic_store_n(&s_current, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mode, 1, __ATOMIC_RELEASE);
}

/* Once a second: the rate over the whole time since calibration, rebased on the OS clock. */
static void refine(const struct params* old)
{
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        return;                         /* another thread is at it */
    uint64_t a = rdtsc(), n = mono_ns(), b = rdtsc();
    const uint64_t tsc = (a + b) / 2;
    const double hz = (double)(tsc - s_cal_tsc) * 1e9 / (double)(n - s_cal_ns);
    const int next = !__atomic_load_n(&s_current, __ATOMIC_RELAXED);
    struct params* p = &s_params[next];
    if (hz > 1e8 && hz < 2e10) {
        p->mult = port_timebase_mult(hz);
        p->tsc0 = tsc;
        p->tb0 = tb_of_ns(n);
    } else {
        *p = *old;
    }
    p->next = tsc + s_tsc_hz_guess;
    __atomic_store_n(&s_current, next, __ATOMIC_RELEASE);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

uint64_t __wrap_ppu_timebase_now(void)
{
    int mode = __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
    if (__builtin_expect(mode < 0, 0)) {
        static int once;                /* first caller calibrates; others use the OS clock meanwhile */
        if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL))
            calibrate();
        mode = __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
    }
    if (mode != 1)
        return __real_ppu_timebase_now();
    const struct params* p = &s_params[__atomic_load_n(&s_current, __ATOMIC_ACQUIRE)];
    const uint64_t now = rdtsc();
    if (__builtin_expect((int64_t)(now - p->next) >= 0, 0)) {
        refine(p);
        p = &s_params[__atomic_load_n(&s_current, __ATOMIC_ACQUIRE)];
    }
    /* Signed: a core whose counter trails the calibrating one's by a few cycles
     * must read the epoch, not wrap to the far future. */
    const int64_t sd = (int64_t)(now - p->tsc0);
    const uint64_t d = sd > 0 ? (uint64_t)sd : 0;
    uint64_t tb = p->tb0 + (uint64_t)(((unsigned __int128)d * p->mult) >> 32);
    if (__builtin_expect(tb < t_last, 0))
        tb = t_last;
    t_last = tb;
    return tb;
}
