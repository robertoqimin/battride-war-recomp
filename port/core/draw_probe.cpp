/* Who draws what (docs/PLANO0.6.1.md, N3; docs/RENDERER.md).
 *
 * Every draw of the game goes through a few translated functions (the
 * edition's chunk_anchors "draw" in recompile.json). With the build option
 * PORT_DRAW_PROBE, CMake makes each of them weak and puts a wrapper in front
 * that counts the call by its caller's return address, then runs the
 * translated function unchanged. PORT_DRAW_PROBE=<seconds> at run time prints,
 * that often, the calls per second of each function and its busiest callers:
 * which part of the game -- menus and HUD or the 3D scene -- draws through
 * which function, the map a native renderer pass is planned from.
 *
 * PORT_DRAW_DUMP=<n>: the first n calls of each draw function log their
 * arguments (r3 the engine's render device, r4..r6) and the device's first
 * 0x200 bytes, and for each word there that points into guest memory, the
 * first 0x40 bytes it points at -- to decode the device (docs/RENDERER.md). */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdint>

extern "C" uint32_t vm_read32(uint64_t addr);

namespace {
constexpr unsigned kMaxFunctions = 8;
std::mutex g_mu;
std::unordered_map<unsigned, unsigned long> g_callers[kMaxFunctions];
unsigned long g_calls[kMaxFunctions];
constexpr unsigned kOps = 64;
unsigned long g_ops[kOps + 1];   /* the engine's render commands by opcode; the last: out of range */
double g_period = -1;
std::chrono::steady_clock::time_point g_last;

double period()
{
    if (g_period < 0) {
        const char* e = std::getenv("PORT_DRAW_PROBE");
        g_period = e ? std::atof(e) : 0;
        if (e && g_period <= 0) g_period = 5;
        g_last = std::chrono::steady_clock::now();
    }
    return g_period;
}

void report(double secs)
{
    for (unsigned f = 0; f < kMaxFunctions; f++) {
        if (!g_calls[f]) continue;
        std::vector<std::pair<unsigned long, unsigned>> top;
        for (const auto& [lr, n] : g_callers[f]) top.emplace_back(n, lr);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "[draw-probe] draw function %u: %.0f calls/s from %zu call sites:", f,
                     g_calls[f] / secs, top.size());
        for (size_t i = 0; i < top.size() && i < 8; i++)
            std::fprintf(stderr, " %08X x%.0f/s", top[i].second, top[i].first / secs);
        std::fprintf(stderr, "\n");
        g_callers[f].clear();
        g_calls[f] = 0;
    }
    unsigned long total = 0;
    for (unsigned op = 0; op <= kOps; op++) total += g_ops[op];
    if (total) {
        std::fprintf(stderr, "[draw-probe] engine commands: %.0f/s:", total / secs);
        for (unsigned op = 0; op <= kOps; op++)
            if (g_ops[op]) std::fprintf(stderr, " op%u x%.0f/s", op, g_ops[op] / secs);
        std::fprintf(stderr, "\n");
        for (auto& n : g_ops) n = 0;
    }
}
}  // namespace


static bool guest_ptr(uint32_t v)
{
    /* main memory, the heap the game allocates from, RSX local memory */
    return (v >= 0x00010000u && v < 0x0F000000u) || (v >= 0x10000000u && v < 0x40000000u) ||
           (v >= 0xC0000000u && v < 0xD0000000u);
}

static void dump_words(std::string& out, const char* what, uint32_t ea, unsigned bytes)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "[draw-dump]   %s %08X:", what, ea);
    out += buf;
    for (unsigned o = 0; o < bytes; o += 4) {
        if (o && o % 32 == 0) {
            std::snprintf(buf, sizeof buf, "\n[draw-dump]   %*s +%03X:", (int)std::strlen(what), "", o);
            out += buf;
        }
        std::snprintf(buf, sizeof buf, " %08X", vm_read32(ea + o));
        out += buf;
    }
    out += "\n";
}

/* The first n calls from each call site, each dump written in one piece. */
static void dump_draw(unsigned fn, unsigned lr, unsigned r3, unsigned r4, unsigned r5, unsigned r6)
{
    static int limit = -1;
    static std::unordered_map<unsigned, unsigned> done;
    if (limit < 0) { const char* e = std::getenv("PORT_DRAW_DUMP"); limit = e ? std::atoi(e) : 0; }
    if (!limit || !guest_ptr(r3)) return;
    unsigned& n = done[lr];
    if (n >= (unsigned)limit) return;
    n++;
    std::string out;
    char buf[160];
    std::snprintf(buf, sizeof buf, "[draw-dump] draw function %u call %u from %08X: r3=%08X r4=%08X r5=%08X r6=%08X\n",
                  fn, n, lr, r3, r4, r5, r6);
    out += buf;
    dump_words(out, "device", r3, 0x200);
    for (unsigned o = 0; o < 0x200; o += 4) {
        const uint32_t v = vm_read32(r3 + o);
        if (v == r3 || !guest_ptr(v) || v >= 0xC0000000u) continue;
        char what[32];
        std::snprintf(what, sizeof what, "device+%03X ->", o);
        dump_words(out, what, v, 0x40);
    }
    std::fputs(out.c_str(), stderr);
}

/* A C string at ea, if it looks like one. */
static std::string guest_string(uint32_t ea)
{
    std::string s;
    if (!guest_ptr(ea)) return s;
    for (unsigned i = 0; i < 48; i++) {
        const uint32_t w = vm_read32((ea + i) & ~3u);
        const unsigned char c = (unsigned char)(w >> (24 - 8 * ((ea + i) & 3)));
        if (!c) return s.size() >= 3 ? s : std::string();
        if (c < 0x20 || c > 0x7E) return std::string();
        s += (char)c;
    }
    return s;
}

/* The effect-manager operations (30..41) carry a handle: high 16 bits an
 * effect in the manager's table (manager at *(TOC+0x3790), table at +0x5C),
 * low 16 bits a parameter in the effect's table (+0x70). With PORT_DRAW_DUMP,
 * the first records of each such operation are logged with the effect and
 * parameter they name, and any string those objects point to. */
static void dump_effect_op(unsigned op, unsigned record, unsigned toc)
{
    static int limit = -1;
    static unsigned done[kOps];
    if (limit < 0) { const char* e = std::getenv("PORT_DRAW_DUMP"); limit = e ? std::atoi(e) : 0; }
    if (!limit || op < 30 || op > 41 || done[op] >= (unsigned)limit * 4) return;
    done[op]++;
    std::string out;
    char buf[200];
    const uint32_t handle = vm_read32(record + 8);
    const uint32_t mgr = vm_read32(vm_read32(toc + 0x3790));
    std::snprintf(buf, sizeof buf, "[effect-dump] op%u record %08X: %08X %08X %08X %08X  manager %08X handle %08X\n",
                  op, record, vm_read32(record), vm_read32(record + 4), handle, vm_read32(record + 12), mgr, handle);
    out += buf;
    if (guest_ptr(mgr)) {
        const uint32_t table = vm_read32(mgr + 0x5C), count = vm_read32(mgr + 0x64);
        const uint32_t ei = handle >> 16, pi = handle & 0xFFFF;
        if (guest_ptr(table) && ei < count) {
            const uint32_t effect = vm_read32(table + ei * 4);
            dump_words(out, "effect", effect, 0x80);
            for (unsigned o = 0; o < 0x80; o += 4) {
                const std::string s = guest_string(vm_read32(effect + o));
                if (!s.empty()) { std::snprintf(buf, sizeof buf, "[effect-dump]   effect+%02X -> \"%s\"\n", o, s.c_str()); out += buf; }
            }
            const uint32_t ptable = vm_read32(effect + 0x70), pcount = vm_read32(effect + 0x78);
            if (guest_ptr(ptable) && pi < pcount) {
                const uint32_t param = vm_read32(ptable + pi * 4);
                dump_words(out, "param", param, 0x40);
                for (unsigned o = 0; o < 0x40; o += 4) {
                    const std::string s = guest_string(vm_read32(param + o));
                    if (!s.empty()) { std::snprintf(buf, sizeof buf, "[effect-dump]   param+%02X -> \"%s\"\n", o, s.c_str()); out += buf; }
                }
            }
        }
    }
    /* op34 binds a texture: its value (record +0xC) is the texture object. */
    const uint32_t value = vm_read32(record + 12);
    if (op == 34 && guest_ptr(value)) {
        dump_words(out, "texture", value, 0x60);
        for (unsigned o = 0; o < 0x60; o += 4) {
            const uint32_t v = vm_read32(value + o);
            if (guest_ptr(v) && v < 0xC0000000u && v != value) {
                char what[32];
                std::snprintf(what, sizeof what, "texture+%02X ->", o);
                dump_words(out, what, v, 0x40);
            }
        }
    }
    std::fputs(out.c_str(), stderr);
}

extern "C" void port_engine_command_hit(unsigned op, unsigned record, unsigned toc)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (period() <= 0) return;
    g_ops[op < kOps ? op : kOps]++;
    dump_effect_op(op, record, toc);
}

extern "C" void port_draw_probe_hit(unsigned fn, unsigned lr, unsigned r3, unsigned r4, unsigned r5, unsigned r6)
{
    if (fn >= kMaxFunctions) return;
    std::lock_guard<std::mutex> lock(g_mu);
    dump_draw(fn, lr, r3, r4, r5, r6);
    const double p = period();
    if (p <= 0) return;
    g_calls[fn]++;
    g_callers[fn][lr]++;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - g_last).count();
    if (secs >= p) {
        report(secs);
        g_last = now;
    }
}
