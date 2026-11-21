#pragma once
/* Force-included into the generated chunk that defines each function
 * ppu_native.cpp replaces. The lifted definition becomes weak, so the
 * linker sends every call -- from other chunks and from its own -- to the
 * native one, and stays reachable under port_lifted_native_lookup for
 * PORT_PPU_NATIVE=check. Generated files and ppu_recomp.h are unchanged.
 *
 * PORT_FN_NATIVE_LOOKUP is the function's name in this edition's lift
 * (func_004B5120 in BLJS10220), set by CMake from recompile.json. */
#include "ppu_recomp.h"

#ifndef PORT_FN_NATIVE_LOOKUP
#error "PORT_FN_NATIVE_LOOKUP: the lifted name of the native lookup (set by CMake)"
#endif
#define PORT_STR_(x) #x
#define PORT_STR(x) PORT_STR_(x)

void PORT_FN_NATIVE_LOOKUP(ppu_context* ctx) __attribute__((weak));
void port_lifted_native_lookup(ppu_context* ctx)
    __attribute__((alias("_Z13" PORT_STR(PORT_FN_NATIVE_LOOKUP) "P11ppu_context")));
