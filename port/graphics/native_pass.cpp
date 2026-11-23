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
 * Nothing here changes what the game draws yet. */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" uint32_t vm_read32(uint64_t addr);

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
int g_mode = -1;                                 /* 0 off, 1 capture */
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
        /* manager *(TOC+0x3790); effect table at +0x5C (count at +0x64); handle = effect << 16 */
        const uint32_t mgr = vm_read32(vm_read32(toc + 0x3790));
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

/* A draw of the 2D path: r3 the render device, r4 primitive, r5 first
 * vertex, r6 count; the vertices at device+0xDC. */
extern "C" void port_native_draw(unsigned r3, unsigned r4, unsigned r5, unsigned r6)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!mode()) return;
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
}
