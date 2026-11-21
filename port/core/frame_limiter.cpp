/* Time spent in the game's frame limiter (func_003ABE20 in BLJS10220), for the busy_ms
 * column of PORT_FRAME_TIMES (docs/PLANO0.6.1.md, F1).
 *
 * The game caps itself at 60 fps: the limiter sleeps and spins until the
 * frame's deadline. Below the cap it returns at once; at the cap the wait
 * hides how much faster a build is, so frame rates of different builds all
 * read about 60. busy_ms = frame interval - time in the limiter is the work
 * the frame took, and 1000 / busy_ms the rate the build could reach.
 *
 * include/port_frame_limiter.h makes the lifted definition weak; this one
 * takes every call, runs the lifted function unchanged and adds its duration
 * to a counter that the frame logger in main.cpp takes at each guest flip. */
#include "ppu_recomp.h"
#include <atomic>
#include <chrono>
#include <cstdint>

/* Its name in this edition's lift (recompile.json, set by CMake). */
#ifndef PORT_FN_FRAME_LIMITER
#error "PORT_FN_FRAME_LIMITER: the lifted name of the game's frame limiter (set by CMake)"
#endif

void port_lifted_frame_limiter(ppu_context* ctx);

namespace {
std::atomic<int64_t> g_limiter_wait_ns{0};
}

/* The limiter's time since the previous call, in milliseconds; resets it. */
extern "C" double port_frame_limiter_take_wait_ms(void)
{
    return (double)g_limiter_wait_ns.exchange(0, std::memory_order_relaxed) / 1e6;
}

void PORT_FN_FRAME_LIMITER(ppu_context* ctx)
{
    const auto start = std::chrono::steady_clock::now();
    port_lifted_frame_limiter(ctx);
    const auto waited = std::chrono::steady_clock::now() - start;
    g_limiter_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count(),
                                std::memory_order_relaxed);
}
