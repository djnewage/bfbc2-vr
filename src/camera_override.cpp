#include "camera_override.h"
#include "shader_registry.h"
#include "vr_tracking.h"
#include "logger.h"

#include <openvr.h>   // GetProjectionRaw, for auto FOV widening

#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace camover {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegPerSecond = 25.0f;   // slow enough to read as deliberate

bool  g_correct_on = false;
bool  g_transposed = true;    // HLSL default packing is column-major (stored = M^T)
float g_angle_rad  = 0.0f;

// Head tracking. When an HMD pose is available the correction R comes from the
// head's yaw/pitch delta against a reference captured at enable time; the
// synthetic spin only runs when there is no pose, so the old test still works
// with SteamVR closed. Axis signs are field-calibrated (F8/F6) because the
// mapping between OpenVR axes and this engine's axes is empirical.
bool  g_hmd_active   = false;   // pose seen and reference captured
float g_ref_yaw      = 0.0f;
float g_ref_pitch    = 0.0f;
// Field-calibrated 2026-08-13: default signs were inverted on both axes
// (user confirmed one F8 and one F6 press fixed look direction).
float g_yaw_sign     = -1.0f;
float g_pitch_sign   = -1.0f;
float g_hmd_yaw      = 0.0f;    // current delta, radians
float g_hmd_pitch    = 0.0f;

// 6DOF. The HMD's positional delta from its reference, mapped through the game
// camera's basis into world space. Frostbite is metric and OpenVR reports
// meters, so this is 1:1 - lean 20cm in the room, lean 20cm in the world.
// END toggles; F5 recenters position along with orientation.
bool  g_pos_enabled  = true;
float g_ref_pos[3]   = {};
float g_hmd_dpos[3]  = {};      // delta in OpenVR axes (+X right, +Y up, -Z fwd)

// Alternate-eye rendering state. The IPD is in WORLD units and the engine's
// world scale is not yet measured, so it is runtime-adjustable: if the world
// feels like a miniature, the offset is too big; like a giant's world, too
// small. F2/F3 tune it, F1 swaps eyes (swapped eyes = depth pops inward).
int   g_frame_eye    = 0;       // eye the CURRENT frame is being rendered for
int   g_last_eye     = 0;       // eye the just-presented frame used
float g_ipd_world    = 0.064f;  // world-units offset between eyes
float g_eye_swap     = 1.0f;    // -1 swaps left/right

// Zero-calibration: IPD comes from the headset's own setting (Frostbite is
// metric, so meters ARE world units), and tracking engages by itself when a
// pose appears. F2/F3 switch to manual mode for world-scale experiments;
// F7 remains the master toggle and remembers an explicit off.
bool g_ipd_manual    = false;
bool g_auto_engaged  = false;   // auto-enable fired once
bool g_user_disabled = false;   // user explicitly F7'd off - respect it

// Yaw/pitch of the HMD forward vector in OpenVR space (right-handed, Y-up,
// forward -Z). Yaw 0 = facing -Z; positive = turning toward -X (left).
void hmd_yaw_pitch(float& yaw, float& pitch)
{
    float r[9];
    vrtrack::orientation(r);
    // forward = R * (0,0,-1), rows are r[0..2], r[3..5], r[6..8]
    const float fx = -r[2], fy = -r[5], fz = -r[8];
    yaw   = std::atan2(fx, -fz);
    const float len = std::sqrt(fx * fx + fz * fz);
    pitch = std::atan2(fy, len > 1e-6f ? len : 1e-6f);
}

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
float g_vp_inv[16]     = {};   // its inverse, refreshed with the correction
bool  g_have_vp        = false;
bool  g_have_vp_inv    = false;

// Fingerprint cache for "constants"-blob shaders: (shader, write start) ->
// matrix offset within the write in vec4s, or -1 for "scanned, none found".
// Guarded by g_fp_mutex; only touched when the active shader is uncharted.
std::mutex g_fp_mutex;
std::map<std::pair<void*, unsigned>, int> g_fingerprints;
unsigned g_fp_found = 0, g_fp_rejected = 0;
float g_eye[3]         = {};   // camera position, from c192
float g_cam_right[3]   = { 1, 0, 0 };   // camera right axis, from c189 row 0
float g_cam_rows[9]    = { 1,0,0, 0,1,0, 0,0,1 };   // full camera-to-world 3x3
bool  g_have_eye       = false;

// FOV widening: scale the camera's x/y axes down inside the correction, so the
// game renders a WIDER field into the same image. game_proj_tangents reports
// the widened tangents, so the compositor keeps displaying at correct angular
// size - the black border shrinks instead of the image stretching.
//
// AUTO: the factors are computed from the headset's own frustum
// (GetProjectionRaw) against the game's native tangents, per axis. They must
// be per-axis because the Index's frustum is nearly square (~2.6 x 2.8 in
// tangent units) while the game renders 16:9 - a uniform factor would either
// leave the top and bottom black or waste enormous horizontal field.
//
// Cost, stated plainly: the engine still CPU-culls at its native frustum, so
// edge pop-in grows with these factors, and the same backbuffer now covers
// more degrees, so the image softens. PgDn trades field back for sharpness.
float g_fov_widen_x = 1.0f;
float g_fov_widen_y = 1.0f;
float g_fov_manual  = 1.0f;   // PgUp/PgDn multiplier on top of auto
bool  g_fov_auto    = true;

// Viewmodel push. Flat FPS games compose the first-person weapon for a 2D
// screen and park it ~30cm from the camera; at true VR scale that is against
// your face. Real VR puts a held weapon at arm's length. Draws whose recovered
// world position sits within kRadius of the eye get an extra shove along the
// camera's forward axis - moved out, not scaled, so it subtends a smaller
// angle the way a real arm's-length object would.
// Minus/Equals keys tune it live.
float g_vm_push   = 0.45f;   // world units (meters) forward
float g_vm_radius = 2.00f;   // treat draws inside this as viewmodel
float g_correction_near[16] = {};
bool  g_have_correction_near = false;
unsigned g_vm_hits = 0, g_vm_hits_last = 0;   // did detection actually fire?
float    g_vm_nearest = 1e9f, g_vm_nearest_last = 0.0f;
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

void transpose(const Mat4 m, Mat4 out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            at(out, r, c) = at(m, c, r);
}

// Rodrigues rotation about an arbitrary (normalized) axis, row-vector form.
void rotation_axis(const float axis[3], float rad, Mat4 m)
{
    const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    identity(m);
    at(m,0,0) = t*x*x + c;   at(m,0,1) = t*x*y + s*z; at(m,0,2) = t*x*z - s*y;
    at(m,1,0) = t*x*y - s*z; at(m,1,1) = t*y*y + c;   at(m,1,2) = t*y*z + s*x;
    at(m,2,0) = t*x*z + s*y; at(m,2,1) = t*y*z - s*x; at(m,2,2) = t*z*z + c;
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

// The game's native (un-widened) projection half-angle tangents, recovered
// from VP: rows 0/1 are rotation rows scaled by the projection's x/y factors,
// so tangent = 1/|row|.
bool base_tangents(float& th, float& tv)
{
    if (!g_have_vp) return false;
    const float lx = std::sqrt(g_vp[0]*g_vp[0] + g_vp[1]*g_vp[1] + g_vp[2]*g_vp[2]);
    const float ly = std::sqrt(g_vp[4]*g_vp[4] + g_vp[5]*g_vp[5] + g_vp[6]*g_vp[6]);
    if (lx < 1e-6f || ly < 1e-6f) return false;
    th = 1.0f / lx;
    tv = 1.0f / ly;
    return true;
}

// Match the rendered field to the headset's frustum, per axis. Recomputed
// rarely - the frustum is a device constant, but VP is only known once the
// game starts drawing.
void update_auto_fov()
{
    if (!g_fov_auto) return;
    auto* sys = vrtrack::system();
    if (!sys) return;

    float th = 0.0f, tv = 0.0f;
    if (!base_tangents(th, tv)) return;

    float need_h = 0.0f, need_v = 0.0f;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        sys->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
        need_h = (std::max)(need_h, (std::max)(std::fabs(l), std::fabs(r)));
        need_v = (std::max)(need_v, (std::max)(std::fabs(t), std::fabs(b)));
    }
    if (need_h < 1e-6f || need_v < 1e-6f) return;

    const float wx = (std::min)((std::max)(need_h / th, 1.0f), 3.0f);
    const float wy = (std::min)((std::max)(need_v / tv, 1.0f), 4.0f);

    if (std::fabs(wx - g_fov_widen_x) > 0.01f || std::fabs(wy - g_fov_widen_y) > 0.01f) {
        g_fov_widen_x = wx;
        g_fov_widen_y = wy;
        VRLOG("[fov] auto: game tangents h=%.4f v=%.4f, headset needs h=%.4f v=%.4f -> widen %.2fx / %.2fx",
              th, tv, need_h, need_v, wx, wy);
    }
}

// Is this stored transform block a draw sitting right on top of the camera?
// World = WVP * VP^-1, and its translation row is the object's world position.
bool is_near_camera(const float* stored_block)
{
    if (!g_have_vp_inv || !g_have_eye) return false;

    Mat4 wvp, world;
    if (g_transposed) transpose(reinterpret_cast<const float(&)[16]>(*stored_block), wvp);
    else              std::memcpy(wvp, stored_block, sizeof(wvp));
    multiply(wvp, g_vp_inv, world);

    const float w = at(world, 3, 3);
    if (std::fabs(w) < 1e-4f) return false;
    const float dx = at(world, 3, 0) / w - g_eye[0];
    const float dy = at(world, 3, 1) / w - g_eye[1];
    const float dz = at(world, 3, 2) / w - g_eye[2];

    // Track the closest draw seen, so the log says whether ANY geometry is
    // near the camera - if the nearest is metres away, the viewmodel's world
    // matrix does not encode its position and distance is the wrong test.
    const float d2 = dx*dx + dy*dy + dz*dz;
    const float d  = std::sqrt(d2);
    if (d < g_vm_nearest) g_vm_nearest = d;

    if (d2 < (g_vm_radius * g_vm_radius)) { ++g_vm_hits; return true; }
    return false;
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

// Does the 4-register window at `window` (transposed storage) hold a WVP?
// If S^T really is World * VP, then S^T * VP^-1 recovers World, which must be
// affine: last column (0,0,0,1). Bone rows, packed params, and lighting data
// do not survive that test.
bool window_is_wvp(const float* window)
{
    if (!g_have_vp_inv) return false;

    Mat4 m, world;
    transpose(reinterpret_cast<const float(&)[16]>(*window), m);
    multiply(m, g_vp_inv, world);

    const float w33 = at(world, 3, 3);
    if (std::fabs(w33 - 1.0f) > 0.02f) return false;
    for (int r = 0; r < 3; ++r) {
        if (std::fabs(at(world, r, 3)) > 0.02f) return false;
    }
    // Degenerate guard: an all-zero window would pass the column test trivially
    // if VP^-1 had zeros in the right places; require a non-trivial diagonal.
    if (std::fabs(at(world, 0, 0)) + std::fabs(at(world, 1, 1)) + std::fabs(at(world, 2, 2)) < 0.1f) return false;
    return true;
}

// For a write from an uncharted ("constants"-blob) shader: find the WVP window
// within it, scanning once per (shader, write start) and caching the result.
int fingerprint_offset(void* shader, unsigned start, const float* data, unsigned vec4_count)
{
    const auto key = std::make_pair(shader, start);
    {
        std::lock_guard<std::mutex> lock(g_fp_mutex);
        auto it = g_fingerprints.find(key);
        if (it != g_fingerprints.end()) return it->second;
    }

    int found = -1;
    if (vec4_count >= 4) {
        for (unsigned off = 0; off + 4 <= vec4_count; ++off) {
            if (window_is_wvp(data + off * 4)) { found = static_cast<int>(off); break; }
        }
    }

    std::lock_guard<std::mutex> lock(g_fp_mutex);
    g_fingerprints[key] = found;
    if (found >= 0) {
        ++g_fp_found;
        VRLOG("[fingerprint] shader %p write c%u+%u: WVP at offset %d (c%u)",
              shader, start, vec4_count, found, start + found);
    } else {
        ++g_fp_rejected;
    }
    return found;
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
        g_have_vp_inv = false;
        return;
    }
    std::memcpy(g_vp_inv, vp_inv, sizeof(g_vp_inv));
    g_have_vp_inv = true;

    // The rotation: HMD yaw/pitch delta when tracking, synthetic spin otherwise.
    Mat4 rot;
    if (g_hmd_active) {
        const float wy[3] = { 0.0f, 1.0f, 0.0f };
        Mat4 ry, rp;
        rotation_axis(wy, g_yaw_sign * g_hmd_yaw, ry);
        rotation_axis(g_cam_right, g_pitch_sign * g_hmd_pitch, rp);
        multiply(rp, ry, rot);   // pitch about camera right, then yaw about world up
    } else {
        rotation_y(g_angle_rad, rot);
    }

    Mat4 to_origin, back, r, tmp;
    translation(-g_eye[0], -g_eye[1], -g_eye[2], to_origin);
    translation(g_eye[0], g_eye[1], g_eye[2], back);
    multiply(to_origin, rot, tmp);
    multiply(tmp, back, r);          // rotate about the eye, not the origin

    // 6DOF: map the head's positional delta through the camera basis and move
    // the WORLD the opposite way, which is the same as moving the camera.
    // OpenVR is +X right / +Y up / -Z forward, hence the negated Z term.
    if (g_pos_enabled && g_hmd_active) {
        const float* right = &g_cam_rows[0];
        const float* up    = &g_cam_rows[3];
        const float* fwd   = &g_cam_rows[6];
        float off[3];
        for (int i = 0; i < 3; ++i) {
            off[i] = g_hmd_dpos[0] * right[i]
                   + g_hmd_dpos[1] * up[i]
                   + (-g_hmd_dpos[2]) * fwd[i];
        }
        Mat4 move, r2;
        translation(-off[0], -off[1], -off[2], move);
        multiply(r, move, r2);
        std::memcpy(r, r2, sizeof(r));
    }

    // FOV widening: K = V * S * M_cw, a camera-space lateral shrink conjugated
    // into world space. Scaling camera x/y by 1/widen before projection makes
    // the same image cover widen-times the field. V and M_cw carry the eye
    // position, so no extra recentering is needed.
    const float wx = g_fov_widen_x * g_fov_manual;
    const float wy = g_fov_widen_y * g_fov_manual;
    if ((wx > 1.001f || wy > 1.001f) && g_have_eye) {
        Mat4 mcw, view, scale, tmp2, k;
        identity(mcw);
        for (int r2 = 0; r2 < 3; ++r2)
            for (int c2 = 0; c2 < 3; ++c2)
                at(mcw, r2, c2) = g_cam_rows[r2 * 3 + c2];
        at(mcw, 3, 0) = g_eye[0]; at(mcw, 3, 1) = g_eye[1]; at(mcw, 3, 2) = g_eye[2];

        // Affine inverse of a rotation+translation: linear part transposes,
        // translation is -eye through the transposed rotation.
        identity(view);
        for (int r2 = 0; r2 < 3; ++r2)
            for (int c2 = 0; c2 < 3; ++c2)
                at(view, r2, c2) = g_cam_rows[c2 * 3 + r2];
        for (int c2 = 0; c2 < 3; ++c2) {
            at(view, 3, c2) = -(g_eye[0] * at(view, 0, c2) +
                                g_eye[1] * at(view, 1, c2) +
                                g_eye[2] * at(view, 2, c2));
        }

        identity(scale);
        at(scale, 0, 0) = 1.0f / wx;
        at(scale, 1, 1) = 1.0f / wy;

        multiply(view, scale, tmp2);
        multiply(tmp2, mcw, k);

        Mat4 combined;
        multiply(k, rot, combined);
        std::memcpy(rot, combined, sizeof(rot));
    }

    // Stereo eye offset: shift the camera half an IPD along its right axis.
    // Shifting the CAMERA right by d == shifting the WORLD left by d, so the
    // world translation is -d for the right eye, +d for the left.
    if (g_hmd_active) {
        // Note: F1 eye-swap is applied at SUBMISSION (last_rendered_eye), not
        // here - applying it in both places would cancel itself out.
        const float half = 0.5f * g_ipd_world;
        const float side = (g_frame_eye == 0) ? +half : -half;
        Mat4 eye_shift, r2;
        translation(side * g_cam_right[0], side * g_cam_right[1], side * g_cam_right[2], eye_shift);
        multiply(r, eye_shift, r2);
        std::memcpy(r, r2, sizeof(r));
    }

    multiply(vp_inv, r, tmp);
    multiply(tmp, g_vp, g_correction);
    g_have_correction = true;

    // Second correction for near-camera draws: the same transform with an
    // extra world-space shove along the camera's forward axis.
    g_have_correction_near = false;
    if (g_vm_push > 0.001f && g_have_eye) {
        const float* fwd = &g_cam_rows[6];   // camera-to-world row 2 = forward
        Mat4 push, r_near, tmp3;
        translation(fwd[0] * g_vm_push, fwd[1] * g_vm_push, fwd[2] * g_vm_push, push);
        multiply(push, r, r_near);           // push first, then the VR transform
        multiply(vp_inv, r_near, tmp3);
        multiply(tmp3, g_vp, g_correction_near);
        g_have_correction_near = true;
    }
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
        const float* rows = data + (kCamWorldBase - start_register) * 4;
        const float* row3 = rows + 3 * 4;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                g_cam_rows[r * 3 + c] = rows[r * 4 + c];
        g_cam_right[0] = g_cam_rows[0]; g_cam_right[1] = g_cam_rows[1]; g_cam_right[2] = g_cam_rows[2];
        g_eye[0] = row3[0]; g_eye[1] = row3[1]; g_eye[2] = row3[2];
        g_have_eye = true;
    }

    if (!g_correct_on || !g_have_correction) return false;

    // Spans the ACTIVE shader's own constant table declares as clip-space
    // transforms; for "constants"-blob shaders that declare nothing useful,
    // fall back to a cached value fingerprint (see window_is_wvp).
    shaderreg::Span spans[shaderreg::kMaxSpans];
    size_t n = shaderreg::active_transform_spans(spans);
    if (n == 0) {
        if (!shaderreg::active_shader_uncharted()) return false;
        const int off = fingerprint_offset(shaderreg::active_shader(), start_register, data, vec4_count);
        if (off < 0) return false;
        spans[0] = { start_register + static_cast<unsigned>(off), 4 };
        n = 1;
    }

    bool any = false;
    for (size_t s = 0; s < n; ++s) {
        if (!covers(start_register, vec4_count, spans[s].start)) continue;

        if (!any) scratch.assign(data, data + vec4_count * 4);
        any = true;

        float* target = scratch.data() + (spans[s].start - start_register) * 4;

        // Viewmodel draws take the pushed-out correction.
        const float* corr = g_correction;
        if (g_have_correction_near && is_near_camera(target)) corr = g_correction_near;

        Mat4 out;
        if (g_transposed) {
            // Stored block is M^T (HLSL column-major packing):
            //   (M * C)^T = C^T * M^T  ->  left-multiply by C^T
            Mat4 ct;
            transpose(reinterpret_cast<const float(&)[16]>(*corr), ct);
            multiply(ct, reinterpret_cast<const float(&)[16]>(*target), out);
        } else {
            // Stored block is M row-by-row: right-multiply as derived.
            multiply(reinterpret_cast<const float(&)[16]>(*target),
                     reinterpret_cast<const float(&)[16]>(*corr), out);
        }
        std::memcpy(target, out, sizeof(out));
        ++g_modified_this_frame;
    }
    return any;
}

void on_present()
{
    static ULONGLONG last_tick = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last_tick ? (now - last_tick) / 1000.0f : 0.0f;
    last_tick = now;

    // Alternate-eye cadence, driven by CONTENT rather than Present count. The
    // game sometimes Presents without rendering (menu overlays, pump frames);
    // toggling the eye on those churns the phase - the fresh copy keeps
    // landing in one eye while the other eye's texture goes stale and gets
    // resubmitted as a seconds-old view (the head-still double vision the
    // stereo screenshots showed). Only a frame that actually contained
    // corrected 3D draws advances the eye.
    // g_modified_this_frame still holds the just-finished frame's count here -
    // the roll that zeroes it happens further down in this function.
    const bool had_3d = (g_modified_this_frame > 0);
    if (had_3d) {
        g_last_eye  = g_frame_eye;
        g_frame_eye ^= 1;
    }

    // Head tracking: refresh the pose and, while enabled, convert it to a
    // yaw/pitch delta against the reference captured when tracking engaged.
    vrtrack::update();

    // Auto-engage: the first valid pose turns VR mode on, like a native VR
    // title - no keypress required. An explicit F7-off stays off.
    if (!g_auto_engaged && !g_user_disabled && !g_correct_on && vrtrack::have_pose()) {
        g_correct_on = true;
        g_auto_engaged = true;
        VRLOG("[vr] auto-engaged - HMD pose available");
    }

    update_auto_fov();

    // Auto-IPD: track the headset's real setting until the user goes manual.
    if (!g_ipd_manual) {
        const float real = vrtrack::user_ipd_meters();
        if (real > 0.0f && std::fabs(real - g_ipd_world) > 0.0005f) {
            g_ipd_world = real;
            VRLOG("[stereo] IPD from headset: %.4f m", g_ipd_world);
        }
    }

    if (g_correct_on && vrtrack::have_pose()) {
        float yaw, pitch;
        hmd_yaw_pitch(yaw, pitch);
        float pos[3];
        vrtrack::position(pos);
        if (!g_hmd_active) {
            g_hmd_active = true;
            g_ref_yaw = yaw; g_ref_pitch = pitch;
            std::memcpy(g_ref_pos, pos, sizeof(g_ref_pos));
            VRLOG("[vr] head tracking engaged (ref yaw=%.1f pitch=%.1f deg, pos %.2f/%.2f/%.2f)",
                  yaw * 180.0f / kPi, pitch * 180.0f / kPi, pos[0], pos[1], pos[2]);
        }
        g_hmd_yaw   = yaw   - g_ref_yaw;
        g_hmd_pitch = pitch - g_ref_pitch;
        for (int i = 0; i < 3; ++i) g_hmd_dpos[i] = pos[i] - g_ref_pos[i];
    } else if (g_hmd_active && !g_correct_on) {
        g_hmd_active = false;
        VRLOG("[vr] head tracking disengaged");
    }

    if (g_correct_on && !g_hmd_active) {
        g_angle_rad += dt * kDegPerSecond * kPi / 180.0f;
        if (g_angle_rad > 2.0f * kPi) g_angle_rad -= 2.0f * kPi;
    }
    rebuild_correction();

    g_modified_last_frame = g_modified_this_frame;
    g_modified_this_frame = 0;

    static unsigned frames = 0;
    ++frames;
    g_vm_hits_last    = g_vm_hits;
    g_vm_nearest_last = (g_vm_nearest < 1e8f) ? g_vm_nearest : -1.0f;
    g_vm_hits = 0;
    g_vm_nearest = 1e9f;

    if (g_correct_on && frames % 300 == 0) {
        VRLOG("[correct] %s: modified %u transform writes last frame (fingerprints: %u found, %u none)",
              g_transposed ? "transposed" : "row-registers", g_modified_last_frame,
              g_fp_found, g_fp_rejected);
        VRLOG("[viewmodel] push=%.2fm radius=%.2fm: %u draws matched, nearest draw %.3fm",
              g_vm_push, g_vm_radius, g_vm_hits_last, g_vm_nearest_last);
    }

    // Stereo phase forensics: eye parity vs corrected-write count per frame.
    // If eye parity and frame content ever fall out of phase - empty frames,
    // double Presents - each panel receives the OTHER eye's viewpoint half the
    // time, which doubles the world permanently and no calibration can fix it.
    // A short burst every ~5s makes that visible in the log.
    if (g_correct_on && g_hmd_active && frames % 300 < 8) {
        VRLOG("[phase] frame %u eye=%d modified=%u ipd=%.4f", frames, g_last_eye, g_modified_last_frame, g_ipd_world);
    }
    if (frames % 300 == 0) {
        for (size_t i = 0; i < g_shape_count; ++i) g_shapes[i].calls = 0;
    }

    if (key_pressed(VK_F7)) {
        g_correct_on = !g_correct_on;
        g_user_disabled = !g_correct_on;   // an explicit off suppresses auto-engage
        VRLOG("[correct] %s  (convention=%s, vp=%d eye=%d correction=%d)",
              g_correct_on ? "ON" : "off", g_transposed ? "transposed" : "row-registers",
              (int)g_have_vp, (int)g_have_eye, (int)g_have_correction);
    }
    // With head tracking live, F8/F6 calibrate axis signs; the transposed
    // convention is settled and only needs changing if geometry breaks again.
    if (key_pressed(VK_F8)) {
        if (g_hmd_active) {
            g_yaw_sign = -g_yaw_sign;
            VRLOG("[vr] yaw sign -> %+0.f", g_yaw_sign);
        } else {
            g_transposed = !g_transposed;
            VRLOG("[correct] convention -> %s", g_transposed ? "transposed" : "row-registers");
        }
    }
    if (key_pressed(VK_F6) && g_hmd_active) {
        g_pitch_sign = -g_pitch_sign;
        VRLOG("[vr] pitch sign -> %+0.f", g_pitch_sign);
    }
    if (key_pressed(VK_F5)) {
        g_angle_rad = 0.0f;
        if (g_hmd_active) {
            float yaw, pitch, pos[3];
            hmd_yaw_pitch(yaw, pitch);
            vrtrack::position(pos);
            g_ref_yaw = yaw; g_ref_pitch = pitch;
            std::memcpy(g_ref_pos, pos, sizeof(g_ref_pos));
            VRLOG("[vr] reference recentered (orientation + position)");
        } else {
            VRLOG("[correct] angle reset; eye=(%.2f, %.2f, %.2f)", g_eye[0], g_eye[1], g_eye[2]);
        }
    }

    // FOV trim on top of the automatic match (PgUp wider / PgDn narrower).
    // PgDn trades field for sharpness and less edge pop-in.
    if (key_pressed(VK_PRIOR)) {
        g_fov_manual = (std::min)(g_fov_manual + 0.05f, 1.5f);
        VRLOG("[fov] trim -> %.2fx (auto %.2f/%.2f)", g_fov_manual, g_fov_widen_x, g_fov_widen_y);
    }
    if (key_pressed(VK_NEXT)) {
        g_fov_manual = (std::max)(g_fov_manual - 0.05f, 0.5f);
        VRLOG("[fov] trim -> %.2fx (auto %.2f/%.2f)", g_fov_manual, g_fov_widen_x, g_fov_widen_y);
    }

    if (key_pressed(VK_END)) {
        g_pos_enabled = !g_pos_enabled;
        VRLOG("[vr] 6DOF position %s", g_pos_enabled ? "ON" : "off (orientation only)");
    }

    // Viewmodel distance: '-' pulls the gun closer, '=' pushes it away.
    if (key_pressed(VK_OEM_MINUS)) {
        g_vm_push = (std::max)(g_vm_push - 0.05f, 0.0f);
        VRLOG("[viewmodel] push -> %.2f m%s", g_vm_push, g_vm_push == 0.0f ? " (off)" : "");
    }
    if (key_pressed(VK_OEM_PLUS)) {
        g_vm_push = (std::min)(g_vm_push + 0.05f, 1.0f);
        VRLOG("[viewmodel] push -> %.2f m", g_vm_push);
    }

    // Stereo calibration.
    if (key_pressed(VK_F1)) {
        g_eye_swap = -g_eye_swap;
        VRLOG("[stereo] eyes %s", g_eye_swap > 0 ? "normal" : "SWAPPED");
    }
    if (key_pressed(VK_F2)) {
        g_ipd_manual = true;
        g_ipd_world *= 0.8f;
        // The multiplicative step approaches zero but never reaches it; floor
        // to a true zero so "identical corrections in both eyes" is testable.
        if (g_ipd_world < 0.0005f) g_ipd_world = 0.0f;
        if (g_ipd_world == 0.0f) VRLOG("[stereo] IPD -> 0 (disabled, manual)");
        else                     VRLOG("[stereo] IPD -> %.4f world units (manual)", g_ipd_world);
    }
    if (key_pressed(VK_F3)) {
        // Shift+F3: back to automatic headset IPD. Plain F3: manual step up.
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_ipd_manual = false;
            VRLOG("[stereo] IPD back to automatic (headset)");
        } else {
            g_ipd_manual = true;
            g_ipd_world = (g_ipd_world == 0.0f) ? 0.008f : g_ipd_world * 1.25f;
            VRLOG("[stereo] IPD -> %.4f world units (manual)", g_ipd_world);
        }
    }
}

int  last_rendered_eye() { return (g_eye_swap > 0) ? g_last_eye : (g_last_eye ^ 1); }
bool stereo_active()     { return g_hmd_active && g_correct_on; }
unsigned modified_last_frame() { return g_modified_last_frame; }

bool game_proj_tangents(float& tan_half_h, float& tan_half_v)
{
    float th = 0.0f, tv = 0.0f;
    if (!base_tangents(th, tv)) return false;
    // Report the field ACTUALLY submitted, not the game's native projection,
    // so the compositor's bounds track the widened image.
    tan_half_h = th * g_fov_widen_x * g_fov_manual;
    tan_half_v = tv * g_fov_widen_y * g_fov_manual;
    return true;
}

} // namespace camover
