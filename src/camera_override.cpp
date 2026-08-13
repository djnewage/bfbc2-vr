#include "camera_override.h"
#include "logger.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace camover {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegPerSecond = 25.0f;   // slow enough to read as deliberate

bool     g_probe_on   = false;
unsigned g_probe_base = kProbeDefault;
float    g_angle_rad  = 0.0f;

// Write-pattern evidence. "Which (start,count) shapes arrive per frame, and
// how many did the probe actually modify?" separates a base that hits one
// consistent matrix slot from one that corrupts mixed data - the difference
// between the world rotating and the screen tearing.
struct WriteShape { unsigned start, count, calls; };
constexpr size_t kMaxShapes = 32;
WriteShape g_shapes[kMaxShapes];
size_t     g_shape_count    = 0;
unsigned   g_modified_this_frame = 0;
unsigned   g_modified_last_frame = 0;

void note_shape(unsigned start, unsigned count)
{
    for (size_t i = 0; i < g_shape_count; ++i) {
        if (g_shapes[i].start == start && g_shapes[i].count == count) { ++g_shapes[i].calls; return; }
    }
    if (g_shape_count < kMaxShapes) g_shapes[g_shape_count++] = { start, count, 1 };
}

// Row-major, row-vector convention (v' = v * M) - what Phase 2 measured.
using Mat4 = float[16];

float g_vp[16]         = {};   // last seen view-projection, from c185-c188
bool  g_have_vp        = false;
float g_eye[3]         = {};   // camera position, from c192
bool  g_have_eye       = false;
float g_correction[16] = {};   // VP^-1 * R * VP, rebuilt once per frame
bool  g_have_correction = false;

inline float& at(Mat4 m, int r, int c)       { return m[r * 4 + c]; }
inline float  at(const Mat4 m, int r, int c) { return m[r * 4 + c]; }

void identity(Mat4 m)
{
    std::memset(m, 0, sizeof(float) * 16);
    at(m, 0, 0) = at(m, 1, 1) = at(m, 2, 2) = at(m, 3, 3) = 1.0f;
}

void multiply(const Mat4 a, const Mat4 b, Mat4 out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += at(a, r, k) * at(b, k, c);
            at(out, r, c) = sum;
        }
}

void rotation_y(float rad, Mat4 m)
{
    identity(m);
    const float s = std::sin(rad), c = std::cos(rad);
    at(m, 0, 0) =  c; at(m, 0, 2) = -s;
    at(m, 2, 0) =  s; at(m, 2, 2) =  c;
}

void translation(float x, float y, float z, Mat4 m)
{
    identity(m);
    at(m, 3, 0) = x; at(m, 3, 1) = y; at(m, 3, 2) = z;
}

// General 4x4 inverse (cofactor expansion). A projection matrix is not affine,
// so the cheap affine-inverse shortcut does not apply here.
bool invert(const Mat4 m, Mat4 out)
{
    float inv[16];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9]  + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9]  - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-12f) return false;

    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

bool key_pressed(int vk)
{
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return fired;
}

bool covers(unsigned start, unsigned count, unsigned base)
{
    return start <= base && (start + count) >= (base + 4);
}

// CORRECTION = VP^-1 * RotAboutEye * VP.
// Valid for any matrix ending in clip space, so the same value corrects a
// per-object WVP and the global VP alike.
void rebuild_correction()
{
    g_have_correction = false;
    if (!g_have_vp || !g_have_eye) return;

    Mat4 vp_inv;
    if (!invert(g_vp, vp_inv)) {
        static bool warned = false;
        if (!warned) { warned = true; VRLOG("[override] VP is singular - cannot build correction"); }
        return;
    }

    Mat4 to_origin, rot, back, r, tmp;
    translation(-g_eye[0], -g_eye[1], -g_eye[2], to_origin);
    rotation_y(g_angle_rad, rot);
    translation(g_eye[0], g_eye[1], g_eye[2], back);
    multiply(to_origin, rot, tmp);
    multiply(tmp, back, r);          // rotate about the eye, not the origin

    multiply(vp_inv, r, tmp);
    multiply(tmp, g_vp, g_correction);
    g_have_correction = true;
}

} // namespace

bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch)
{
    if (!data || vec4_count == 0) return false;

    note_shape(start_register, vec4_count);

    // Keep VP and eye current regardless of probe state - the correction is
    // built from them and they must track the game's real camera.
    if (covers(start_register, vec4_count, kViewProjBase)) {
        std::memcpy(g_vp, data + (kViewProjBase - start_register) * 4, sizeof(g_vp));
        g_have_vp = true;
    }
    if (covers(start_register, vec4_count, kCamWorldBase)) {
        const float* row3 = data + (kCamWorldBase + 3 - start_register) * 4;
        g_eye[0] = row3[0]; g_eye[1] = row3[1]; g_eye[2] = row3[2];
        g_have_eye = true;
    }

    if (!g_probe_on || !g_have_correction) return false;
    if (!covers(start_register, vec4_count, g_probe_base)) return false;

    scratch.assign(data, data + vec4_count * 4);

    float* target = scratch.data() + (g_probe_base - start_register) * 4;
    Mat4 out;
    multiply(reinterpret_cast<const float(&)[16]>(*target), g_correction, out);
    std::memcpy(target, out, sizeof(out));
    ++g_modified_this_frame;
    return true;
}

void on_present()
{
    static ULONGLONG last_tick = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last_tick ? (now - last_tick) / 1000.0f : 0.0f;
    last_tick = now;

    if (g_probe_on) {
        g_angle_rad += dt * kDegPerSecond * kPi / 180.0f;
        if (g_angle_rad > 2.0f * kPi) g_angle_rad -= 2.0f * kPi;
    }
    rebuild_correction();

    g_modified_last_frame = g_modified_this_frame;
    g_modified_this_frame = 0;

    // Evidence heartbeat: write shapes + how much the probe touched. Every 300
    // frames while probing, so a slow manual sweep leaves a readable trail.
    static unsigned frames = 0;
    ++frames;
    if (g_probe_on && frames % 300 == 0) {
        VRLOG("[probe] base c%u: modified %u writes last frame", g_probe_base, g_modified_last_frame);
        for (size_t i = 0; i < g_shape_count; ++i) {
            if (g_shapes[i].calls > 0) {
                VRLOG("[shapes]   start=c%-3u count=%-3u  x%u/frame",
                      g_shapes[i].start, g_shapes[i].count, g_shapes[i].calls);
            }
            g_shapes[i].calls = 0;
        }
    } else if (frames % 300 == 0) {
        for (size_t i = 0; i < g_shape_count; ++i) g_shapes[i].calls = 0;
    }

    if (key_pressed(VK_F7)) {
        g_probe_on = !g_probe_on;
        VRLOG("[probe] %s at base c%u  (vp=%d eye=%d correction=%d, modified %u last frame)",
              g_probe_on ? "ON" : "off", g_probe_base,
              (int)g_have_vp, (int)g_have_eye, (int)g_have_correction, g_modified_last_frame);
    }
    if (key_pressed(VK_F8)) {
        ++g_probe_base;
        VRLOG("[probe] base -> c%u   %s", g_probe_base, g_probe_on ? "(probe ON)" : "(probe off - press F7)");
    }
    if (key_pressed(VK_F6)) {
        if (g_probe_base > 0) --g_probe_base;
        VRLOG("[probe] base -> c%u   %s", g_probe_base, g_probe_on ? "(probe ON)" : "(probe off - press F7)");
    }
    if (key_pressed(VK_F5)) {
        g_angle_rad = 0.0f;
        VRLOG("[probe] angle reset; base c%u, eye=(%.2f, %.2f, %.2f)",
              g_probe_base, g_eye[0], g_eye[1], g_eye[2]);
    }
}

} // namespace camover
