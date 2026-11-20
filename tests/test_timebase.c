/* port/core/timebase.c: the TSC timebase keeps the SDK clock's epoch and rate,
 * never goes back within a thread, and PORT_TIMEBASE=os keeps the SDK path. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TB 79800000ULL
uint64_t __wrap_ppu_timebase_now(void);
uint64_t port_timebase_mult(double hz);
static int os_calls;

/* The SDK's ppu_timebase_now (runtime/syscalls/sys_timer.c, Linux). */
uint64_t __real_ppu_timebase_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    __atomic_add_fetch(&os_calls, 1, __ATOMIC_RELAXED);
    return (uint64_t)ts.tv_sec * TB + (uint64_t)ts.tv_nsec * TB / 1000000000ull;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static void* reader(void* arg)
{
    (void)arg;
    uint64_t last = 0;
    for (int i = 0; i < 2000000; i++) {
        const uint64_t v = __wrap_ppu_timebase_now();
        if (v < last) { CHECK(0, "went back in a thread: %llu < %llu", (unsigned long long)v, (unsigned long long)last); break; }
        last = v;
    }
    return NULL;
}

int main(void)
{
    /* 32.32 fixed point: 79.8 MHz from a 3 GHz counter. */
    const uint64_t m = port_timebase_mult(3e9);
    CHECK(fabs((double)m / 4294967296.0 - 79.8e6 / 3e9) < 1e-9, "multiplier %llu", (unsigned long long)m);

    if (getenv("PORT_TIMEBASE")) {      /* PORT_TIMEBASE=os run */
        __wrap_ppu_timebase_now();
        CHECK(os_calls > 0, "PORT_TIMEBASE=os did not use the SDK clock");
        return failures != 0;
    }
    /* Same epoch and rate as the SDK clock: agree now, and three seconds later. */
    const uint64_t a_tsc = __wrap_ppu_timebase_now(), a_os = __real_ppu_timebase_now();
    /* Past a second, so the refinement has run at least once. */
    const struct timespec wait = {1, 500 * 1000 * 1000};
    nanosleep(&wait, NULL);
    __wrap_ppu_timebase_now();
    nanosleep(&wait, NULL);
    const uint64_t b_tsc = __wrap_ppu_timebase_now(), b_os = __real_ppu_timebase_now();
    const double off_ms = fabs((double)a_tsc - (double)a_os) / TB * 1e3;
    const double rate = (double)(b_tsc - a_tsc) / (double)(b_os - a_os);
    printf("timebase: offset %.3f ms, rate %.6f\n", off_ms, rate);
    CHECK(off_ms < 1.0, "epoch differs by %.3f ms", off_ms);
    CHECK(fabs(rate - 1.0) < 2e-5, "rate %.6f", rate);
    const double end_ms = fabs((double)b_tsc - (double)b_os) / TB * 1e3;
    printf("timebase: offset after 3 s %.4f ms\n", end_ms);
    CHECK(end_ms < 0.1, "drifted %.4f ms from the OS clock", end_ms);

    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, reader, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    if (!failures) puts("timebase: ok");
    return failures != 0;
}
