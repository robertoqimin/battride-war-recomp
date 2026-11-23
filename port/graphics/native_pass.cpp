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
#include <algorithm>
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
std::map<std::string, unsigned long> g_why;      /* hud: why draws stayed with the game */
const char* g_reason = "";
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
    static char why[64];
    g_reason = why;
    /* The effect does not decide anything here: the RSX engine picks the
     * shader by the fragment program the game's flush binds (op32's effect
     * is stale for the full-screen passes, which choose theirs elsewhere). */
    static const char* only = std::getenv("PORT_NATIVE_EFFECTS");   /* diagnostic: limit to these effect names */
    if (only && (g_effect.empty() || !std::strstr(only, g_effect.c_str()))) {
        std::snprintf(why, sizeof why, "effect %s", g_effect.c_str());
        return false;
    }
    if (!r6 || r6 > PORT_NATIVE_MAX_VERTS) { std::snprintf(why, sizeof why, "vertex count %s", r6 > 1024 ? ">1024" : r6 > 512 ? ">512" : ">256"); return false; }
    const uint32_t prim = rsx_primitive(r4);
    if (prim < 5 || prim > 7) { std::snprintf(why, sizeof why, "primitive %u", prim); return false; }   /* triangles, strip, fan */
    if (prim != 5 && (r6 < 3 || (r6 - 2) * 3 > PORT_NATIVE_MAX_VERTS)) { g_reason = "vertex count (strip, fan)"; return false; }
    /* The vertex declaration (device+0xCC): after the object's first word,
     * elements of { usage, type, offset, 0 } up to usage 0. Usage 1 position
     * (type 3, float3), 7 colour (type 9 or 5, four bytes the RSX reads as
     * UB4: the sprites' and the filters'), 8 texture coordinate (type 2,
     * float2). Sprites: 24 bytes in a ring buffer in main memory; full-screen
     * copies: position and coordinate in 20 bytes in RSX memory; filters:
     * all three in 24 bytes. */
    const uint32_t decl = vm_read32(r3 + 0xCC);
    if (!guest_ptr(decl)) { g_reason = "no vertex declaration"; return false; }
    int pos = -1, col = -1, uv = -1;
    uint32_t stride = 0;
    for (unsigned e = 0; e < 8; e++) {
        const uint32_t w = decl + 4 + e * 16;
        const uint32_t usage = vm_read32(w), type = vm_read32(w + 4), off = vm_read32(w + 8);
        if (!usage) break;
        if (usage == 1 && type == 3) { pos = (int)off; stride = std::max(stride, off + 12); }
        else if (usage == 7 && (type == 9 || type == 5)) { col = (int)off; stride = std::max(stride, off + 4); }
        else if (usage == 8 && type == 2) { uv = (int)off; stride = std::max(stride, off + 8); }
        else {
            static std::map<std::string, int> shown;
            std::snprintf(why, sizeof why, "vertex element %u type %u", usage, type);
            if (!shown[why]++) std::fprintf(stderr, "[native-pass] %s (effect %s)\n", why, g_effect.c_str());
            return false;
        }
    }
    if (pos < 0) { g_reason = "no position"; return false; }
    const uint32_t vbase = vm_read32(r3 + 0xDC);
    if (!guest_ptr(vbase)) { g_reason = "no vertices"; return false; }
    /* the command segment: device+0x108 -> { begin, end, current } */
    const uint32_t seg = vm_read32(r3 + 0x108);
    if (!guest_ptr(seg)) return false;
    const uint32_t cur = vm_read32(seg + 8), end = vm_read32(seg + 4);
    if (!guest_ptr(cur) || cur + 8 > end) { g_reason = "segment full"; return false; }

    const uint32_t id = g_next_id++ & 0xFFFFu;
    port_native_record& rec = g_ring[id % kRing];
    rec.vertex_colour = col >= 0;
    std::snprintf(rec.effect, sizeof rec.effect, "%s", g_effect.c_str());
    const uint32_t base = vbase + r5 * stride;
    auto emit = [&](unsigned i) {
        const uint32_t v = base + i * stride;
        float* o = &rec.vertices[(size_t)rec.vertex_count++ * 12];
        o[0] = as_float(vm_read32(v + pos)); o[1] = as_float(vm_read32(v + pos + 4)); o[2] = as_float(vm_read32(v + pos + 8)); o[3] = 1.f;
        if (col >= 0) unpack_rgba(vm_read32(v + col), o + 4);
        else o[4] = o[5] = o[6] = o[7] = 1.f;    /* the engine puts the RSX's current value here */
        if (uv >= 0) { o[8] = as_float(vm_read32(v + uv)); o[9] = as_float(vm_read32(v + uv + 4)); }
        else o[8] = o[9] = 0.f;
        o[10] = 0.f; o[11] = 1.f;
    };
    rec.vertex_count = 0;
    if (prim == 5) {
        for (unsigned i = 0; i < r6; i++) emit(i);
        rec.vertex_count -= rec.vertex_count % 3;
    } else if (prim == 6) {                      /* strip: every other triangle turned back to the same winding */
        for (unsigned i = 0; i + 2 < r6; i++) {
            if (i & 1) { emit(i + 1); emit(i); emit(i + 2); } else { emit(i); emit(i + 1); emit(i + 2); }
        }
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

/* PORT_NATIVE_PROBE=<n>: after the first n draws of each effect the game
 * kept, a probe marker for the RSX engine to describe what the draw used
 * (program, textures, constants, target). The draw's vertex declaration
 * is printed here. */
namespace { std::vector<std::string> g_probe_names; std::map<std::string, unsigned> g_probe_count; bool g_kept; }

extern "C" const char* port_native_probe_name(uint32_t index)
{
    return index < g_probe_names.size() ? g_probe_names[index].c_str() : nullptr;
}

extern "C" void port_native_after(unsigned r3)
{
    static int limit = -1;
    if (limit < 0) { const char* e = std::getenv("PORT_NATIVE_PROBE"); limit = e ? std::atoi(e) : 0; }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!limit || !g_kept || g_effect.empty()) return;
    g_kept = false;
    if (g_probe_count[g_effect]++ >= (unsigned)limit) return;
    unsigned index = 0;
    while (index < g_probe_names.size() && g_probe_names[index] != g_effect) index++;
    if (index == g_probe_names.size()) g_probe_names.push_back(g_effect);
    const uint32_t decl = vm_read32(r3 + 0xCC);
    char words[200]; int n = 0;
    for (unsigned k = 1; k < 20 && guest_ptr(decl); k++) n += std::snprintf(words + n, sizeof words - n, " %X", vm_read32(decl + k * 4));
    const uint32_t seg = vm_read32(r3 + 0x108);
    const uint32_t cur = guest_ptr(seg) ? vm_read32(seg + 8) : 0, end = guest_ptr(seg) ? vm_read32(seg + 4) : 0;
    std::fprintf(stderr, "[native-probe] %s #%u: decl%s, vertices at %08X; device %08X segment %08X at %08X end %08X\n",
                 g_effect.c_str(), g_probe_count[g_effect], words, vm_read32(r3 + 0xDC), r3, seg, cur, end);
    if (!guest_ptr(cur) || cur + 8 > end) return;
    vm_write32(cur, 0x00040100u);
    vm_write32(cur + 4, PORT_NATIVE_PROBE_MARK | index);
    vm_write32(seg + 8, cur + 8);
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
        if (!done) g_why[g_reason]++;
        g_kept = !done;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - g_t0).count() >= 5) {
            std::string line;
            for (const auto& [why, n] : g_why) line += "; " + why + " x" + std::to_string(n);
            std::fprintf(stderr, "[native-pass] hud: %lu draws native, %lu by the game, in 5 s%s\n", g_native, g_fallback,
                         line.c_str());
            g_native = g_fallback = 0;
            g_why.clear();
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
