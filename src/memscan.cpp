#include "memscan.h"
#include "camera_override.h"
#include "logger.h"

#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace memscan {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxCandidates = 200000;

struct Candidate { float* addr; float original; };
std::vector<Candidate> g_cands;      // current candidate list (scan / refine)
std::vector<float> g_snapshot;       // values at last snapshot, parallel to g_cands

// Our own image range: never scan or poke ourselves.
const char* g_self_lo = nullptr;
const char* g_self_hi = nullptr;
const char* g_game_lo = nullptr;
const char* g_game_hi = nullptr;

void resolve_self()
{
    if (g_self_lo) return;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(&resolve_self), &mbi, sizeof(mbi))) {
        const auto* base = static_cast<const char*>(mbi.AllocationBase);
        g_self_lo = base;
        __try {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            g_self_hi = base + nt->OptionalHeader.SizeOfImage;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_self_hi = base + 0x400000; }
    }
    const HMODULE exe = GetModuleHandleW(nullptr);
    g_game_lo = reinterpret_cast<const char*>(exe);
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_game_lo + dos->e_lfanew);
        g_game_hi = g_game_lo + nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_game_hi = g_game_lo; }
}

bool writable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Visit every committed writable region (excluding our own DLL).
template <typename F>
void for_each_region(F&& fn)
{
    resolve_self();
    SYSTEM_INFO si; GetSystemInfo(&si);
    const char* p = static_cast<const char*>(si.lpMinimumApplicationAddress);
    const char* end = static_cast<const char*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi = {};
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        const char* base = static_cast<const char*>(mbi.BaseAddress);
        const char* next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && writable(mbi.Protect) && mbi.RegionSize <= (512u << 20) &&
            !(base >= g_self_lo && base < g_self_hi)) {
            fn(base, mbi.RegionSize);
        }
        if (next <= p) break;
        p = next;
    }
}

// Scan one region for floats in [lo, hi]; SEH-guarded (a page can vanish).
void scan_region(const char* base, size_t size, float lo, float hi)
{
    __try {
        const float* f = reinterpret_cast<const float*>(base);
        const size_t n = size / sizeof(float);
        for (size_t i = 0; i < n && g_cands.size() < kMaxCandidates; ++i) {
            const float v = f[i];
            if (v >= lo && v <= hi) g_cands.push_back({ const_cast<float*>(f + i), v });
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

size_t scan_range(float lo, float hi)
{
    g_cands.clear();
    for_each_region([&](const char* base, size_t size) { scan_region(base, size, lo, hi); });
    return g_cands.size();
}

bool read_float(const float* p, float& out)
{
    __try { out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_float(float* p, float v)
{
    __try { *p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void refine(bool (*keep)(float now, float before, float a, float b), float a, float b)
{
    std::vector<Candidate> kept;
    for (size_t i = 0; i < g_cands.size(); ++i) {
        float now;
        if (!read_float(g_cands[i].addr, now)) continue;
        const float before = (i < g_snapshot.size()) ? g_snapshot[i] : g_cands[i].original;
        if (keep(now, before, a, b)) kept.push_back({ g_cands[i].addr, now });
    }
    g_cands.swap(kept);
    g_snapshot.clear();
}

void snapshot()
{
    g_snapshot.resize(g_cands.size());
    for (size_t i = 0; i < g_cands.size(); ++i) {
        float v; g_snapshot[i] = read_float(g_cands[i].addr, v) ? v : g_cands[i].original;
    }
}

const char* where(const void* p)
{
    const char* c = static_cast<const char*>(p);
    if (c >= g_game_lo && c < g_game_hi) return "exe";
    return "heap";
}

// ---- locks and watches ----
struct Lock { float* addr; float value; };
std::vector<Lock> g_locks;
std::vector<float*> g_watches;

// ---- FOV hunt ----
struct HuntSet { const char* name; float value; float factor; };   // factor applied to the ORIGINAL when poking
struct HuntCand { float* addr; float original; int set; };
struct HuntHit  { float* addr; float original; int set; float th_before, tv_before, th_after, tv_after; };

std::vector<HuntSet>  g_hunt_sets;
std::vector<HuntCand> g_hunt;
std::vector<HuntHit>  g_hits;
size_t   g_hunt_i = 0;
int      g_hunt_phase = -1;       // -1 idle, 0 write, 1..N wait, N+1 check
float    g_hunt_factor = 1.3f;
float    g_base_th = 0.0f, g_base_tv = 0.0f;
unsigned g_hunt_frames_total = 0;
constexpr int kHuntWait = 3;
constexpr size_t kHuntPerSet = 160;

void hunt_begin(float factor)
{
    float th, tv;
    if (!camover::world_tangents(th, tv)) { VRLOG("[hunt] no world projection yet - load into a level first"); return; }
    g_hunt_factor = factor;
    g_hunt_sets.clear();
    g_hunt.clear();
    g_hits.clear();
    const float deg_h = 2.0f * std::atan(th) * 180.0f / kPi, deg_v = 2.0f * std::atan(tv) * 180.0f / kPi;
    const float rad_h = deg_h * kPi / 180.0f, rad_v = deg_v * kPi / 180.0f;
    g_hunt_sets = {
        { "deg_v",      deg_v,        factor },
        { "deg_h",      deg_h,        factor },
        { "halfdeg_v",  deg_v * 0.5f, factor },
        { "halfdeg_h",  deg_h * 0.5f, factor },
        { "rad_v",      rad_v,        factor },
        { "rad_h",      rad_h,        factor },
        { "halfrad_v",  rad_v * 0.5f, factor },
        { "halfrad_h",  rad_h * 0.5f, factor },
        { "tan_v",      tv,           factor },
        { "tan_h",      th,           factor },
        { "proj_b",     1.0f / tv,    1.0f / factor },
        { "proj_a",     1.0f / th,    1.0f / factor },
        { "ini55",      55.0f,        factor },
        { "ini90",      90.0f,        factor },
    };
    VRLOG("[hunt] tangents h=%.4f v=%.4f -> deg %.2f x %.2f; scanning %zu value sets (eps 0.3%%)...",
          th, tv, deg_h, deg_v, g_hunt_sets.size());
    for (size_t s = 0; s < g_hunt_sets.size(); ++s) {
        const float v = g_hunt_sets[s].value;
        const float eps = std::fabs(v) * 0.003f + 1e-5f;
        scan_range(v - eps, v + eps);
        size_t added = 0;
        for (const Candidate& c : g_cands) {
            if (added >= kHuntPerSet) break;
            g_hunt.push_back({ c.addr, c.original, static_cast<int>(s) });
            ++added;
        }
        VRLOG("[hunt]  set %-10s value %10.4f: %zu matches (%zu taken)", g_hunt_sets[s].name, v, g_cands.size(), added);
    }
    g_cands.clear();
    g_hunt_i = 0;
    g_hunt_phase = 0;
    g_hunt_frames_total = 0;
    VRLOG("[hunt] %zu candidates; poking each for %d frames (~%.0f s at 60 fps). Results as [hunt] HIT lines.",
          g_hunt.size(), kHuntWait + 2, g_hunt.size() * (kHuntWait + 2) / 60.0f);
}

void hunt_tick()
{
    if (g_hunt_phase < 0) return;
    if (g_hunt_i >= g_hunt.size()) {
        g_hunt_phase = -1;
        VRLOG("[hunt] DONE: %zu hits of %zu candidates over %u frames", g_hits.size(), g_hunt.size(), g_hunt_frames_total);
        for (const HuntHit& h : g_hits) {
            VRLOG("[hunt]  HIT %p (%s) set=%s orig=%.4f  tangents %.4f/%.4f -> %.4f/%.4f",
                  h.addr, where(h.addr), g_hunt_sets[h.set].name, h.original, h.th_before, h.tv_before, h.th_after, h.tv_after);
        }
        return;
    }
    ++g_hunt_frames_total;
    HuntCand& c = g_hunt[g_hunt_i];
    if (g_hunt_phase == 0) {
        camover::world_tangents(g_base_th, g_base_tv);
        float cur;
        if (!read_float(c.addr, cur) || std::fabs(cur - c.original) > std::fabs(c.original) * 0.01f + 1e-5f) {
            // Value moved since the scan - not a stable setting; skip.
            ++g_hunt_i; return;
        }
        write_float(c.addr, c.original * g_hunt_sets[c.set].factor);
        g_hunt_phase = 1;
        return;
    }
    if (g_hunt_phase <= kHuntWait) { ++g_hunt_phase; return; }

    // Check and restore.
    float th, tv;
    const bool have = camover::world_tangents(th, tv);
    write_float(c.addr, c.original);
    if (have && g_base_th > 0.0f &&
        (std::fabs(th / g_base_th - 1.0f) > 0.05f || std::fabs(tv / g_base_tv - 1.0f) > 0.05f)) {
        g_hits.push_back({ c.addr, c.original, c.set, g_base_th, g_base_tv, th, tv });
        VRLOG("[hunt] HIT %p (%s) set=%s orig=%.4f: tangents %.4f/%.4f -> %.4f/%.4f",
              c.addr, where(c.addr), g_hunt_sets[c.set].name, c.original, g_base_th, g_base_tv, th, tv);
    }
    ++g_hunt_i;
    g_hunt_phase = 0;
    if (g_hunt_i % 100 == 0) VRLOG("[hunt] %zu/%zu candidates tried, %zu hits so far", g_hunt_i, g_hunt.size(), g_hits.size());
}

float* parse_addr(const char* s)
{
    if (!s) return nullptr;
    return reinterpret_cast<float*>(static_cast<uintptr_t>(std::strtoull(s, nullptr, 16)));
}

} // namespace

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    char a1[64] = {}, a2[64] = {};
    if (args) sscanf_s(args, "%63s %63s", a1, static_cast<unsigned>(sizeof(a1)), a2, static_cast<unsigned>(sizeof(a2)));
    const bool has1 = a1[0] != 0, has2 = a2[0] != 0;

    if (!strcmp(cmd, "scan")) {
        if (!has2) { _snprintf_s(reply, n, _TRUNCATE, "usage: scan <lo> <hi>"); return true; }
        const size_t c = scan_range(static_cast<float>(atof(a1)), static_cast<float>(atof(a2)));
        _snprintf_s(reply, n, _TRUNCATE, "scan: %zu candidates in [%s, %s]%s", c, a1, a2, c >= kMaxCandidates ? " (CAPPED)" : "");
        return true;
    }
    if (!strcmp(cmd, "scanv")) {
        if (!has1) { _snprintf_s(reply, n, _TRUNCATE, "usage: scanv <value> [eps]"); return true; }
        const float v = static_cast<float>(atof(a1));
        const float eps = has2 ? static_cast<float>(atof(a2)) : std::fabs(v) * 0.003f + 1e-5f;
        const size_t c = scan_range(v - eps, v + eps);
        _snprintf_s(reply, n, _TRUNCATE, "scanv: %zu candidates near %.5f (eps %.5f)", c, v, eps);
        return true;
    }
    if (!strcmp(cmd, "refine")) {
        if (!has1) { _snprintf_s(reply, n, _TRUNCATE, "usage: refine <value> [eps]"); return true; }
        const float v = static_cast<float>(atof(a1));
        const float eps = has2 ? static_cast<float>(atof(a2)) : std::fabs(v) * 0.003f + 1e-5f;
        refine([](float now, float, float a, float b) { return std::fabs(now - a) <= b; }, v, eps);
        _snprintf_s(reply, n, _TRUNCATE, "refine: %zu candidates remain", g_cands.size());
        return true;
    }
    if (!strcmp(cmd, "snapshot")) { snapshot(); _snprintf_s(reply, n, _TRUNCATE, "snapshot of %zu candidates", g_cands.size()); return true; }
    if (!strcmp(cmd, "changed")) {
        refine([](float now, float before, float, float) { return std::fabs(now - before) > 1e-6f; }, 0, 0);
        _snprintf_s(reply, n, _TRUNCATE, "changed: %zu candidates remain", g_cands.size()); return true;
    }
    if (!strcmp(cmd, "unchanged")) {
        refine([](float now, float before, float, float) { return std::fabs(now - before) <= 1e-6f; }, 0, 0);
        _snprintf_s(reply, n, _TRUNCATE, "unchanged: %zu candidates remain", g_cands.size()); return true;
    }
    if (!strcmp(cmd, "list")) {
        const size_t lim = has1 ? static_cast<size_t>(atoi(a1)) : 32;
        size_t shown = 0;
        for (const Candidate& c : g_cands) {
            if (shown++ >= lim) break;
            float v; const bool ok = read_float(c.addr, v);
            VRLOG("[scan]  %p (%s) = %.6f (was %.6f)", c.addr, where(c.addr), ok ? v : 0.0f, c.original);
        }
        _snprintf_s(reply, n, _TRUNCATE, "list: %zu of %zu shown in log", shown < lim ? shown : lim, g_cands.size());
        return true;
    }
    if (!strcmp(cmd, "poke")) {
        float* p = parse_addr(a1);
        if (!p || !has2) { _snprintf_s(reply, n, _TRUNCATE, "usage: poke <hexaddr> <float>"); return true; }
        const bool ok = write_float(p, static_cast<float>(atof(a2)));
        _snprintf_s(reply, n, _TRUNCATE, "poke %p = %s: %s", p, a2, ok ? "ok" : "FAILED"); return true;
    }
    if (!strcmp(cmd, "lock")) {
        float* p = parse_addr(a1);
        if (!p || !has2) { _snprintf_s(reply, n, _TRUNCATE, "usage: lock <hexaddr> <float>"); return true; }
        g_locks.push_back({ p, static_cast<float>(atof(a2)) });
        _snprintf_s(reply, n, _TRUNCATE, "lock %p = %s every frame (%zu locks)", p, a2, g_locks.size()); return true;
    }
    if (!strcmp(cmd, "unlock")) { g_locks.clear(); _snprintf_s(reply, n, _TRUNCATE, "all locks cleared"); return true; }
    if (!strcmp(cmd, "watch")) {
        float* p = parse_addr(a1);
        if (!p) { g_watches.clear(); _snprintf_s(reply, n, _TRUNCATE, "watches cleared"); return true; }
        g_watches.push_back(p);
        _snprintf_s(reply, n, _TRUNCATE, "watching %p (%zu watches)", p, g_watches.size()); return true;
    }
    if (!strcmp(cmd, "fovhunt")) {
        hunt_begin(has1 ? static_cast<float>(atof(a1)) : 1.3f);
        _snprintf_s(reply, n, _TRUNCATE, "fovhunt started with %zu candidates (see [hunt] log lines)", g_hunt.size());
        return true;
    }
    if (!strcmp(cmd, "fovlock")) {
        if (g_hits.empty()) { _snprintf_s(reply, n, _TRUNCATE, "fovlock: no hunt hits yet"); return true; }
        const float factor = has1 ? static_cast<float>(atof(a1)) : 1.3f;
        size_t idx = has2 ? static_cast<size_t>(atoi(a2)) : 0;
        if (idx >= g_hits.size()) idx = 0;
        const HuntHit& h = g_hits[idx];
        g_locks.push_back({ h.addr, h.original * (g_hunt_sets[h.set].factor < 1.0f ? 1.0f / factor : factor) });
        _snprintf_s(reply, n, _TRUNCATE, "fovlock: hit %zu %p (%s) locked to %.4f (orig %.4f)",
                    idx, h.addr, g_hunt_sets[h.set].name, g_locks.back().value, h.original);
        return true;
    }
    if (!strcmp(cmd, "hunthits")) {
        for (size_t i = 0; i < g_hits.size(); ++i) {
            const HuntHit& h = g_hits[i];
            VRLOG("[hunt]  hit %zu: %p (%s) set=%s orig=%.4f  %.4f/%.4f -> %.4f/%.4f",
                  i, h.addr, where(h.addr), g_hunt_sets[h.set].name, h.original, h.th_before, h.tv_before, h.th_after, h.tv_after);
        }
        _snprintf_s(reply, n, _TRUNCATE, "%zu hits listed in log", g_hits.size());
        return true;
    }
    return false;
}

void on_present()
{
    hunt_tick();
    for (const Lock& l : g_locks) write_float(l.addr, l.value);
}

void status(FILE* f)
{
    fprintf(f, "scan: %zu candidates; locks=%zu; watches=%zu; hunt=%s (%zu/%zu, %zu hits)\n",
            g_cands.size(), g_locks.size(), g_watches.size(),
            g_hunt_phase >= 0 ? "RUNNING" : "idle", g_hunt_i, g_hunt.size(), g_hits.size());
    for (float* w : g_watches) {
        float v; const bool ok = read_float(w, v);
        fprintf(f, "watch %p (%s) = %.6f%s\n", static_cast<void*>(w), where(w), ok ? v : 0.0f, ok ? "" : " (unreadable)");
    }
    for (const Lock& l : g_locks) fprintf(f, "lock  %p = %.6f\n", static_cast<void*>(l.addr), l.value);
    for (size_t i = 0; i < g_hits.size(); ++i) {
        const HuntHit& h = g_hits[i];
        fprintf(f, "hit %zu: %p (%s) set=%s orig=%.4f  tangents %.4f/%.4f -> %.4f/%.4f\n",
                i, static_cast<void*>(h.addr), where(h.addr), g_hunt_sets[h.set].name, h.original,
                h.th_before, h.tv_before, h.th_after, h.tv_after);
    }
}

} // namespace memscan
