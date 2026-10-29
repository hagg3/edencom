// FT.15a: heat/battery instruments for interactive sessions (WORKING/thermal-power-plan-2026-10-28.md §4.1).
//   --power-log=N   one `[eden-power]` line to stderr every N s (fps, frame p50/p90, CPU, thermal, battery).
//   --power-exp=a,b lever switches, each off by default (off = today's behaviour, byte-identical):
//                   menu30 (the others were shipped or dropped in FT.15d).
// Zero cost when neither flag is given: power_frame() returns on its first test.
#include "harness.h"
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace {
int    g_exp = 0;
std::string g_expText;
double g_logSecs = 0.0;
double g_lastMs = 0.0, g_winStartMs = 0.0, g_cpuStart = 0.0;
std::vector<double> g_dt;

double cpu_seconds() {
#if defined(_WIN32)
    return 0.0;   // no CPU column on Windows; the lever legs are iOS/macOS
#else
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u) != 0) return 0.0;
    return (double)u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + (double)u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
#endif
}
double pct(std::vector<double>& v, double f) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))];
}
}  // namespace

extern "C" int eden_power_exp(int bit) { return (g_exp & bit) != 0; }   // also read by gl_context_native.cpp

void power_parse_exp(const char* list) {
    g_expText = list;
    for (const char* p = list; *p; ) {
        const char* e = std::strchr(p, ',');
        const size_t n = e ? (size_t)(e - p) : std::strlen(p);
        auto is = [&](const char* s) { return n == std::strlen(s) && !std::strncmp(p, s, n); };
        if (is("menu30")) g_exp |= EDEN_PEXP_MENU30;
        else std::fprintf(stderr, "[eden-power] unknown --power-exp item '%.*s'\n", (int)n, p);
        if (!e) break;
        p = e + 1;
    }
}

void power_log_set(double secs) { g_logSecs = secs; }

// 0 menu, 1 play, 2 wait (loading); -1 before the world exists.
int power_game_mode() { return World::getWorld ? World::getWorld->game_mode : -1; }

// Once per loop iteration, at the top (so dt is the whole frame including pacing).
void power_frame() {
    if (g_logSecs <= 0.0) return;
    const double now = eden_platform_now_ms();
    if (g_lastMs == 0.0) { g_lastMs = g_winStartMs = now; g_cpuStart = cpu_seconds(); return; }
    g_dt.push_back(now - g_lastMs);
    g_lastMs = now;
    const double span = now - g_winStartMs;
    if (span < g_logSecs * 1000.0) return;
    const double cpu = cpu_seconds();
    const int mode = power_game_mode();
    int batt = -1;
    SDL_GetPowerInfo(nullptr, &batt);
    std::fprintf(stderr, "[eden-power] t=%.0f mode=%s mp=%d frames=%zu fps=%.1f p50_ms=%.1f p90_ms=%.1f "
                 "cpu_pct=%.0f thermal=%d batt=%d scale=%d cap=%d exp=%s\n",
                 now * 0.001, mode == 1 ? "play" : mode == 2 ? "wait" : "menu", MpWorld::active() ? 1 : 0,
                 g_dt.size(), (double)g_dt.size() * 1000.0 / span, pct(g_dt, 0.5), pct(g_dt, 0.9),
                 (cpu - g_cpuStart) * 100000.0 / span, eden_native_thermal_state(), batt,
                 eden_get_render_scale_pct(), eden_get_fps_cap(), g_expText.empty() ? "-" : g_expText.c_str());
    std::fflush(stderr);
    g_dt.clear();
    g_winStartMs = now;
    g_cpuStart = cpu;
}
