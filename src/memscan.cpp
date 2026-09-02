#include "memscan.h"
#include "os_input.h"
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
constexpr size_t kMaxCandidates = 2000000;

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

// Our own big buffers must never be scanned: a scan for value V stores V in
// g_cands (the `original` field), and the next region sweep would find those
// copies and feed on itself (seen: 2,000,000 "matches" of one vtable pointer).
struct Exclude { const char* lo; const char* hi; };
Exclude g_excl[4] = {};
size_t  g_excl_n = 0;
template <typename V>
void exclude_vector(const V& v)
{
    if (g_excl_n >= 4 || v.capacity() == 0) return;
    const char* lo = reinterpret_cast<const char*>(v.data());
    g_excl[g_excl_n++] = { lo, lo + v.capacity() * sizeof(typename V::value_type) };
}
bool excluded(const char* p)
{
    for (size_t i = 0; i < g_excl_n; ++i) if (p >= g_excl[i].lo && p < g_excl[i].hi) return true;
    return false;
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
            // Split around our own buffers rather than skipping whole regions.
            const char* cur = base;
            const char* stop = base + mbi.RegionSize;
            while (cur < stop) {
                const char* seg_end = stop;
                bool inside = false;
                for (size_t i = 0; i < g_excl_n; ++i) {
                    if (cur >= g_excl[i].lo && cur < g_excl[i].hi) { cur = g_excl[i].hi; inside = true; break; }
                    if (g_excl[i].lo > cur && g_excl[i].lo < seg_end) seg_end = g_excl[i].lo;
                }
                if (inside) continue;
                if (seg_end > cur) fn(cur, static_cast<size_t>(seg_end - cur));
                cur = seg_end;
            }
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

void prepare_scan()
{
    g_cands.clear();
    if (g_cands.capacity() < kMaxCandidates) g_cands.reserve(kMaxCandidates);   // stable buffer
    g_excl_n = 0;
    exclude_vector(g_cands);
}

size_t scan_range(float lo, float hi)
{
    prepare_scan();
    for_each_region([&](const char* base, size_t size) { scan_region(base, size, lo, hi); });
    return g_cands.size();
}

const char* where(const void* p);

// Every object whose first dword is `vtable`: list base addresses and the
// float at `offset` (default +0x50, where the camera class keeps its FOV).
void list_objects(unsigned vtable, unsigned offset, unsigned limit)
{
    prepare_scan();
    for_each_region([&](const char* base, size_t size) {
        __try {
            const unsigned* w = reinterpret_cast<const unsigned*>(base);
            for (size_t i = 0; i < size / 4 && g_cands.size() < kMaxCandidates; ++i) {
                if (w[i] == vtable) {
                    float fv = 0.0f;
                    g_cands.push_back({ const_cast<float*>(reinterpret_cast<const float*>(w + i)), fv });
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    });
    unsigned shown = 0;
    for (const Candidate& c : g_cands) {
        if (shown++ >= limit) break;
        float fv = 0.0f; unsigned bits = 0;
        const char* obj = reinterpret_cast<const char*>(c.addr);
        __try { std::memcpy(&bits, obj + offset, 4); } __except (EXCEPTION_EXECUTE_HANDLER) { bits = 0xDEADDEAD; }
        std::memcpy(&fv, &bits, 4);
        VRLOG("[objects]  %p (%s) +0x%X = %.4f [0x%08X]", static_cast<const void*>(obj), where(obj), offset, fv, bits);
    }
    VRLOG("[objects] %zu objects with vtable %08X", g_cands.size(), vtable);
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

// Synthetic input lives in os_input.h now, shared with vrinput. The rule it
// carries is unchanged: never inject unless the game is the foreground window.
using osinput::focus_game;
using osinput::game_window;
using osinput::send_key;
using osinput::send_mouse_button;

// ---- engine FOV control ----
// The engine's camera vertical FOV in degrees lives in a heap object (found
// 2026-08-20 by the two-state hunt, poke-verified: writing it moves the
// recovered projection AND the cull frustum - no void). Its address changes
// per launch, so 'fovfind' re-locates it: scan for the current vertical FOV
// in degrees, poke-verify the few matches, adopt the one that moves the
// tangents. 'fov auto' then holds it at the headset's vertical field.
float*   g_fov_addr = nullptr;
float    g_fov_original = 0.0f;

// The same for the first-person weapon's own FOV (the arms deform when the
// camera FOV departs from it - the engine's arm IK relates the two).
float*   g_vmfov_addr = nullptr;
float    g_vmfov_original = 0.0f;
enum class VmFovMode { Off, Fixed, Follow };
VmFovMode g_vmfov_mode = VmFovMode::Off;      // follow did NOT fix the arms and loses the push; opt-in via vmfov
float    g_vmfov_fixed_deg = 60.0f;
unsigned g_vmfovfind_attempts = 0;
bool     g_fovfind_hinted = false;   // the "run fovfind yourself" note is once per launch
unsigned g_restore_failed = 0;       // pokes we could not put back - corruption we caused
unsigned g_wrong_direction = 0;      // candidates that narrowed the view instead of widening it
bool     g_hunt_weapon = false;     // hunt verification signal: weapon tangents instead of world

bool hunt_tangents(float& th, float& tv)
{
    return g_hunt_weapon ? camover::weapon_tangents(th, tv) : camover::world_tangents(th, tv);
}
enum class FovMode { Off, Fixed, Auto };
FovMode  g_fov_mode = FovMode::Auto;     // auto: headset-matched once the address is known
float    g_fov_fixed_deg = 100.0f;
bool     g_fovfind_pending = false;      // adopt the first hunt hit when the hunt completes
unsigned g_fovfind_attempts = 0;
unsigned g_fov_stable_frames = 0;        // frames with a valid world projection

// What the engine last wrote into the FOV field on its own (read back before
// each of our writes). While we hold the field, this is the only trace of the
// game's intent - notably its ADS/scope zoom.
float    g_fov_engine_intent = 0.0f;
float    g_zoom = 1.0f;
bool     g_zoom_enabled = true;

void dump_words(const char* base, unsigned count)
{
    for (unsigned i = 0; i < count; i += 4) {
        char line[256] = {};
        size_t off = _snprintf_s(line, sizeof(line), _TRUNCATE, "[dump] %p:", base + i * 4);
        for (unsigned j = 0; j < 4 && i + j < count; ++j) {
            unsigned bits = 0; float fv = 0.0f;
            __try { std::memcpy(&bits, base + (i + j) * 4, 4); } __except (EXCEPTION_EXECUTE_HANDLER) { bits = 0xDEADDEAD; }
            std::memcpy(&fv, &bits, 4);
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "  %08X", bits);
            if (std::fabs(fv) > 1e-4f && std::fabs(fv) < 1e6f) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "(%.4g)", fv);
        }
        VRLOG("%s", line);
    }
}

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

// Two-state hunt: the engine zooms when aiming down sights; we press RMB
// ourselves, scan for every encoding of the ZOOMED tangents, release, and
// keep only addresses that return to the encoding of the UN-ZOOMED tangents.
// Survivors are then poke-verified like hunt v1.
int      g_h2_phase = -1;     // -1 idle, 0 press, 1 wait, 2 scan, 3 release, 4 wait, 5 refine
int      g_h2_wait = 0;
float    g_h2_th0 = 0.0f, g_h2_tv0 = 0.0f, g_h2_th1 = 0.0f, g_h2_tv1 = 0.0f;
constexpr int kH2Settle = 75;
constexpr size_t kH2Max = 1500000;   // compact candidates across all encodings
int g_h2_taps = 0;

// Encodings of a pair of tangents, same order as g_hunt_sets (minus ini literals).
struct Enc { const char* name; float factor; };
constexpr Enc kEnc[] = {
    { "deg_v", 1.3f }, { "deg_h", 1.3f }, { "halfdeg_v", 1.3f }, { "halfdeg_h", 1.3f },
    { "rad_v", 1.3f }, { "rad_h", 1.3f }, { "halfrad_v", 1.3f }, { "halfrad_h", 1.3f },
    { "tan_v", 1.3f }, { "tan_h", 1.3f }, { "proj_b", 1.0f / 1.3f }, { "proj_a", 1.0f / 1.3f },
};
constexpr size_t kEncCount = sizeof(kEnc) / sizeof(kEnc[0]);

float encode(size_t i, float th, float tv)
{
    const float deg_h = 2.0f * std::atan(th) * 180.0f / kPi, deg_v = 2.0f * std::atan(tv) * 180.0f / kPi;
    switch (i) {
    case 0: return deg_v;  case 1: return deg_h;  case 2: return deg_v * 0.5f; case 3: return deg_h * 0.5f;
    case 4: return deg_v * kPi / 180.0f; case 5: return deg_h * kPi / 180.0f;
    case 6: return deg_v * kPi / 360.0f; case 7: return deg_h * kPi / 360.0f;
    case 8: return tv; case 9: return th; case 10: return 1.0f / tv; case 11: return 1.0f / th;
    }
    return 0.0f;
}

void hunt_begin(float factor, bool deg_v_only = false)
{
    float th, tv;
    if (!hunt_tangents(th, tv)) { VRLOG("[hunt] no %s projection yet - load into a level first", g_hunt_weapon ? "weapon" : "world"); return; }
    if (g_hunt_phase >= 0) { VRLOG("[hunt] already running"); return; }
    g_hunt_factor = factor;
    g_hunt_sets.clear();
    g_hunt.clear();
    g_hits.clear();
    g_restore_failed = 0;
    g_wrong_direction = 0;
    const float deg_h = 2.0f * std::atan(th) * 180.0f / kPi, deg_v = 2.0f * std::atan(tv) * 180.0f / kPi;
    const float rad_h = deg_h * kPi / 180.0f, rad_v = deg_v * kPi / 180.0f;
    if (deg_v_only) {
        // fovfind: the vertical FOV in degrees, tight tolerance, every match.
        g_hunt_sets = { { "deg_v", deg_v, factor } };
        const float eps = std::fabs(deg_v) * 0.0005f + 1e-4f;
        scan_range(deg_v - eps, deg_v + eps);
        for (const Candidate& c : g_cands) {
            if (g_hunt.size() >= 4096) break;
            g_hunt.push_back({ c.addr, c.original, 0 });
        }
        g_cands.clear();
        g_hunt_i = 0; g_hunt_phase = 0; g_hunt_frames_total = 0;
        VRLOG("[fovfind] %s vertical FOV %.3f deg: %zu candidates, poke-verifying (x%.2f, ~%.0f s)", g_hunt_weapon ? "WEAPON" : "camera", deg_v, g_hunt.size(), factor, g_hunt.size() * (kHuntWait + 2) / 60.0f);
        return;
    }
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

void hunt2_begin(float factor)
{
    float th, tv;
    if (!hunt_tangents(th, tv)) { VRLOG("[hunt2] no %s projection yet", g_hunt_weapon ? "weapon" : "world"); return; }
    if (g_hunt_phase >= 0) { VRLOG("[hunt2] a hunt is already running"); return; }
    g_hunt_factor = factor;
    g_hunt_sets.clear();
    for (size_t i = 0; i < kEncCount; ++i) g_hunt_sets.push_back({ kEnc[i].name, 0.0f, kEnc[i].factor });
    g_hunt.clear(); g_hits.clear();
    g_h2_phase = 0; g_h2_wait = 0; g_h2_taps = 0;
    VRLOG("[hunt2] starting: two-state (ADS toggle) hunt on the %s projection, factor %.2f", g_hunt_weapon ? "WEAPON" : "world", factor);
}

void tap_rmb() { send_mouse_button(true, true); send_mouse_button(true, false); }

// One sweep, all encodings: a float matching encoding i of (th, tv) within
// 0.1% is recorded as {addr, i}. Much faster than 12 sweeps, and the tight
// tolerance keeps the list small enough to keep everything.
void scan_all_encodings(float th, float tv)
{
    float want[kEncCount], eps[kEncCount];
    for (size_t i = 0; i < kEncCount; ++i) { want[i] = encode(i, th, tv); eps[i] = std::fabs(want[i]) * 0.001f + 1e-6f; }
    g_hunt.clear();
    if (g_hunt.capacity() < kH2Max) g_hunt.reserve(kH2Max);
    g_excl_n = 0;
    exclude_vector(g_cands);
    exclude_vector(g_hunt);
    for_each_region([&](const char* base, size_t size) {
        __try {
            const float* f = reinterpret_cast<const float*>(base);
            const size_t cnt = size / 4;
            for (size_t k = 0; k < cnt && g_hunt.size() < kH2Max; ++k) {
                const float v = f[k];
                if (!(v > 1e-4f && v < 1e4f)) continue;
                for (size_t i = 0; i < kEncCount; ++i) {
                    if (std::fabs(v - want[i]) <= eps[i]) { g_hunt.push_back({ const_cast<float*>(f + k), v, static_cast<int>(i) }); break; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    });
}

void hunt2_tick()
{
    if (g_h2_phase < 0) return;
    switch (g_h2_phase) {
    case 0: {
        hunt_tangents(g_h2_th0, g_h2_tv0);
        if (!focus_game()) {
            VRLOG("[hunt2] ABORT: could not bring the game window to the foreground; click the game window and rerun hunt2");
            g_h2_phase = -1;
            return;
        }
        VRLOG("[hunt2] T0 %s tangents %.4f/%.4f; game focused; tapping RMB (zoom toggle)", g_hunt_weapon ? "weapon" : "world", g_h2_th0, g_h2_tv0);
        tap_rmb(); g_h2_taps = 1;
        g_h2_phase = 1; g_h2_wait = 0;
        return;
    }
    case 1:
        if (++g_h2_wait < kH2Settle) return;
        g_h2_phase = 2;
        return;
    case 2: {
        hunt_tangents(g_h2_th1, g_h2_tv1);
        const bool moved = std::fabs(g_h2_th1 / g_h2_th0 - 1.0f) > 0.03f || std::fabs(g_h2_tv1 / g_h2_tv0 - 1.0f) > 0.03f;
        if (!moved) {
            if (g_h2_taps < 3) {   // maybe the first tap un-zoomed a stale zoom; try once more
                VRLOG("[hunt2] no change after tap %d (%.4f/%.4f); tapping again", g_h2_taps, g_h2_th1, g_h2_tv1);
                tap_rmb(); ++g_h2_taps; g_h2_phase = 1; g_h2_wait = 0;
                return;
            }
            VRLOG("[hunt2] ABORT: %s tangents did not change under zoom (%.4f/%.4f)", g_hunt_weapon ? "weapon" : "world", g_h2_th1, g_h2_tv1);
            g_h2_phase = -1;
            return;
        }
        VRLOG("[hunt2] T1 (zoomed) %.4f/%.4f - one-pass scan of %zu encodings at 0.1%%...", g_h2_th1, g_h2_tv1, kEncCount);
        scan_all_encodings(g_h2_th1, g_h2_tv1);
        size_t per[kEncCount] = {};
        for (const HuntCand& c : g_hunt) ++per[c.set];
        for (size_t i = 0; i < kEncCount; ++i) if (per[i]) VRLOG("[hunt2]  %-10s zoomed=%10.4f: %zu", kEnc[i].name, encode(i, g_h2_th1, g_h2_tv1), per[i]);
        VRLOG("[hunt2] %zu candidates%s; tapping RMB to un-zoom", g_hunt.size(), g_hunt.size() >= kH2Max ? " (CAPPED)" : "");
        g_h2_phase = 3;
        return;
    }
    case 3:
        tap_rmb();
        g_h2_phase = 4; g_h2_wait = 0;
        return;
    case 4:
        if (++g_h2_wait < kH2Settle) return;
        g_h2_phase = 5;
        return;
    case 5: {
        float th2, tv2;
        hunt_tangents(th2, tv2);
        const bool back = std::fabs(th2 / g_h2_th0 - 1.0f) < 0.03f && std::fabs(tv2 / g_h2_tv0 - 1.0f) < 0.03f;
        if (!back) {
            VRLOG("[hunt2] not back to T0 after un-zoom tap (%.4f/%.4f vs %.4f/%.4f); tapping again", th2, tv2, g_h2_th0, g_h2_tv0);
            tap_rmb(); g_h2_phase = 4; g_h2_wait = 0;
            if (++g_h2_taps > 6) { VRLOG("[hunt2] ABORT: cannot restore the un-zoomed state"); g_h2_phase = -1; }
            return;
        }
        VRLOG("[hunt2] T2 (released) %.4f/%.4f; keeping addresses that followed the zoom back", th2, tv2);
        std::vector<HuntCand> kept;
        size_t per_set[kEncCount] = {};
        for (const HuntCand& c : g_hunt) {
            float now;
            if (!read_float(c.addr, now)) continue;
            const float want = encode(static_cast<size_t>(c.set), th2, tv2);
            if (std::fabs(now - want) <= std::fabs(want) * 0.001f + 1e-6f &&
                std::fabs(now - c.original) > std::fabs(want) * 0.01f) {
                kept.push_back({ c.addr, now, c.set });
                ++per_set[c.set];
            }
        }
        g_hunt.swap(kept);
        for (size_t i = 0; i < kEncCount; ++i) if (per_set[i]) VRLOG("[hunt2]  %-10s: %zu two-state matches", kEnc[i].name, per_set[i]);
        for (size_t i = 0; i < g_hunt.size() && i < 60; ++i)
            VRLOG("[hunt2]  cand %zu: %p (%s) set=%s value=%.4f", i, g_hunt[i].addr, where(g_hunt[i].addr), kEnc[g_hunt[i].set].name, g_hunt[i].original);
        if (g_hunt.size() > 3000) { VRLOG("[hunt2] %zu survivors - verifying the first 3000", g_hunt.size()); g_hunt.resize(3000); }
        VRLOG("[hunt2] %zu survivors -> poke-verify (factor %.2f)", g_hunt.size(), g_hunt_factor);
        g_h2_phase = -1;
        g_hunt_i = 0; g_hunt_phase = 0; g_hunt_frames_total = 0;
        return;
    }
    }
}

void hunt_tick()
{
    if (g_hunt_phase < 0) return;
    if (g_hunt_i >= g_hunt.size()) {
        g_hunt_phase = -1;
        VRLOG("[hunt] DONE: %zu hits of %zu candidates over %u frames "
              "(%u rejected for narrowing, %u restores FAILED)",
              g_hits.size(), g_hunt.size(), g_hunt_frames_total, g_wrong_direction, g_restore_failed);
        if (g_restore_failed) {
            VRLOG("[hunt] WARNING: %u pokes could not be restored - the projection may be wrong. "
                  "Run 'fov restore', or restart the game.", g_restore_failed);
        }
        for (const HuntHit& h : g_hits) {
            VRLOG("[hunt]  HIT %p (%s) set=%s orig=%.4f  tangents %.4f/%.4f -> %.4f/%.4f",
                  h.addr, where(h.addr), g_hunt_sets[h.set].name, h.original, h.th_before, h.tv_before, h.th_after, h.tv_after);
        }
        if (g_fovfind_pending) {
            g_fovfind_pending = false;
            if (!g_hits.empty()) {
                // Prefer the hit whose effect matched the poke exactly (a source value).
                const HuntHit* best = &g_hits[0];
                for (const HuntHit& h : g_hits) {
                    const float want = std::tan(0.5f * h.original * g_hunt_sets[h.set].factor * kPi / 180.0f);
                    const float got  = h.tv_after;
                    const float wb   = std::tan(0.5f * best->original * g_hunt_sets[best->set].factor * kPi / 180.0f);
                    if (std::fabs(got / want - 1.0f) < std::fabs(best->tv_after / wb - 1.0f)) best = &h;
                }
                if (g_hunt_weapon) {
                    g_vmfov_addr = best->addr;
                    g_vmfov_original = best->original;
                    VRLOG("[fovfind] WEAPON FOV ADDRESS %p (%s), original %.3f deg", g_vmfov_addr, where(g_vmfov_addr), g_vmfov_original);
                } else {
                    g_fov_addr = best->addr;
                    g_fov_original = best->original;
                    VRLOG("[fovfind] ENGINE FOV ADDRESS %p (%s), original %.3f deg - '%s' mode active",
                          g_fov_addr, where(g_fov_addr), g_fov_original,
                          g_fov_mode == FovMode::Auto ? "auto" : g_fov_mode == FovMode::Fixed ? "fixed" : "off");
                }
            } else {
                VRLOG("[fovfind] no candidate moved the %s projection (attempt %u)", g_hunt_weapon ? "weapon" : "world", g_hunt_weapon ? g_vmfovfind_attempts : g_fovfind_attempts);
            }
        }
        return;
    }
    ++g_hunt_frames_total;
    HuntCand& c = g_hunt[g_hunt_i];
    if (g_hunt_phase == 0) {
        hunt_tangents(g_base_th, g_base_tv);
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
    const bool have = hunt_tangents(th, tv);
    write_float(c.addr, c.original);

    // Verify the restore actually took. A hunt that cannot put back what it
    // poked silently corrupts the game: the world frustum ends up wrong, the
    // headset view black-boxes, and nothing says why. Count and name them.
    {
        float back = 0.0f;
        if (!read_float(c.addr, back) ||
            std::fabs(back - c.original) > std::fabs(c.original) * 0.01f + 1e-5f) {
            ++g_restore_failed;
            if (g_restore_failed <= 10) {
                VRLOG("[hunt] RESTORE FAILED %p (%s) set=%s: wanted %.4f, reads %.4f",
                      c.addr, where(c.addr), g_hunt_sets[c.set].name, c.original, back);
            }
        }
    }

    const bool moved = have && g_base_th > 0.0f &&
        (std::fabs(th / g_base_th - 1.0f) > 0.05f || std::fabs(tv / g_base_tv - 1.0f) > 0.05f);
    // Every encoding in kEnc is built so that a correct address WIDENS the
    // field: deg/rad/tan are poked x1.3, and proj_a/proj_b hold 1/tan so they
    // are poked x(1/1.3). So the tangent must go UP. A candidate that narrows
    // it is not the FOV - it is some other float that perturbs the projection -
    // and accepting it is how the world collapsed from 124 to 18.5 degrees.
    const bool wider = moved && th > g_base_th && tv > g_base_tv;
    if (moved && !wider) {
        ++g_wrong_direction;
        if (g_wrong_direction <= 10) {
            VRLOG("[hunt] rejected %p (%s) set=%s orig=%.4f: NARROWS the view %.4f/%.4f -> %.4f/%.4f",
                  c.addr, where(c.addr), g_hunt_sets[c.set].name, c.original,
                  g_base_th, g_base_tv, th, tv);
        }
    }
    if (wider) {
        g_hits.push_back({ c.addr, c.original, c.set, g_base_th, g_base_tv, th, tv });
        VRLOG("[hunt] HIT %p (%s) set=%s orig=%.4f: tangents %.4f/%.4f -> %.4f/%.4f",
              c.addr, where(c.addr), g_hunt_sets[c.set].name, c.original, g_base_th, g_base_tv, th, tv);
    }
    ++g_hunt_i;
    g_hunt_phase = 0;
    if (g_hunt_i % 100 == 0) VRLOG("[hunt] %zu/%zu candidates tried, %zu hits so far", g_hunt_i, g_hunt.size(), g_hits.size());
}

bool read_ptr(void* addr, void*& out)
{
    __try { out = *static_cast<void**>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the engine's own settings lookup ---------------------------------------
// Decompiled from the dumpimage image (docs/recon/decompiled-live.txt): the
// game resolves "Module.Field" names to live storage through
//
//     void* __thiscall GetGlobalVariable(Registry* this, const char* name, TypeInfo* type)
//
// at 0x004F4C40, with the registry singleton in the static at 0x0155E05C. It
// splits on the dot, finds the module's instance, and resolves the field BY
// NAME through reflection - so "Render.ForceFov" resolves to
// GameRenderSettings::ForceFov (Float32 @+0x18) even though no such string
// exists in the binary. The TypeInfo argument is the field's expected type;
// the scalar objects are at fixed addresses (docs/engine-map.md).
//
// The image base is 0x00400000 on every launch recorded (no ASLR), and that
// is checked here rather than assumed.
namespace gvar {
constexpr uintptr_t kFn        = 0x004F4C40;
constexpr uintptr_t kRegistry  = 0x0155E05C;
constexpr uintptr_t kFloat32   = 0x01BF13E4;
constexpr uintptr_t kBoolean   = 0x01BF1354;
constexpr uintptr_t kInt32     = 0x01BF13B4;
constexpr uintptr_t kUint32    = 0x01BF13A4;
// __fastcall with a dead EDX slot is how a __thiscall free function is
// spelled through a pointer in MSVC: ECX = this, the rest on the stack,
// callee cleans.
typedef void* (__fastcall* Fn)(void* self, void* edx_unused, const char* name, void* type);

bool image_ok()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == 0x00400000u;
}

void* lookup(const char* name, uintptr_t type_obj, char* err, size_t n)
{
    if (!image_ok()) { _snprintf_s(err, n, _TRUNCATE, "image base is not 0x400000"); return nullptr; }
    void* self = nullptr;
    if (!read_ptr(reinterpret_cast<void*>(kRegistry), self) || !self) {
        _snprintf_s(err, n, _TRUNCATE, "registry static %08X is null", static_cast<unsigned>(kRegistry)); return nullptr;
    }
    void* out = nullptr;
    __try {
        out = reinterpret_cast<Fn>(kFn)(self, nullptr, name, reinterpret_cast<void*>(type_obj));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(err, n, _TRUNCATE, "GetGlobalVariable faulted"); return nullptr;
    }
    if (!out) _snprintf_s(err, n, _TRUNCATE, "not found");
    return out;
}

uintptr_t type_for(const char* t)
{
    if (!t || !*t || !_stricmp(t, "float")) return kFloat32;
    if (!_stricmp(t, "bool"))  return kBoolean;
    if (!_stricmp(t, "int"))   return kInt32;
    if (!_stricmp(t, "uint"))  return kUint32;
    return 0;
}
} // namespace gvar

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
            unsigned bits = 0; std::memcpy(&bits, &v, sizeof(bits));
            VRLOG("[scan]  %p (%s) = %.6f [0x%08X] (was %.6f)", c.addr, where(c.addr), ok ? v : 0.0f, ok ? bits : 0u, c.original);
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
    if (!strcmp(cmd, "dump")) {
        const char* base = reinterpret_cast<const char*>(parse_addr(a1));
        const unsigned count = has2 ? static_cast<unsigned>(atoi(a2)) : 32;
        if (!base) { _snprintf_s(reply, n, _TRUNCATE, "usage: dump <hexaddr> [dwords]"); return true; }
        dump_words(base, count > 1024 ? 1024 : count);
        _snprintf_s(reply, n, _TRUNCATE, "dumped %u dwords at %p", count, static_cast<const void*>(base));
        return true;
    }
    if (!strcmp(cmd, "scanptr")) {
        // Words whose VALUE (as a 32-bit integer) lies in [lo, hi]: pointer scan.
        if (!has2) { _snprintf_s(reply, n, _TRUNCATE, "usage: scanptr <hexlo> <hexhi>"); return true; }
        const unsigned lo = static_cast<unsigned>(strtoul(a1, nullptr, 16)), hi = static_cast<unsigned>(strtoul(a2, nullptr, 16));
        prepare_scan();
        for_each_region([&](const char* base, size_t size) {
            __try {
                const unsigned* w = reinterpret_cast<const unsigned*>(base);
                for (size_t i = 0; i < size / 4 && g_cands.size() < kMaxCandidates; ++i) {
                    if (w[i] >= lo && w[i] <= hi) {
                        float fv; std::memcpy(&fv, &w[i], 4);
                        g_cands.push_back({ const_cast<float*>(reinterpret_cast<const float*>(w + i)), fv });
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        });
        size_t in_exe = 0;
        for (const Candidate& c : g_cands) if (!strcmp(where(c.addr), "exe")) ++in_exe;
        _snprintf_s(reply, n, _TRUNCATE, "scanptr: %zu words point into [%08X, %08X], %zu of them in the exe image (list to see)", g_cands.size(), lo, hi, in_exe);
        return true;
    }
    if (!strcmp(cmd, "objects")) {
        if (!has1) { _snprintf_s(reply, n, _TRUNCATE, "usage: objects <vtable-hex> [offset-hex]"); return true; }
        const unsigned vt = static_cast<unsigned>(strtoul(a1, nullptr, 16));
        const unsigned off = has2 ? static_cast<unsigned>(strtoul(a2, nullptr, 16)) : 0x50u;
        list_objects(vt, off, 64);
        _snprintf_s(reply, n, _TRUNCATE, "objects: %zu with vtable %08X (see [objects] lines)", g_cands.size(), vt);
        return true;
    }
    if (!strcmp(cmd, "gvar")) {
        // Read-only: resolve a "Module.Field" name through the engine's own
        // GetGlobalVariable and report where it lives and what it holds.
        if (!has1) { _snprintf_s(reply, n, _TRUNCATE, "usage: gvar <Module.Field> [float|bool|int|uint]"); return true; }
        const uintptr_t ty = gvar::type_for(has2 ? a2 : "float");
        if (!ty) { _snprintf_s(reply, n, _TRUNCATE, "gvar: unknown type %s", a2); return true; }
        char err[96] = {};
        void* p = gvar::lookup(a1, ty, err, sizeof(err));
        if (!p) { _snprintf_s(reply, n, _TRUNCATE, "gvar %s: %s", a1, err); return true; }
        float fv = 0.0f; unsigned uv = 0;
        const bool okf = read_float(static_cast<float*>(p), fv);
        __try { uv = *static_cast<unsigned*>(p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        _snprintf_s(reply, n, _TRUNCATE, "gvar %s -> %p (%s) float=%.4f raw=0x%08X byte=%u",
                    a1, p, where(static_cast<float*>(p)), okf ? fv : 0.0f, uv, uv & 0xFF);
        return true;
    }
    if (!strcmp(cmd, "gvarset")) {
        // The one deliberate write. Float only, and it says what it replaced.
        if (!has2) { _snprintf_s(reply, n, _TRUNCATE, "usage: gvarset <Module.Field> <float>"); return true; }
        char err[96] = {};
        void* p = gvar::lookup(a1, gvar::kFloat32, err, sizeof(err));
        if (!p) { _snprintf_s(reply, n, _TRUNCATE, "gvarset %s: %s", a1, err); return true; }
        float was = 0.0f; read_float(static_cast<float*>(p), was);
        const float v = static_cast<float>(atof(a2));
        write_float(static_cast<float*>(p), v);
        VRLOG("[gvar] %s @%p: %.4f -> %.4f", a1, p, was, v);
        _snprintf_s(reply, n, _TRUNCATE, "gvarset %s @%p: %.4f -> %.4f", a1, p, was, v);
        return true;
    }
    if (!strcmp(cmd, "fovfind")) {
        g_hunt_weapon = has2 && !_stricmp(a2, "weapon");
        g_fovfind_pending = true;
        if (g_hunt_weapon) ++g_vmfovfind_attempts; else ++g_fovfind_attempts;
        hunt_begin(has1 ? static_cast<float>(atof(a1)) : 1.3f, true);
        _snprintf_s(reply, n, _TRUNCATE, "fovfind started (%zu candidates)", g_hunt.size());
        return true;
    }
    if (!strcmp(cmd, "fov")) {
        if (!has1) {
            _snprintf_s(reply, n, _TRUNCATE, "fov: addr=%p mode=%s fixed=%.1f", static_cast<void*>(g_fov_addr),
                        g_fov_mode == FovMode::Auto ? "auto" : g_fov_mode == FovMode::Fixed ? "fixed" : "off", g_fov_fixed_deg);
            return true;
        }
        if (!_stricmp(a1, "auto")) g_fov_mode = FovMode::Auto;
        else if (!_stricmp(a1, "restore")) {
            // Put back EVERY address a hunt has touched, not just the one that
            // was selected. `fov off` restores only g_fov_addr, which is no use
            // when the damage was done by candidates that were poked, recorded
            // and rejected - which is exactly the case that black-boxed the
            // view. Verified, because an unverified restore is what got us here.
            g_fov_mode = FovMode::Off;
            unsigned tried = 0, failed = 0;
            auto put_back = [&](float* addr, float orig) {
                if (!addr) return;
                ++tried;
                write_float(addr, orig);
                float back = 0.0f;
                if (!read_float(addr, back) ||
                    std::fabs(back - orig) > std::fabs(orig) * 0.01f + 1e-5f) ++failed;
            };
            for (const HuntHit& h : g_hits)   put_back(h.addr, h.original);
            for (const HuntCand& c : g_hunt)  put_back(c.addr, c.original);
            put_back(g_fov_addr, g_fov_original);
            put_back(g_vmfov_addr, g_vmfov_original);
            g_locks.clear();
            VRLOG("[fov] restore: %u addresses put back, %u would not take", tried, failed);
            _snprintf_s(reply, n, _TRUNCATE,
                        "fov restore: %u put back, %u failed%s", tried, failed,
                        failed ? " - restart the game to be sure" : "");
            return true;
        }
        else if (!_stricmp(a1, "off")) {
            g_fov_mode = FovMode::Off;
            if (g_fov_addr) write_float(g_fov_addr, g_fov_original);
        } else if (!_stricmp(a1, "addr") && has2) {
            g_fov_addr = parse_addr(a2);
            read_float(g_fov_addr, g_fov_original);
        } else {
            g_fov_mode = FovMode::Fixed;
            g_fov_fixed_deg = (std::min)((std::max)(static_cast<float>(atof(a1)), 20.0f), 150.0f);
        }
        _snprintf_s(reply, n, _TRUNCATE, "fov: addr=%p mode=%s fixed=%.1f orig=%.2f", static_cast<void*>(g_fov_addr),
                    g_fov_mode == FovMode::Auto ? "auto" : g_fov_mode == FovMode::Fixed ? "fixed" : "off", g_fov_fixed_deg, g_fov_original);
        return true;
    }
    if (!strcmp(cmd, "zoom")) {
        if (has1) {
            if (!_stricmp(a1, "on")) g_zoom_enabled = true;
            else if (!_stricmp(a1, "off")) { g_zoom_enabled = false; g_zoom = 1.0f; }
        }
        _snprintf_s(reply, n, _TRUNCATE, "zoom %s: engine wants %.2f deg (default %.2f) -> %.2fx",
                    g_zoom_enabled ? "on" : "off", g_fov_engine_intent, g_fov_original, g_zoom);
        return true;
    }
    if (!strcmp(cmd, "vmfov")) {
        if (!has1) {
            _snprintf_s(reply, n, _TRUNCATE, "vmfov: addr=%p mode=%s fixed=%.1f orig=%.2f", static_cast<void*>(g_vmfov_addr),
                        g_vmfov_mode == VmFovMode::Follow ? "follow" : g_vmfov_mode == VmFovMode::Fixed ? "fixed" : "off", g_vmfov_fixed_deg, g_vmfov_original);
            return true;
        }
        if (!_stricmp(a1, "follow")) g_vmfov_mode = VmFovMode::Follow;
        else if (!_stricmp(a1, "off")) { g_vmfov_mode = VmFovMode::Off; if (g_vmfov_addr) write_float(g_vmfov_addr, g_vmfov_original); }
        else if (!_stricmp(a1, "addr") && has2) { g_vmfov_addr = parse_addr(a2); read_float(g_vmfov_addr, g_vmfov_original); }
        else { g_vmfov_mode = VmFovMode::Fixed; g_vmfov_fixed_deg = (std::min)((std::max)(static_cast<float>(atof(a1)), 10.0f), 150.0f); }
        _snprintf_s(reply, n, _TRUNCATE, "vmfov: addr=%p mode=%s fixed=%.1f orig=%.2f", static_cast<void*>(g_vmfov_addr),
                    g_vmfov_mode == VmFovMode::Follow ? "follow" : g_vmfov_mode == VmFovMode::Fixed ? "fixed" : "off", g_vmfov_fixed_deg, g_vmfov_original);
        return true;
    }
    if (!strcmp(cmd, "hunt2")) {
        g_hunt_weapon = has2 && !_stricmp(a2, "weapon");
        g_fovfind_pending = true;
        hunt2_begin(has1 ? static_cast<float>(atof(a1)) : 1.3f);
        _snprintf_s(reply, n, _TRUNCATE, "hunt2 started (watch [hunt2] lines)");
        return true;
    }
    if (!strcmp(cmd, "focus")) {
        const bool ok = focus_game();
        _snprintf_s(reply, n, _TRUNCATE, "focus: hwnd=%p %s", static_cast<void*>(game_window()), ok ? "foreground" : "NOT foreground");
        return true;
    }
    if (!strcmp(cmd, "rmb") || !strcmp(cmd, "lmb")) {
        const bool down = has1 && !_stricmp(a1, "down");
        if (!focus_game()) { _snprintf_s(reply, n, _TRUNCATE, "%s: game not in foreground - refusing to inject input", cmd); return true; }
        send_mouse_button(cmd[0] == 'r', down);
        _snprintf_s(reply, n, _TRUNCATE, "%s %s", cmd, down ? "down" : "up");
        return true;
    }
    if (!strcmp(cmd, "key")) {
        if (!has1) { _snprintf_s(reply, n, _TRUNCATE, "usage: key <vk-hex> down|up|tap"); return true; }
        const WORD vk = static_cast<WORD>(strtoul(a1, nullptr, 16));
        if (!focus_game()) { _snprintf_s(reply, n, _TRUNCATE, "key: game not in foreground - refusing to inject input"); return true; }
        if (!has2 || !_stricmp(a2, "tap")) { send_key(vk, true); send_key(vk, false); }
        else send_key(vk, !_stricmp(a2, "down"));
        _snprintf_s(reply, n, _TRUNCATE, "key 0x%02X %s", vk, has2 ? a2 : "tap");
        return true;
    }
    if (!strcmp(cmd, "huntstop")) {
        g_hunt_phase = -1; g_h2_phase = -1;
        _snprintf_s(reply, n, _TRUNCATE, "hunts stopped (%zu hits kept)", g_hits.size());
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
    hunt2_tick();
    hunt_tick();
    for (const Lock& l : g_locks) write_float(l.addr, l.value);

    // Engine FOV: hold it once an address is known. Locating it is NOT automatic.
    //
    // This used to start a hunt by itself ~10 s after the view stabilised, and
    // retry up to three times. A hunt pokes thousands of heap floats and puts
    // each back; when a restore does not take, the world frustum is left wrong,
    // and nothing tells the player. One launch it found two candidates that
    // happened to cancel; the next it found four that cascaded:
    //
    //   HIT 10152BC4 orig=45.0994: tangents 0.5536/0.4152 -> 0.4349/0.3262
    //   HIT 1027CD48 orig=45.0814: tangents 0.4158/0.3118 -> 0.3405/0.2554
    //   HIT 1027E164 orig=45.0813: tangents 0.3238/0.2429 -> 0.2513/0.1885
    //   HIT 102DAAD8 orig=45.1000: tangents 0.2332/0.1749 -> 0.1630/0.1223
    //
    // leaving the world at 18.5 x 13.9 degrees instead of 124 x 109. That
    // collapse alone black-boxed the headset view (the eye bounds sample far
    // outside the texture), hid the gun, and killed the viewmodel classifier,
    // which keys on the weapon FOV differing from the world's. Unsupervised
    // memory writes with no verification are not worth that. Run `fovfind`
    // (or `fovhunt`) from the console when you want it.
    float th, tv;
    const bool have_world = camover::world_tangents(th, tv);
    g_fov_stable_frames = have_world ? g_fov_stable_frames + 1 : 0;
    if (!g_fov_addr && g_fov_mode != FovMode::Off && have_world && !g_fovfind_hinted &&
        g_fov_stable_frames > 600) {
        g_fovfind_hinted = true;
        VRLOG("[fovfind] engine FOV not located; widening is OFF. Run 'fovfind' to locate it, "
              "'fov restore' to undo a hunt. Auto-location is deliberately disabled.");
    }
    if (g_vmfov_addr && g_vmfov_mode != VmFovMode::Off && g_hunt_phase < 0) {
        float target = g_vmfov_fixed_deg;
        if (g_vmfov_mode == VmFovMode::Follow) {
            float cur = 0.0f;
            if (g_fov_addr && read_float(g_fov_addr, cur) && cur > 5.0f) target = cur;
            else target = g_vmfov_original;
        }
        write_float(g_vmfov_addr, (std::min)((std::max)(target, 10.0f), 150.0f));
    }

    if (g_fov_addr && g_fov_mode != FovMode::Off && g_hunt_phase < 0) {
        float target = g_fov_fixed_deg;
        if (g_fov_mode == FovMode::Auto) {
            float hh, hv;
            if (camover::headset_tangents(hh, hv)) target = 2.0f * std::atan(hv) * 180.0f / kPi;
            else target = g_fov_original;
        }
        target = (std::min)((std::max)(target, 20.0f), 150.0f);

        // Read before writing: anything that is not our own target is the
        // engine's own intent for this frame (idle 45.1, scoped ~13.9...).
        float seen = 0.0f;
        if (read_float(g_fov_addr, seen) && seen > 1.0f && seen < 179.0f &&
            std::fabs(seen - target) > 0.01f) {
            g_fov_engine_intent = seen;
        }
        if (g_zoom_enabled && g_fov_original > 1.0f && g_fov_engine_intent > 1.0f) {
            const float tn = std::tan(0.5f * g_fov_original * kPi / 180.0f);
            const float tz = std::tan(0.5f * g_fov_engine_intent * kPi / 180.0f);
            const float z = (tz > 1e-5f) ? tn / tz : 1.0f;
            g_zoom = (std::min)((std::max)(z, 1.0f), 12.0f);
        } else {
            g_zoom = 1.0f;
        }

        write_float(g_fov_addr, target);
    }
}

float engine_zoom() { return g_zoom_enabled ? g_zoom : 1.0f; }

void status(FILE* f)
{
    fprintf(f, "scan: %zu candidates; locks=%zu; watches=%zu; hunt=%s (%zu/%zu, %zu hits)\n",
            g_cands.size(), g_locks.size(), g_watches.size(),
            g_hunt_phase >= 0 ? "RUNNING" : "idle", g_hunt_i, g_hunt.size(), g_hits.size());
    fprintf(f, "hunt2: %s\n", g_h2_phase < 0 ? "idle" : "RUNNING");
    {
        float cur = 0.0f; const bool ok = g_fov_addr && read_float(g_fov_addr, cur);
        fprintf(f, "engine fov: addr=%p mode=%s fixed=%.1f original=%.2f current=%.2f attempts=%u\n",
                static_cast<void*>(g_fov_addr), g_fov_mode == FovMode::Auto ? "auto" : g_fov_mode == FovMode::Fixed ? "fixed" : "off",
                g_fov_fixed_deg, g_fov_original, ok ? cur : 0.0f, g_fovfind_attempts);
        float vcur = 0.0f; const bool vok = g_vmfov_addr && read_float(g_vmfov_addr, vcur);
        fprintf(f, "zoom: %s engine-intent=%.2f deg -> %.2fx\n", g_zoom_enabled ? "on" : "off", g_fov_engine_intent, g_zoom);
        fprintf(f, "weapon fov: addr=%p mode=%s fixed=%.1f original=%.2f current=%.2f attempts=%u\n",
                static_cast<void*>(g_vmfov_addr), g_vmfov_mode == VmFovMode::Follow ? "follow" : g_vmfov_mode == VmFovMode::Fixed ? "fixed" : "off",
                g_vmfov_fixed_deg, g_vmfov_original, vok ? vcur : 0.0f, g_vmfovfind_attempts);
    }
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
