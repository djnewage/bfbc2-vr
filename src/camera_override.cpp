#include "camera_override.h"
#include "logger.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace camover {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegPerSecond = 25.0f;   // slow enough to read as deliberate

bool  g_override_viewproj = false;
bool  g_override_camworld = false;
float g_angle_rad = 0.0f;

// Camera position, cached from c192 (the translation row of camera-to-world).
// Needed so the synthetic rotation happens about the player's own eye instead
// of about the world origin - the camera sits ~630 units out, so rotating about
// the origin would fling it across the map rather than look around.
float g_eye[3] = { 0.0f, 0.0f, 0.0f };
bool  g_have_eye = false;

// All matrices here are row-major with a row-vector convention (v' = v * M),
// which is what Phase 2 measured: c192 held the translation and its w was 1.
using Mat4 = float[16];

inline float& at(Mat4 m, int r, int c) { return m[r * 4 + c]; }
inline float  at(const Mat4 m, int r, int c) { return m[r * 4 + c]; }

void identity(Mat4 m)
{
    std::memset(m, 0, sizeof(float) * 16);
    at(m, 0, 0) = at(m, 1, 1) = at(m, 2, 2) = at(m, 3, 3) = 1.0f;
}

void multiply(const Mat4 a, const Mat4 b, Mat4 out)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += at(a, r, k) * at(b, k, c);
            at(out, r, c) = sum;
        }
    }
}

void rotation_y(float rad, Mat4 m)
{
    identity(m);
    const float s = std::sin(rad), c = std::cos(rad);
    at(m, 0, 0) =  c;  at(m, 0, 2) = -s;
    at(m, 2, 0) =  s;  at(m, 2, 2) =  c;
}

void translation(float x, float y, float z, Mat4 m)
{
    identity(m);
    at(m, 3, 0) = x; at(m, 3, 1) = y; at(m, 3, 2) = z;
}

// Rotation about the camera position rather than the origin:
//   T(-eye) * RotY(angle) * T(+eye)
void rotation_about_eye(float rad, Mat4 out)
{
    Mat4 to_origin, rot, back, tmp;
    translation(-g_eye[0], -g_eye[1], -g_eye[2], to_origin);
    rotation_y(rad, rot);
    translation(g_eye[0], g_eye[1], g_eye[2], back);
    multiply(to_origin, rot, tmp);
    multiply(tmp, back, out);
}

bool key_pressed(int vk)
{
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return fired;
}

// True if [start, start+count) fully contains the 4 registers at `base`.
bool covers(unsigned start, unsigned count, unsigned base)
{
    return start <= base && (start + count) >= (base + 4);
}

} // namespace

bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch)
{
    if (!data || vec4_count == 0) return false;

    // Always keep the cached eye position current, override or not.
    if (covers(start_register, vec4_count, kCamWorldBase)) {
        const float* row3 = data + (kCamWorldBase + 3 - start_register) * 4;
        g_eye[0] = row3[0]; g_eye[1] = row3[1]; g_eye[2] = row3[2];
        g_have_eye = true;
    }

    if (!g_override_viewproj && !g_override_camworld) return false;
    if (!g_have_eye) return false;

    const bool hit_vp = g_override_viewproj && covers(start_register, vec4_count, kViewProjBase);
    const bool hit_cw = g_override_camworld && covers(start_register, vec4_count, kCamWorldBase);
    if (!hit_vp && !hit_cw) return false;

    scratch.assign(data, data + vec4_count * 4);

    Mat4 rot;
    rotation_about_eye(g_angle_rad, rot);

    if (hit_vp) {
        // v_clip = v_world * VP. Inserting the rotation in front rotates the
        // world about the eye, which reads as the camera looking around.
        float* vp = scratch.data() + (kViewProjBase - start_register) * 4;
        Mat4 out;
        multiply(rot, reinterpret_cast<const float(&)[16]>(*vp), out);
        std::memcpy(vp, out, sizeof(out));
    }

    if (hit_cw) {
        // Camera-to-world: rotate the orientation in place, leave position.
        float* cw = scratch.data() + (kCamWorldBase - start_register) * 4;
        Mat4 out;
        multiply(reinterpret_cast<const float(&)[16]>(*cw), rot, out);
        // Restore the original translation row - rotation_about_eye would
        // otherwise nudge it by floating point noise.
        for (int i = 0; i < 3; ++i) at(out, 3, i) = cw[12 + i];
        std::memcpy(cw, out, sizeof(out));
    }

    return true;
}

void on_present()
{
    static ULONGLONG last_tick = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last_tick ? (now - last_tick) / 1000.0f : 0.0f;
    last_tick = now;

    if (g_override_viewproj || g_override_camworld) {
        g_angle_rad += dt * kDegPerSecond * kPi / 180.0f;
        if (g_angle_rad > 2.0f * kPi) g_angle_rad -= 2.0f * kPi;
    }

    if (key_pressed(VK_F7)) {
        g_override_viewproj = !g_override_viewproj;
        VRLOG("[override] view-projection (c%u-c%u) %s", kViewProjBase, kViewProjBase + 3,
              g_override_viewproj ? "ON" : "off");
    }
    if (key_pressed(VK_F8)) {
        g_override_camworld = !g_override_camworld;
        VRLOG("[override] camera-to-world (c%u-c%u) %s", kCamWorldBase, kCamWorldBase + 3,
              g_override_camworld ? "ON" : "off");
    }
    if (key_pressed(VK_F6)) {
        g_angle_rad = 0.0f;
        VRLOG("[override] angle reset; eye=(%.2f, %.2f, %.2f) cached=%d",
              g_eye[0], g_eye[1], g_eye[2], static_cast<int>(g_have_eye));
    }
}

bool any_override_active() { return g_override_viewproj || g_override_camworld; }

} // namespace camover
