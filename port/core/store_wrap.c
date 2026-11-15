/* The fast path of include/port_ppu_memory.h for every translated store.
 *
 * Five hot generated chunks compile their guest stores inline through
 * port_ppu_memory.h; the rest call the runtime's vm_write8/16/32/64
 * (ppu_loader.cpp via loader_bridge.cpp), which check the write watches, the
 * bounds, null-page stores, SPU reservations and the raw SPU registers on every
 * store. Inlining more chunks was measured and rejected (larger executable, more
 * long frames: docs/PERFORMANCE.md). Instead the linker routes the calls here
 * (-Wl,--wrap): one small out-of-line function per width that stores directly
 * under the same conditions as port_ppu_store_fast() and hands everything else
 * -- active diagnostics, low/null, unaligned, out-of-range, raw SPU windows,
 * reserved lines -- to the original accessor. The translated code itself is
 * unchanged, so its GCC profiles still apply.
 *
 * STG02 fighting, interleaved A/B, 3 pairs: 57.4 fps against 56.1, worst 5 s
 * 46.9 against 45.7 (the main thread builds the frame's commands in chunk 020,
 * which has no inline path). The diagnostics, null-sweep and bounds conditions
 * are one compare against port_ppu_store_end (include/port_ppu_store_gate.h):
 * worst 5 s 56.3 against 54.7. */
#include <stdint.h>
extern uint8_t* vm_base;
extern uint32_t port_ppu_store_end;
extern uint32_t g_barrier_sync_watch;
extern const unsigned char* port_ppu_coh_bits;
extern const int* port_ppu_coh_on;
void __real_vm_write8(uint64_t a, uint8_t v);
void __real_vm_write16(uint64_t a, uint16_t v);
void __real_vm_write32(uint64_t a, uint32_t v);
void __real_vm_write64(uint64_t a, uint64_t v);
static inline __attribute__((always_inline)) int store_fast(uint32_t ea, uint32_t n)
{
    if (g_barrier_sync_watch || (ea & (n - 1u)) ||
        ea - 0x10000u >= port_ppu_store_end - 0x10000u) return 0;
    if (__atomic_load_n(port_ppu_coh_on, __ATOMIC_RELAXED)) {
        const uint32_t line = ea >> 7;
        if ((__atomic_load_n(&port_ppu_coh_bits[line >> 3], __ATOMIC_RELAXED) >> (line & 7u)) & 1u) return 0;
    }
    return 1;
}
void __wrap_vm_write8(uint64_t a, uint8_t v) { const uint32_t ea = (uint32_t)a; if (!store_fast(ea, 1)) { __real_vm_write8(a, v); return; } __atomic_store_n(vm_base + ea, v, __ATOMIC_RELAXED); }
void __wrap_vm_write16(uint64_t a, uint16_t v) { const uint32_t ea = (uint32_t)a; if (!store_fast(ea, 2)) { __real_vm_write16(a, v); return; } __atomic_store_n((uint16_t*)(vm_base + ea), __builtin_bswap16(v), __ATOMIC_RELAXED); }
void __wrap_vm_write32(uint64_t a, uint32_t v) { const uint32_t ea = (uint32_t)a; if (!store_fast(ea, 4)) { __real_vm_write32(a, v); return; } __atomic_store_n((uint32_t*)(vm_base + ea), __builtin_bswap32(v), __ATOMIC_RELAXED); }
void __wrap_vm_write64(uint64_t a, uint64_t v) { const uint32_t ea = (uint32_t)a; if (!store_fast(ea, 8)) { __real_vm_write64(a, v); return; } __atomic_store_n((uint64_t*)(vm_base + ea), __builtin_bswap64(v), __ATOMIC_RELAXED); }
