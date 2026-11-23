/* Native render passes (docs/PLANO0.6.1.md, N3; docs/RENDERER.md).
 *
 * The engine builds its frame as a list of its own render commands
 * (func_0044F468 in BLJS10220) and draws through a few functions that turn
 * the state those commands set into RSX commands. A native pass draws in
 * Vulkan from that state instead. This is stage 1, capture: with the build
 * option PORT_NATIVE_PASS, the engine-command interpreter and the draw
 * functions report here; for the 2D path (menus and HUD) each draw becomes a
 * complete native draw record -- effect, texture (its CellGcmTexture),
 * sampler, vertices copied out of the engine's ring buffer -- and
 * PORT_NATIVE_PASS=capture[:<seconds>] prints what was captured, to check the
 * decoding before anything is drawn from it. Render state (blend, depth,
 * cull) is not tracked: the engine emits it into the RSX command stream as it
 * goes (op5 -> func_004689A0), where the RSX engine sees it.
 *
 * Stage 3, PORT_NATIVE_PASS=hud: a draw of the 2D path with the textured
 * effect (SH_PRETRANS_TEX_COL_P, a DXT texture) is not run as the game's own
 * code. Its record -- triangles in the host's vertex layout, the texture and
 * the sampler -- goes into a host queue, and in the game's RSX command buffer,
 * where the draw's commands would have gone, a NO_OPERATION carrying the
 * record's id. The RSX engine draws the record when it reaches that word
 * (rsx_engine_bridge.c, eng_native_draw), so it lands between the same
 * neighbours with the render state (blend, depth, scissor) the engine had
 * emitted by then. Anything else, or a full command segment, runs the game's
 * draw as before. */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "port_native_pass.h"

extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void vm_write32(uint64_t addr, uint32_t value);

/* The edition's TOC offsets (recompile.json, set by CMake). */
#ifndef PORT_TOC_EFFECT_MANAGER
#error "PORT_TOC_EFFECT_MANAGER: the TOC offset of the effect manager (set by CMake)"
#endif
#ifndef PORT_TOC_PRIMITIVE_TABLE
#error "PORT_TOC_PRIMITIVE_TABLE: the TOC offset of the draw's primitive table (set by CMake)"
#endif

namespace {

/* Engine command opcodes (docs/RENDERER.md). */
enum : unsigned { OP_RENDER_STATE = 5, OP_EFFECT = 32, OP_TEXTURE = 34, OP_SAMPLER = 35 };
constexpr unsigned kVertexBytes = 24;          /* float3 position, RGBA8 colour, float2 UV */
constexpr unsigned kMaxVertices = 4096;

/* CellGcmTexture, 32 bytes: format|mipmap|dimension|cubemap, remap,
 * width << 16 | height, depth << 16 | location << 8, pitch, offset. */
struct Texture { uint32_t object = 0; uint32_t gcm[8] = {}; };

struct Draw {
    std::string effect;
    Texture texture;
    uint32_t sampler[8] = {};
    unsigned prim = 0, first = 0, count = 0;
    std::vector<uint32_t> vertices;              /* count * 6 words, as the guest wrote them */
};

struct Stats {
    unsigned long draws = 0, captured = 0, bad_vertices = 0;
    std::map<std::string, unsigned long> effects;
    std::map<uint32_t, unsigned long> formats;   /* CellGcmTexture format byte */
    std::map<unsigned, unsigned long> prims;
    float min_xy[2] = { 1e30f, 1e30f }, max_xy[2] = { -1e30f, -1e30f };
};

std::mutex g_mu;
int g_mode = -1;                                 /* 0 off, 1 capture, 2 hud (draw natively) */
constexpr unsigned kRing = 4096;                 /* records in flight between the PPU and the RSX engine */
port_native_record* g_ring;                      /* kRing records */
uint32_t g_next_id, g_pending;
unsigned long g_native, g_fallback;
double g_period = 5;
uint32_t g_toc = 0;
std::string g_effect;                            /* the effect op32 selected */
uint32_t g_effect_object = 0;
std::map<uint32_t, Texture> g_textures;          /* by parameter handle (op34) */
std::map<uint32_t, std::map<uint32_t, uint32_t>> g_samplers;   /* handle -> index -> value (op35) */
Draw g_last;
Stats g_stats;
std::chrono::steady_clock::time_point g_t0;

int mode()
{
    if (g_mode < 0) {
        const char* e = std::getenv("PORT_NATIVE_PASS");
        g_mode = 0;
        if (e && !std::strncmp(e, "capture", 7)) {
            g_mode = 1;
            if (e[7] == ':') g_period = std::atof(e + 8);
            if (g_period <= 0) g_period = 5;
        } else if (e && (!std::strcmp(e, "hud") || !std::strcmp(e, "hud-both"))) {
            /* hud-both, a diagnostic: the native draw and the game's own, one over the other */
            g_ring = static_cast<port_native_record*>(std::calloc(kRing, sizeof(port_native_record)));
            g_mode = g_ring ? (e[3] ? 3 : 2) : 0;
        }
        g_t0 = std::chrono::steady_clock::now();
    }
    return g_mode;
}

bool guest_ptr(uint32_t v) { return v >= 0x00010000u && v < 0xD0000000u; }

std::string inline_name(uint32_t ea)             /* a name stored in the object itself */
{
    std::string s;
    for (unsigned i = 0; i < 40; i++) {
        const uint32_t w = vm_read32((ea + i) & ~3u);
        const char c = (char)(w >> (24 - 8 * ((ea + i) & 3)));
        if (!c) break;
        if (c < 0x20 || c > 0x7E) return std::string();
        s += c;
    }
    return s;
}

float as_float(uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }

void report(double secs)
{
    Stats& s = g_stats;
    std::fprintf(stderr, "[native-pass] %.0f 2D draws/s, %.0f captured/s, %lu with vertices off screen;"
                 " positions x %.2f..%.2f y %.2f..%.2f\n", s.draws / secs, s.captured / secs, s.bad_vertices,
                 s.min_xy[0], s.max_xy[0], s.min_xy[1], s.max_xy[1]);
    std::string line = "[native-pass]   effects:";
    for (const auto& [name, n] : s.effects) line += " " + name + " x" + std::to_string((unsigned long)(n / secs));
    std::fprintf(stderr, "%s\n", line.c_str());
    line = "[native-pass]   texture formats:";
    char buf[48];
    for (const auto& [fmt, n] : s.formats) { std::snprintf(buf, sizeof buf, " 0x%02X x%lu", fmt, (unsigned long)(n / secs)); line += buf; }
    line += "; primitives:";
    for (const auto& [p, n] : s.prims) { std::snprintf(buf, sizeof buf, " %u x%lu", p, (unsigned long)(n / secs)); line += buf; }
    std::fprintf(stderr, "%s\n", line.c_str());
    if (!g_last.effect.empty()) {
        const Draw& d = g_last;
        std::fprintf(stderr, "[native-pass]   last: %s, prim %u, %u vertices, texture %08X format 0x%02X %ux%u"
                     " offset %08X; first vertex (%.3f, %.3f, %.3f) colour %08X uv (%.3f, %.3f)\n",
                     d.effect.c_str(), d.prim, d.count, d.texture.object, d.texture.gcm[0] >> 24,
                     d.texture.gcm[2] >> 16, d.texture.gcm[2] & 0xFFFF, d.texture.gcm[5],
                     d.vertices.empty() ? 0.f : as_float(d.vertices[0]), d.vertices.empty() ? 0.f : as_float(d.vertices[1]),
                     d.vertices.empty() ? 0.f : as_float(d.vertices[2]), d.vertices.empty() ? 0u : d.vertices[3],
                     d.vertices.empty() ? 0.f : as_float(d.vertices[4]), d.vertices.empty() ? 0.f : as_float(d.vertices[5]));
    }
    g_stats = Stats();
}

}  // namespace

/* Every engine render command, before the interpreter runs it. */
extern "C" void port_native_engine_command(unsigned op, unsigned record, unsigned toc)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!mode()) return;
    g_toc = toc;
    switch (op) {
    case OP_EFFECT: {
        /* manager *(TOC+PORT_TOC_EFFECT_MANAGER); effect table at +0x5C (count at +0x64); handle = effect << 16 */
        const uint32_t mgr = vm_read32(vm_read32(toc + PORT_TOC_EFFECT_MANAGER));
        const uint32_t handle = vm_read32(record + 8), table = vm_read32(mgr + 0x5C);
        g_effect.clear(); g_effect_object = 0;
        if (guest_ptr(mgr) && guest_ptr(table) && (handle >> 16) < vm_read32(mgr + 0x64)) {
            g_effect_object = vm_read32(table + (handle >> 16) * 4);
            g_effect = inline_name(g_effect_object + 4);
        }
        break;
    }
    case OP_TEXTURE: {
        Texture t;
        t.object = vm_read32(record + 12);
        const uint32_t gcm = guest_ptr(t.object) ? vm_read32(t.object + 0x48) : 0;
        if (guest_ptr(gcm)) for (unsigned i = 0; i < 8; i++) t.gcm[i] = vm_read32(gcm + i * 4);
        g_textures[vm_read32(record + 8)] = t;
        break;
    }
    case OP_SAMPLER:
        g_samplers[vm_read32(record + 4)][vm_read32(record + 8)] = vm_read32(record + 12);
        break;
    default:
        break;
    }
}

namespace {

uint32_t rsx_primitive(unsigned engine_prim)       /* the draw's own table, *(TOC+0x3800) */
{
    const uint32_t table = g_toc ? vm_read32(g_toc + PORT_TOC_PRIMITIVE_TABLE) : 0;
    return guest_ptr(table) && engine_prim < 8 ? vm_read32(table + engine_prim * 4) : 0;
}

void unpack_rgba(uint32_t w, float* out)
{
    out[0] = (w >> 24) / 255.f; out[1] = ((w >> 16) & 255) / 255.f;
    out[2] = ((w >> 8) & 255) / 255.f; out[3] = (w & 255) / 255.f;
}

/* Stage 3: build the record and put its marker in the command stream; false
 * when the game's own draw has to run. */
bool draw_natively(unsigned r3, unsigned r4, unsigned r5, unsigned r6)
{
    if (g_effect != "SH_PRETRANS_TEX_COL_P" || !r6 || r6 > PORT_NATIVE_MAX_VERTS / 2) return false;
    const auto t = g_textures.find(0x8D);         /* the global sampler S_Simple */
    if (t == g_textures.end()) return false;
    const uint32_t fmt = t->second.gcm[0] >> 24;
    if ((fmt & 0x9Fu) != 0x86u && (fmt & 0x9Fu) != 0x87u && (fmt & 0x9Fu) != 0x88u) return false;   /* DXT1/3/5 */
    const uint32_t prim = rsx_primitive(r4);
    if (prim != 5 && prim != 7) return false;    /* triangles, fan */
    /* Only the sprite vertex declaration (device+0xCC): position, colour,
     * texture coordinate in 24 bytes. Others -- the full-screen passes' 20-byte
     * position and texture coordinate -- stay with the game for now. */
    static const uint32_t kSpriteDecl[11] = { 1, 3, 0, 0, 7, 9, 0xC, 0, 8, 2, 0x10 };
    const uint32_t decl = vm_read32(r3 + 0xCC);
    if (!guest_ptr(decl)) return false;
    for (unsigned i = 0; i < 11; i++) if (vm_read32(decl + 4 + i * 4) != kSpriteDecl[i]) return false;
    /* the command segment: device+0x108 -> { begin, end, current } */
    const uint32_t seg = vm_read32(r3 + 0x108);
    if (!guest_ptr(seg)) return false;
    const uint32_t cur = vm_read32(seg + 8), end = vm_read32(seg + 4);
    if (!guest_ptr(cur) || cur + 8 > end) return false;

    const uint32_t id = g_next_id++ & 0xFFFFu;
    port_native_record& rec = g_ring[id % kRing];
    rec.textured = 1;
    const uint32_t* gcm = t->second.gcm;
    rec.tex_format = fmt;
    rec.tex_levels = (gcm[0] >> 16) & 0xFF;
    rec.tex_remap = gcm[1];
    rec.tex_width = gcm[2] >> 16; rec.tex_height = gcm[2] & 0xFFFF;
    rec.tex_location = (gcm[3] >> 8) & 0xFF;
    rec.tex_pitch = gcm[4]; rec.tex_offset = gcm[5];
    const auto& smp = g_samplers[0x8D];
    auto get = [&](uint32_t i, uint32_t dflt) { auto it = smp.find(i); return it == smp.end() ? dflt : it->second; };
    rec.wrap_s = (uint8_t)get(0, 3); rec.wrap_t = (uint8_t)get(1, 3);
    rec.min_linear = get(4, 2) == 2; rec.mag_linear = get(5, 2) == 2;

    const uint32_t base = vm_read32(r3 + 0xDC) + r5 * kVertexBytes;
    auto emit = [&](unsigned i) {
        const uint32_t v = base + i * kVertexBytes;
        float* o = &rec.vertices[(size_t)rec.vertex_count++ * 12];
        o[0] = as_float(vm_read32(v)); o[1] = as_float(vm_read32(v + 4)); o[2] = as_float(vm_read32(v + 8)); o[3] = 1.f;
        unpack_rgba(vm_read32(v + 12), o + 4);
        o[8] = as_float(vm_read32(v + 16)); o[9] = as_float(vm_read32(v + 20)); o[10] = 0.f; o[11] = 1.f;
    };
    rec.vertex_count = 0;
    if (prim == 5) {
        for (unsigned i = 0; i < r6; i++) emit(i);
        rec.vertex_count -= rec.vertex_count % 3;
    } else {
        for (unsigned i = 1; i + 1 < r6; i++) { emit(0); emit(i); emit(i + 1); }
    }
    g_pending = id;
    return true;
}

/* After the game's state flush (func_00455E08: it binds the render target and
 * the rest lazily, at the draw), the marker: NO_OPERATION, count 1, carrying
 * the id, at the segment's write pointer, which moves past it. */
bool commit_marker(unsigned r3)
{
    const uint32_t seg = vm_read32(r3 + 0x108);
    if (!guest_ptr(seg)) return false;
    const uint32_t cur = vm_read32(seg + 8), end = vm_read32(seg + 4);
    if (!guest_ptr(cur) || cur + 8 > end) return false;
    vm_write32(cur, 0x00040100u);
    vm_write32(cur + 4, PORT_NATIVE_MARK | g_pending);
    vm_write32(seg + 8, cur + 8);
    return true;
}

}  // namespace

/* hud: called by the draw wrapper after it ran the game's state flush; false
 * when the marker did not fit and the game's draw must run after all. */
extern "C" int port_native_commit(unsigned r3)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (commit_marker(r3)) return 1;
    g_native--; g_fallback++;
    return 0;
}

extern "C" const port_native_record* port_native_take(uint32_t id)
{
    return g_ring ? &g_ring[(id & 0xFFFFu) % kRing] : nullptr;
}

/* A draw of the 2D path: r3 the render device, r4 primitive, r5 first
 * vertex, r6 count; the vertices at device+0xDC. Returns 1 when it was drawn
 * natively and the game's draw must not run. */
extern "C" int port_native_draw(unsigned r3, unsigned r4, unsigned r5, unsigned r6)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!mode()) return 0;
    if (g_mode >= 2) {
        bool done = draw_natively(r3, r4, r5, r6);
        if (done && g_mode == 3 && !commit_marker(r3)) done = false;   /* hud-both: no flush needed */
        (done ? g_native : g_fallback)++;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - g_t0).count() >= 5) {
            std::fprintf(stderr, "[native-pass] hud: %lu draws native, %lu by the game, in 5 s\n", g_native, g_fallback);
            g_native = g_fallback = 0;
            g_t0 = now;
        }
        return done && g_mode == 2 ? 1 : 0;
    }
    g_stats.draws++;
    if (g_effect != "SH_PRETRANS_TEX_COL_P" || !r6 || r6 > kMaxVertices) {
        g_stats.effects[g_effect.empty() ? std::string("(none)") : g_effect]++;
    } else {
        Draw d;
        d.effect = g_effect;
        d.prim = r4; d.first = r5; d.count = r6;
        const uint32_t sampler_handle = 0x8D;    /* the global sampler S_Simple (docs/RENDERER.md) */
        auto t = g_textures.find(sampler_handle);
        if (t != g_textures.end()) d.texture = t->second;
        for (const auto& [i, v] : g_samplers[sampler_handle]) if (i < 8) d.sampler[i] = v;
        const uint32_t base = vm_read32(r3 + 0xDC) + r5 * kVertexBytes;
        d.vertices.resize((size_t)r6 * (kVertexBytes / 4));
        bool off = false;
        for (unsigned i = 0; i < r6; i++) {
            for (unsigned w = 0; w < kVertexBytes / 4; w++)
                d.vertices[(size_t)i * 6 + w] = vm_read32(base + i * kVertexBytes + w * 4);
            const float x = as_float(d.vertices[(size_t)i * 6]), y = as_float(d.vertices[(size_t)i * 6 + 1]);
            if (!(x > -4 && x < 4 && y > -4 && y < 4)) off = true;
            g_stats.min_xy[0] = std::min(g_stats.min_xy[0], x); g_stats.max_xy[0] = std::max(g_stats.max_xy[0], x);
            g_stats.min_xy[1] = std::min(g_stats.min_xy[1], y); g_stats.max_xy[1] = std::max(g_stats.max_xy[1], y);
        }
        g_stats.bad_vertices += off;
        g_stats.captured++;
        g_stats.effects[d.effect]++;
        g_stats.formats[d.texture.gcm[0] >> 24]++;
        g_stats.prims[d.prim]++;
        g_last = std::move(d);
    }
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - g_t0).count();
    if (secs >= g_period) { report(secs); g_t0 = now; }
    return 0;
}
