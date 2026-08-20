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

// ---- game window + synthetic input (for autonomous state changes) ----
HWND g_game_hwnd = nullptr;

BOOL CALLBACK find_game_window(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r = {};
    GetWindowRect(h, &r);
    if (r.right - r.left < 200 || r.bottom - r.top < 200) return TRUE;
    g_game_hwnd = h;
    return FALSE;
}

HWND game_window()
{
    if (!g_game_hwnd || !IsWindow(g_game_hwnd)) { g_game_hwnd = nullptr; EnumWindows(find_game_window, 0); }
    return g_game_hwnd;
}

bool focus_game()
{
    HWND h = game_window();
    if (!h) return false;
    if (GetForegroundWindow() == h) return true;
    const DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, TRUE);
    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, FALSE);
    return GetForegroundWindow() == h;
}

void send_mouse_button(bool right, bool down)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                          : (down ? MOUSEEVENTF_LEFTDOWN  : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof(in));
}

void send_key(WORD vk, bool down)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

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
VmFovMode g_vmfov_mode = VmFovMode::Follow;   // follow: keep the weapon FOV equal to the camera FOV
float    g_vmfov_fixed_deg = 60.0f;
unsigned g_vmfovfind_attempts = 0;
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
        VRLOG("[hunt] DONE: %zu hits of %zu candidates over %u frames", g_hits.size(), g_hunt.size(), g_hunt_frames_total);
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

    // Engine FOV: locate once (no input needed), then hold every frame.
    float th, tv;
    const bool have_world = camover::world_tangents(th, tv);
    g_fov_stable_frames = have_world ? g_fov_stable_frames + 1 : 0;
    if (!g_fov_addr && g_fov_mode != FovMode::Off && g_hunt_phase < 0 && g_h2_phase < 0 &&
        g_fov_stable_frames > 600 && g_fovfind_attempts < 3 && (g_fov_stable_frames % 1800) == 601) {
        VRLOG("[fovfind] auto-locating the engine FOV (attempt %u)...", g_fovfind_attempts + 1);
        g_fovfind_pending = true;
        ++g_fovfind_attempts;
        hunt_begin(1.3f, true);
    }
    // Weapon FOV: locate after the camera FOV is known, then hold (follow/fixed).
    float wth, wtv;
    const bool have_weapon = camover::weapon_tangents(wth, wtv);
    if (g_fov_addr && !g_vmfov_addr && g_vmfov_mode != VmFovMode::Off && have_weapon &&
        g_hunt_phase < 0 && g_h2_phase < 0 && g_vmfovfind_attempts < 2 && (g_fov_stable_frames % 900) == 450) {
        VRLOG("[fovfind] auto-locating the WEAPON FOV (attempt %u)...", g_vmfovfind_attempts + 1);
        g_hunt_weapon = true;
        g_fovfind_pending = true;
        ++g_vmfovfind_attempts;
        hunt_begin(1.3f, true);
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
        write_float(g_fov_addr, target);
    }
}

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
