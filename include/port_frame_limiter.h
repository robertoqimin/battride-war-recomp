#pragma once
/* Force-included into the generated chunk that defines the game's own frame
 * limiter (it sleeps whole milliseconds, then spins on the timebase to the
 * frame deadline; docs/PERFORMANCE.md). The lifted definition becomes weak,
 * so the call goes to port/core/frame_limiter.cpp, which times the wait and
 * calls the lifted function under port_lifted_frame_limiter. Generated files
 * and ppu_recomp.h are unchanged.
 *
 * PORT_FN_FRAME_LIMITER is its name in this edition's lift (func_003ABE20 in
 * BLJS10220), set by CMake from recompile.json. */
#include "ppu_recomp.h"

#ifndef PORT_FN_FRAME_LIMITER
#error "PORT_FN_FRAME_LIMITER: the lifted name of the game's frame limiter (set by CMake)"
#endif
#define PORT_STR_(x) #x
#define PORT_STR(x) PORT_STR_(x)

void PORT_FN_FRAME_LIMITER(ppu_context* ctx) __attribute__((weak));
void port_lifted_frame_limiter(ppu_context* ctx)
    __attribute__((alias("_Z13" PORT_STR(PORT_FN_FRAME_LIMITER) "P11ppu_context")));
