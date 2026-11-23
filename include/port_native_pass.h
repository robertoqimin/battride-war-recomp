#pragma once
/* The native 2D pass's record, shared by port/graphics/native_pass.cpp (the
 * PPU side, which fills it at the game's draw) and rsx_engine_bridge.c (the
 * RSX engine, which draws it where its marker sits in the command stream).
 * docs/RENDERER.md. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NV4097 NO_OPERATION (method 0x100) carrying PORT_NATIVE_MARK | record id. */
#define PORT_NATIVE_MARK      0xBA7E0000u
#define PORT_NATIVE_MAX_VERTS 256u

typedef struct port_native_record {
    uint32_t textured;                 /* 0: colour only */
    /* the texture, from its CellGcmTexture */
    uint32_t tex_location, tex_offset, tex_format, tex_width, tex_height;
    uint32_t tex_levels, tex_pitch, tex_remap;
    /* the sampler (engine op35): wrap U/V as RSX wrap modes, linear filters */
    uint8_t wrap_s, wrap_t, min_linear, mag_linear;
    /* triangles, already expanded from the engine's primitive: 3 float4 a
     * vertex (position, colour, texture coordinate), attributes 0, 3 and 8 */
    uint32_t vertex_count;
    float vertices[PORT_NATIVE_MAX_VERTS * 12];
} port_native_record;

/* The record a marker names, or 0 when it is gone. Engine side. */
const port_native_record* port_native_take(uint32_t id);

#ifdef __cplusplus
}
#endif
