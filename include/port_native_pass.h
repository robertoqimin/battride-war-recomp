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

/* The engine effects the pass draws, each with the fragment program of the
 * guest's own (docs/RENDERER.md): */
enum {
    PORT_NATIVE_TEX_COL = 0,           /* SH_PRETRANS_TEX_COL_P: texture x colour */
    PORT_NATIVE_COL = 1,               /* SH_PRETRANS_COL_P: colour */
    PORT_NATIVE_TEX_COL_MUL = 2,       /* SH_PRETRANS_TEX_COL_MUL_P: tex.a (1 - tex.rgb) col.rgb col.a, and tex to target 1 */
    PORT_NATIVE_TEX = 3,               /* SH_PRETRANS_TEX_P: texture */
    PORT_NATIVE_KINDS
};

typedef struct port_native_record {
    uint32_t kind;                     /* PORT_NATIVE_* */
    uint32_t textured;                 /* 0: colour only */
    uint32_t vertex_colour;            /* 0: no colour attribute (the full-screen passes): the RSX's current value */
    char effect[32];                   /* the engine effect's name, for diagnostics */
    /* The texture and its sampler are not here: the game's state flush,
     * which runs before the marker, binds them to texture unit 0, and the
     * RSX engine takes them from there like any draw of its own. */
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
