/* The SDK's SPU reservation tracking (runtime/spu/spu_coherency.c), unchanged,
 * plus a way to read its bitmap without a call. Every guest store checks it
 * (loader_bridge.cpp, VM_WRITE_COH), and as an out-of-line call it was a few
 * percent of the game thread's time in crowded fights. */
#include PORT_SPU_COHERENCY_SOURCE

const unsigned char* port_spu_coh_bitmap(void) { return s_coh_bitmap; }
const int* port_spu_coh_armed(void) { return &s_coh_armed; }
