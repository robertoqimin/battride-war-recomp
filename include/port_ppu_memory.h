#pragma once
/* Port-local fast paths for selected generated PPU translation units only.
 * Keep ppu_recomp.h unmodified and leave the SDK runtime's accessors intact. */
#include <stdint.h>
#if defined(__GNUC__) && (!defined(_WIN32) || defined(__MINGW32__))
/* GCC left these small helpers out of line in several very large generated
 * functions. Retain their existing diagnostics, MMIO and bounds predicates. */
static inline uint8_t vm_read8_inl(uint64_t) __attribute__((always_inline));
static inline uint16_t vm_read16_inl(uint64_t) __attribute__((always_inline));
static inline uint32_t vm_read32_inl(uint64_t) __attribute__((always_inline));
static inline uint64_t vm_read64_inl(uint64_t) __attribute__((always_inline));

#endif
#include "ppu_recomp.h"

#if defined(__GNUC__) && (!defined(_WIN32) || defined(__MINGW32__))

extern "C" {
extern int port_ppu_store_inline_ok;
extern uint32_t g_null_sweep_hi;
extern uint32_t g_barrier_sync_watch;
extern const unsigned char* port_ppu_coh_bits;
extern const int* port_ppu_coh_on;
}

static inline __attribute__((always_inline)) int port_ppu_store_fast(uint32_t ea, uint32_t n)
{
    /* Aligned 1/2/4/8-byte stores cannot cross a 128-byte reservation line.
     * All raw SPU windows, low/null addresses, unaligned stores, bounds errors
     * and active diagnostics are delegated to the original accessor. */
    if (!port_ppu_store_inline_ok || g_null_sweep_hi || g_barrier_sync_watch ||
        ea < 0x10000u || ea >= 0xE0000000u || (ea & (n - 1u)) ||
        (ppu_vm_size && (uint64_t)ea + n > ppu_vm_size)) return 0;
    if (__atomic_load_n(port_ppu_coh_on, __ATOMIC_RELAXED)) {
        const uint32_t line = ea >> 7;
        if ((__atomic_load_n(&port_ppu_coh_bits[line >> 3], __ATOMIC_RELAXED) >> (line & 7u)) & 1u)
            return 0;
    }
    return 1;
}

static inline __attribute__((always_inline)) void port_vm_write8(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (!port_ppu_store_fast(ea, 1)) { vm_write8(a, v); return; }
    __atomic_store_n(vm_base + ea, v, __ATOMIC_RELAXED);
}
static inline __attribute__((always_inline)) void port_vm_write16(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (!port_ppu_store_fast(ea, 2)) { vm_write16(a, v); return; }
    __atomic_store_n((uint16_t*)(vm_base + ea), __builtin_bswap16(v), __ATOMIC_RELAXED);
}
static inline __attribute__((always_inline)) void port_vm_write32(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (!port_ppu_store_fast(ea, 4)) { vm_write32(a, v); return; }
    __atomic_store_n((uint32_t*)(vm_base + ea), __builtin_bswap32(v), __ATOMIC_RELAXED);
}
static inline __attribute__((always_inline)) void port_vm_write64(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (!port_ppu_store_fast(ea, 8)) { vm_write64(a, v); return; }
    __atomic_store_n((uint64_t*)(vm_base + ea), __builtin_bswap64(v), __ATOMIC_RELAXED);
}

#define vm_write8(a, v) port_vm_write8((a), (v))
#define vm_write16(a, v) port_vm_write16((a), (v))
#define vm_write32(a, v) port_vm_write32((a), (v))
#define vm_write64(a, v) port_vm_write64((a), (v))
#endif
