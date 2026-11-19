#pragma once
/* Force-included into the generated chunk that defines func_003ABE20, the
 * game's own frame limiter (it sleeps whole milliseconds, then spins on the
 * timebase to the frame deadline; docs/PERFORMANCE.md). The lifted definition
 * becomes weak, so the call goes to port/core/frame_limiter.cpp, which times
 * the wait and calls the lifted function under port_lifted_func_003ABE20.
 * Generated files and ppu_recomp.h are unchanged. */
#include "ppu_recomp.h"

void func_003ABE20(ppu_context* ctx) __attribute__((weak));
void port_lifted_func_003ABE20(ppu_context* ctx)
    __attribute__((alias("_Z13func_003ABE20P11ppu_context")));
