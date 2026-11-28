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

/* The 3D scene's draw (func_005B4718 in BLJS10220): one batch of a mesh,
 * its vertex streams and its 16-bit index buffer in RSX memory, drawn as
 * triangles. PORT_NATIVE_3D=check: after the game's own draw, a
 * NO_OPERATION carrying PORT_NATIVE_MESH_MARK | slot, for the engine to
 * check the decoding against what the draw set. */
#define PORT_NATIVE_MESH_MARK 0xBA7C0000u
/* PORT_NATIVE_3D=draw: in place of the game's draw, NO_OPERATION carrying
 * PORT_NATIVE_MESH_DRAW_MARK | slot; the RSX engine draws the batch. */
#define PORT_NATIVE_MESH_DRAW_MARK 0xBA7B0000u
#define PORT_NATIVE_MESH_STREAMS 16u

typedef struct port_native_stream {
    uint8_t attr, type, size, stride;  /* SET_VERTEX_DATA_ARRAY_FORMAT's fields */
    uint32_t offset;                   /* SET_VERTEX_DATA_ARRAY_OFFSET: location in bit 31 */
} port_native_stream;

typedef struct port_native_mesh {
    uint32_t n_streams;
    port_native_stream streams[PORT_NATIVE_MESH_STREAMS];
    uint32_t attr_mask;                /* the streams the pass draws: all, or 0x83 for a depth-only pass */
    uint32_t index_offset, index_count;
    uint32_t batch;                    /* the batch object, for diagnostics */
} port_native_mesh;

const port_native_mesh* port_native_mesh_take(uint32_t slot);

/* The effect name a probe marker carries, or 0. Engine side. */
const char* port_native_probe_name(uint32_t index);

/* The record a marker names, or 0 when it is gone. Engine side. */
const port_native_record* port_native_take(uint32_t id);

#ifdef __cplusplus
}
#endif
