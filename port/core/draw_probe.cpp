/* Who draws what (docs/PLANO0.6.1.md, N3; docs/RENDERER.md).
 *
 * Every draw of the game goes through a few translated functions (the
 * edition's chunk_anchors "draw" in recompile.json). With the build option
 * PORT_DRAW_PROBE, CMake makes each of them weak and puts a wrapper in front
 * that counts the call by its caller's return address, then runs the
 * translated function unchanged. PORT_DRAW_PROBE=<seconds> at run time prints,
 * that often, the calls per second of each function and its busiest callers:
 * which part of the game -- menus and HUD or the 3D scene -- draws through
 * which function, the map a native renderer pass is planned from. */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

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

extern "C" void port_engine_command_hit(unsigned op)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (period() <= 0) return;
    g_ops[op < kOps ? op : kOps]++;
}

extern "C" void port_draw_probe_hit(unsigned fn, unsigned lr)
{
    if (fn >= kMaxFunctions) return;
    std::lock_guard<std::mutex> lock(g_mu);
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
