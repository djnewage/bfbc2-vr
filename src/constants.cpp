#include "constants.h"
#include "logger.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace vrconst {
namespace {

constexpr float kEps = 1e-4f;

std::mutex g_mutex;

float g_regs[kMaxRegisters][4]     = {};
float g_baseline[kMaxRegisters][4] = {};
bool  g_written[kMaxRegisters]     = {};   // has this register ever been set?
bool  g_have_baseline = false;

// Within-frame stability. See the header for why this is the signal that
// separates camera constants from per-object ones.
float    g_frame_first[kMaxRegisters][4] = {};
bool     g_seen_this_frame[kMaxRegisters]   = {};
bool     g_varied_this_frame[kMaxRegisters] = {};
bool     g_varied_last_frame[kMaxRegisters] = {};
unsigned g_writes[kMaxRegisters]            = {};   // writes during the last complete frame
unsigned g_writes_accum[kMaxRegisters]      = {};

unsigned g_highest_register = 0;
unsigned g_writes_this_frame = 0;
unsigned g_frame = 0;
int      g_snapshot_index = 0;

// Reads a 4x4 block either as rows-in-registers or columns-in-registers.
// HLSL packs matrices column-major into constant registers by default, so a
// view matrix commonly appears transposed relative to the C++ layout. We test
// both rather than guessing.
struct MatView {
    const float* m;
    bool transposed;
    float at(int r, int c) const { return transposed ? m[c * 4 + r] : m[r * 4 + c]; }
};

bool is_zero(float v) { return std::fabs(v) < kEps; }
bool is_one(float v)  { return std::fabs(v - 1.0f) < kEps; }

// Affine transform: last column (0,0,0,1). Translation lives in row 3.
bool is_affine(const MatView& v)
{
    return is_zero(v.at(0, 3)) && is_zero(v.at(1, 3)) &&
           is_zero(v.at(2, 3)) && is_one(v.at(3, 3));
}

// Orthonormal upper-left 3x3 => pure rotation, no scale. Strong signal for a
// view or world matrix.
bool is_rotation(const MatView& v)
{
    for (int r = 0; r < 3; ++r) {
        const float len = std::sqrt(v.at(r,0)*v.at(r,0) + v.at(r,1)*v.at(r,1) + v.at(r,2)*v.at(r,2));
        if (std::fabs(len - 1.0f) > 1e-3f) return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const float dot = v.at(a,0)*v.at(b,0) + v.at(a,1)*v.at(b,1) + v.at(a,2)*v.at(b,2);
            if (std::fabs(dot) > 1e-3f) return false;
        }
    }
    return true;
}

// D3D perspective projection, row-major:
//   [ w 0 0 0 ][ 0 h 0 0 ][ 0 0 q 1 ][ 0 0 -qn 0 ]
// Diagnostic: m33 == 0 while m23 is +/-1, and the off-diagonals are zero.
bool is_projection(const MatView& v)
{
    return is_zero(v.at(3, 3)) && std::fabs(std::fabs(v.at(2, 3)) - 1.0f) < 1e-3f &&
           is_zero(v.at(0, 1)) && is_zero(v.at(0, 2)) &&
           is_zero(v.at(1, 0)) && is_zero(v.at(1, 2));
}

// Returns a human-readable classification of the 4-register block at `base`,
// or an empty string if it does not resemble a transform.
void classify(unsigned base, char* out, size_t out_size)
{
    out[0] = '\0';
    if (base + 3 >= kMaxRegisters) return;
    const float* m = &g_regs[base][0];

    for (int pass = 0; pass < 2; ++pass) {
        const MatView v{ m, pass == 1 };
        const char* layout = pass == 0 ? "row-major" : "column-major/transposed";

        if (is_projection(v)) {
            // Recover vertical FOV from the [1][1] term: h = cot(fovY/2)
            const float h = v.at(1, 1);
            const float fov_y = (h > kEps) ? (2.0f * std::atan(1.0f / h) * 57.29578f) : 0.0f;
            _snprintf_s(out, out_size, _TRUNCATE,
                        "PROJECTION (%s)  fovY~%.1fdeg  aspect~%.3f",
                        layout, fov_y, (std::fabs(v.at(0,0)) > kEps) ? h / v.at(0,0) : 0.0f);
            return;
        }
        if (is_affine(v)) {
            const bool rot = is_rotation(v);
            _snprintf_s(out, out_size, _TRUNCATE,
                        "AFFINE %s(%s)  translation=(%.3f, %.3f, %.3f)",
                        rot ? "ROTATION-ONLY " : "", layout,
                        v.at(3, 0), v.at(3, 1), v.at(3, 2));
            return;
        }
    }
}

FILE* open_dump(const char* kind, char* path_out, size_t path_size)
{
    const std::string p = vrlog::dump_path(kind, static_cast<unsigned>(g_snapshot_index));
    _snprintf_s(path_out, path_size, _TRUNCATE, "%s", p.c_str());
    FILE* f = nullptr;
    fopen_s(&f, path_out, "w");
    return f;
}

const char* stability_tag(unsigned i)
{
    if (!g_writes[i])            return "  -    ";   // untouched last frame
    if (g_varied_last_frame[i])  return "VARYING";   // per-object: world, bones
    return "STABLE ";                                // camera-ish candidate
}

void write_register(FILE* f, unsigned i)
{
    char note[160];
    fprintf(f, "c%-3u %s x%-5u % 12.5f % 12.5f % 12.5f % 12.5f",
            i, stability_tag(i), g_writes[i],
            g_regs[i][0], g_regs[i][1], g_regs[i][2], g_regs[i][3]);
    // Every 4-register window, NOT only 4-aligned ones. BFBC2's camera matrices
    // sit at c185 and c189 - both congruent to 1 mod 4 - so an alignment
    // assumption hid the exact matrices this tool exists to find.
    classify(i, note, sizeof(note));
    if (note[0]) fprintf(f, "   <-- c%u..c%u %s", i, i + 3, note);
    fputc('\n', f);
}

void write_legend(FILE* f)
{
    fprintf(f, "Columns: register | within-frame stability | writes last frame | value\n\n");
    fprintf(f, "  STABLE  = many writes, always the same value for the whole frame.\n");
    fprintf(f, "            Camera candidates live here (view, projection, view-proj).\n");
    fprintf(f, "  VARYING = different values within one frame -> per-object data\n");
    fprintf(f, "            (world matrices, bone palettes). NOT the camera.\n\n");
    fprintf(f, "The camera is STABLE within a frame but CHANGES between frames as you move.\n");
    fprintf(f, "So: look for STABLE registers in the F11 diff. That is the intersection.\n\n");
}

bool key_pressed(int vk)
{
    // Edge-triggered: only fires on the transition to down.
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return fired;
}

} // namespace

void record(unsigned start_register, const float* data, unsigned vec4_count)
{
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_mutex);

    g_writes_this_frame += vec4_count;

    for (unsigned i = 0; i < vec4_count; ++i) {
        const unsigned reg = start_register + i;
        if (reg >= kMaxRegisters) {
            // Worth knowing about rather than silently ignoring - it would mean
            // the game uses more constants than we mirror.
            static bool warned = false;
            if (!warned) {
                warned = true;
                VRLOG("[constants] WARNING: write to c%u exceeds tracked range (%zu)", reg, kMaxRegisters);
            }
            break;
        }
        const float* src = data + i * 4;

        // Stability bookkeeping: first value seen this frame, and whether any
        // later write in the same frame disagreed with it.
        ++g_writes_accum[reg];
        if (!g_seen_this_frame[reg]) {
            g_seen_this_frame[reg] = true;
            std::memcpy(g_frame_first[reg], src, sizeof(float) * 4);
        } else if (!g_varied_this_frame[reg]) {
            for (int c = 0; c < 4; ++c) {
                if (std::fabs(g_frame_first[reg][c] - src[c]) > 1e-6f) { g_varied_this_frame[reg] = true; break; }
            }
        }

        std::memcpy(g_regs[reg], src, sizeof(float) * 4);
        g_written[reg] = true;
        if (reg > g_highest_register) g_highest_register = reg;
    }
}

void mark_baseline()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(g_baseline, g_regs, sizeof(g_regs));
    g_have_baseline = true;
    VRLOG("[constants] baseline marked at frame %u (highest register c%u)", g_frame, g_highest_register);
}

void dump_snapshot()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    char path[MAX_PATH];
    FILE* f = open_dump("snapshot", path, sizeof(path));
    if (!f) { VRLOG("[constants] failed to open snapshot file"); return; }

    fprintf(f, "bfbc2vr vertex shader constant snapshot\n");
    fprintf(f, "frame %u, highest register written c%u\n\n", g_frame, g_highest_register);
    write_legend(f);

    for (unsigned i = 0; i <= g_highest_register && i < kMaxRegisters; ++i) {
        if (g_written[i]) write_register(f, i);
    }
    fclose(f);

    VRLOG("[constants] snapshot %02d written -> %s", g_snapshot_index, path);
    ++g_snapshot_index;
}

void dump_diff_vs_baseline()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_have_baseline) {
        VRLOG("[constants] no baseline yet - press F10 first");
        return;
    }

    char path[MAX_PATH];
    FILE* f = open_dump("diff", path, sizeof(path));
    if (!f) { VRLOG("[constants] failed to open diff file"); return; }

    fprintf(f, "bfbc2vr constant diff vs baseline\n");
    fprintf(f, "frame %u\n\n", g_frame);
    write_legend(f);

    // Two passes. STABLE-and-changed is the answer we are hunting; everything
    // else is per-object churn that changes whenever the camera moves and would
    // otherwise bury the signal.
    unsigned changed = 0, stable_changed = 0;

    for (int pass = 0; pass < 2; ++pass) {
        const bool want_stable = (pass == 0);
        fprintf(f, "%s\n", want_stable
            ? "=== STABLE registers that changed - CAMERA CANDIDATES ==========="
            : "=== VARYING registers that changed - per-object churn, ignore ===");

        unsigned n = 0;
        for (unsigned i = 0; i <= g_highest_register && i < kMaxRegisters; ++i) {
            if (!g_written[i]) continue;
            if (g_varied_last_frame[i] == want_stable) continue;

            bool differs = false;
            for (int c = 0; c < 4; ++c) {
                if (std::fabs(g_regs[i][c] - g_baseline[i][c]) > 1e-5f) { differs = true; break; }
            }
            if (!differs) continue;

            ++n; ++changed;
            if (want_stable) ++stable_changed;

            fprintf(f, "c%-3u  was  % 12.5f % 12.5f % 12.5f % 12.5f\n", i,
                    g_baseline[i][0], g_baseline[i][1], g_baseline[i][2], g_baseline[i][3]);
            fprintf(f, "      now  % 12.5f % 12.5f % 12.5f % 12.5f",
                    g_regs[i][0], g_regs[i][1], g_regs[i][2], g_regs[i][3]);

            char note[160];
            classify(i, note, sizeof(note));
            if (note[0]) fprintf(f, "   <-- c%u..c%u %s", i, i + 3, note);
            fprintf(f, "\n\n");
        }
        if (!n) fprintf(f, "(none)\n\n");
    }

    fprintf(f, "\n%u registers changed, %u of them STABLE.\n", changed, stable_changed);
    if (stable_changed == 0) {
        fprintf(f, "\nNo stable register changed. Either the camera did not actually move\n"
                   "between F10 and F11, or this engine pre-multiplies the camera into\n"
                   "per-object matrices - in which case the view transform must be\n"
                   "recovered by factoring a VARYING register instead.\n");
    }
    fclose(f);

    VRLOG("[constants] diff %02d written (%u changed) -> %s", g_snapshot_index, changed, path);
    ++g_snapshot_index;
}

void on_present()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_frame;

        // Roll per-frame stability state. Dumps below read the just-completed
        // frame, so this has to happen before the hotkeys are serviced.
        unsigned stable = 0, varying = 0;
        for (unsigned i = 0; i < kMaxRegisters; ++i) {
            g_varied_last_frame[i] = g_varied_this_frame[i];
            g_writes[i]            = g_writes_accum[i];
            if (g_seen_this_frame[i]) { if (g_varied_this_frame[i]) ++varying; else ++stable; }
            g_seen_this_frame[i]   = false;
            g_varied_this_frame[i] = false;
            g_writes_accum[i]      = 0;
        }

        // Low-volume heartbeat so the log stays readable.
        if (g_frame % 600 == 0) {
            VRLOG("[constants] frame %u: %u vec4 writes, highest c%u, %u stable / %u varying",
                  g_frame, g_writes_this_frame, g_highest_register, stable, varying);
        }
        g_writes_this_frame = 0;
    }

    if (key_pressed(VK_F9))  dump_snapshot();
    if (key_pressed(VK_F10)) mark_baseline();
    if (key_pressed(VK_F11)) dump_diff_vs_baseline();
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    if (strcmp(cmd, "consts") != 0) return false;
    if (args && _stricmp(args, "baseline") == 0) {
        mark_baseline();
        _snprintf_s(reply, n, _TRUNCATE, "consts: baseline marked (F10)");
    } else if (args && _stricmp(args, "diff") == 0) {
        dump_diff_vs_baseline();
        _snprintf_s(reply, n, _TRUNCATE, "consts: diff vs baseline written (F11)");
    } else {
        dump_snapshot();
        _snprintf_s(reply, n, _TRUNCATE, "consts: snapshot written (F9)");
    }
    return true;
}

} // namespace vrconst
