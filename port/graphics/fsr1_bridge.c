/* AMD FidelityFX Super Resolution 1.0 for the window present: the two
 * compute passes' SPIR-V (fsr1_spv.h, from shaders/fsr1.comp) and their
 * constants, computed with AMD's own setup functions compiled for the CPU.
 * Kept out of vulkan_bridge.c so ffx_a.h's short macro names stay here.
 * MIT licensed, see third_party/fsr1/LICENSE.txt. */
#include <math.h>      /* ffx_a.h's CPU path expects these */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsr1_spv.h"

#define A_CPU 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"   /* AMD's header, unmodified */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../third_party/fsr1/ffx_a.h"
#include "../../third_party/fsr1/ffx_fsr1.h"
#pragma GCC diagnostic pop

const uint32_t* port_fsr1_easu_code(size_t* bytes) { *bytes = sizeof fsr1_easu_spv; return fsr1_easu_spv; }
const uint32_t* port_fsr1_rcas_code(size_t* bytes) { *bytes = sizeof fsr1_rcas_spv; return fsr1_rcas_spv; }

/* EASU push constants (four uvec4) for scaling an in_w x in_h image to
 * out_w x out_h. */
void port_fsr1_easu_constants(uint32_t out[16], uint32_t in_w, uint32_t in_h,
                              uint32_t out_w, uint32_t out_h)
{
    varAU4(con0); varAU4(con1); varAU4(con2); varAU4(con3);
    FsrEasuCon(con0, con1, con2, con3, (AF1)in_w, (AF1)in_h, (AF1)in_w, (AF1)in_h,
               (AF1)out_w, (AF1)out_h);
    memcpy(out, con0, 16); memcpy(out + 4, con1, 16);
    memcpy(out + 8, con2, 16); memcpy(out + 12, con3, 16);
}

/* RCAS push constants; sharpness in stops, 0 the strongest. */
void port_fsr1_rcas_constants(uint32_t out[4], float sharpness)
{
    varAU4(con);
    FsrRcasCon(con, (AF1)sharpness);
    memcpy(out, con, 16);
}
