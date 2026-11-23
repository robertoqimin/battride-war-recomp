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
/* PORT_NATIVE_PROBE: NO_OPERATION carrying PORT_NATIVE_PROBE_MARK | effect
 * index, after a draw the game made itself, for the engine to describe. */
#define PORT_NATIVE_PROBE_MARK 0xBA7D0000u

typedef struct port_native_record {
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

/* The effect name a probe marker carries, or 0. Engine side. */
const char* port_native_probe_name(uint32_t index);

/* The record a marker names, or 0 when it is gone. Engine side. */
const port_native_record* port_native_take(uint32_t id);

#ifdef __cplusplus
}
#endif
