#include "vr_tracking.h"
#include "logger.h"

#include <windows.h>
#include <openvr.h>
#include <cstring>

namespace vrtrack {
namespace {

vr::IVRSystem* g_system = nullptr;
bool  g_tried_recently = false;
ULONGLONG g_last_try = 0;
bool  g_have_pose = false;
float g_rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
float g_pos[3] = { 0,0,0 };

// Render pose delivered by the compositor loop (see header).
bool  g_have_render_pose = false;
float g_render_rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
float g_render_pos[3] = { 0,0,0 };

// Controller poses, from the same WaitGetPoses the eyes are rendered with, so
// hand and head belong to one instant.
bool  g_ctl_valid[2] = { false, false };
bool  g_ctl_seen[2]  = { false, false };
float g_ctl[2][12]   = {};
unsigned g_ctl_seq[2] = { 0, 0 };
vr::TrackedDeviceIndex_t g_ctl_index[2] = { vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid };

void store_controller(int hand, const vr::TrackedDevicePose_t& p)
{
    // VALID alone is not enough: OpenVR reports an inferred last-known pose
    // as valid after tracking is lost, and a weapon must never be driven by
    // a guess. Require actively tracked.
    const bool ok = p.bPoseIsValid && p.bDeviceIsConnected &&
                    p.eTrackingResult == vr::TrackingResult_Running_OK;
    g_ctl_seen[hand] = p.bDeviceIsConnected;
    if (!ok) { g_ctl_valid[hand] = false; return; }
    const auto& m = p.mDeviceToAbsoluteTracking.m;
    bool changed = !g_ctl_valid[hand];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float v = m[r][c];
            if (v != g_ctl[hand][r * 4 + c]) changed = true;
            g_ctl[hand][r * 4 + c] = v;
        }
    }
    if (changed) ++g_ctl_seq[hand];
    g_ctl_valid[hand] = true;
}

// Which axis carries the thumbstick and which bit is its click, per device.
// Index/Knuckles put the joystick on axis 3 / button 35 and the TRACKPAD on
// axis 0 / button 32; everything else uses axis 0 / button 32 for the stick.
// Getting this wrong is what made a resting thumb read as a deliberate click.
char g_ctl_type[2][64] = {};
int  g_stick_axis[2] = { 0, 0 };
unsigned long long g_stick_click[2] = { 1ull << 32, 1ull << 32 };

void identify_controller(int hand, vr::TrackedDeviceIndex_t idx)
{
    char type[64] = {};
    g_system->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String,
                                             type, sizeof(type), nullptr);
    if (!type[0] || std::strcmp(type, g_ctl_type[hand]) == 0) return;   // unchanged

    std::strncpy(g_ctl_type[hand], type, sizeof(g_ctl_type[hand]) - 1);
    const bool index = std::strstr(type, "knuckles") != nullptr ||
                       std::strstr(type, "index") != nullptr;
    g_stick_axis[hand]  = index ? 3 : 0;
    g_stick_click[hand] = index ? (1ull << 35) : (1ull << 32);
    VRLOG("[input] %s controller is '%s' - stick on axis %d, click bit %d",
          hand ? "right" : "left", type, g_stick_axis[hand], index ? 35 : 32);
}

void refresh_controllers(const vr::TrackedDevicePose_t* poses, unsigned count)
{
    if (!g_system || !poses) return;
    for (int hand = 0; hand < 2; ++hand) {
        const auto role = (hand == 0) ? vr::TrackedControllerRole_LeftHand
                                      : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t idx = g_system->GetTrackedDeviceIndexForControllerRole(role);
        g_ctl_index[hand] = idx;
        if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= count) {
            g_ctl_valid[hand] = false; g_ctl_seen[hand] = false;
            continue;
        }
        identify_controller(hand, idx);
        store_controller(hand, poses[idx]);
    }
}

} // namespace

void set_render_poses(const vr::TrackedDevicePose_t* poses, unsigned count)
{
    refresh_controllers(poses, count);
}

void set_render_pose(const vr::HmdMatrix34_t& pose, bool valid)
{
    if (!valid) return;
    const auto& m = pose.m;
    g_render_rot[0] = m[0][0]; g_render_rot[1] = m[0][1]; g_render_rot[2] = m[0][2];
    g_render_rot[3] = m[1][0]; g_render_rot[4] = m[1][1]; g_render_rot[5] = m[1][2];
    g_render_rot[6] = m[2][0]; g_render_rot[7] = m[2][1]; g_render_rot[8] = m[2][2];
    // Column 3 of the 3x4 device-to-absolute transform is the position.
    g_render_pos[0] = m[0][3]; g_render_pos[1] = m[1][3]; g_render_pos[2] = m[2][3];
    g_have_render_pose = true;
}

bool ensure_init()
{
    if (g_system) return true;

    // Retry every 5 seconds, not every frame - VR_Init is not cheap and
    // SteamVR may simply not be running yet.
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_try < 5000) return false;
    g_last_try = now;

    if (!vr::VR_IsRuntimeInstalled()) {
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[vr] no OpenVR runtime installed"); }
        return false;
    }
    if (!vr::VR_IsHmdPresent()) {
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[vr] runtime present, no HMD detected yet (will keep retrying)"); }
        return false;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    // Scene mode: Phase 4 submits frames, and the compositor only accepts
    // Submit from a scene app. Falls back to Background (tracking-only, the
    // Phase 3b behavior) if Scene init is refused.
    g_system = vr::VR_Init(&err, vr::VRApplication_Scene);
    if (err != vr::VRInitError_None) {
        VRLOG("[vr] Scene init failed (%s) - falling back to Background",
              vr::VR_GetVRInitErrorAsEnglishDescription(err));
        err = vr::VRInitError_None;
        g_system = vr::VR_Init(&err, vr::VRApplication_Background);
    }
    if (err != vr::VRInitError_None) {
        g_system = nullptr;
        static vr::EVRInitError last_logged = vr::VRInitError_None;
        if (err != last_logged) {
            last_logged = err;
            VRLOG("[vr] VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        }
        return false;
    }

    char model[128] = {}, serial[128] = {};
    g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_ModelNumber_String, model, sizeof(model));
    g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_SerialNumber_String, serial, sizeof(serial));
    VRLOG("[vr] OpenVR initialized (Background). HMD: %s (%s)", model, serial);
    return true;
}

void update()
{
    g_have_pose = false;
    if (!g_system && !ensure_init()) return;

    // Compositor-predicted pose wins: it is what the compositor will assume
    // this frame was rendered with. One pose per frame - the flag resets so a
    // stalled compositor loop falls back to sampling instead of freezing.
    if (g_have_render_pose) {
        std::memcpy(g_rot, g_render_rot, sizeof(g_rot));
        std::memcpy(g_pos, g_render_pos, sizeof(g_pos));
        g_have_render_pose = false;
        g_have_pose = true;
        return;
    }

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    // Fallback: freely sampled pose (tracking-only mode, no compositor).
    g_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.02f,
                                              poses, vr::k_unMaxTrackedDeviceCount);

    refresh_controllers(poses, vr::k_unMaxTrackedDeviceCount);

    const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid) return;

    const auto& m = hmd.mDeviceToAbsoluteTracking.m;
    // Upper-left 3x3 of the 3x4 device-to-absolute transform, stored row-major.
    g_rot[0] = m[0][0]; g_rot[1] = m[0][1]; g_rot[2] = m[0][2];
    g_rot[3] = m[1][0]; g_rot[4] = m[1][1]; g_rot[5] = m[1][2];
    g_rot[6] = m[2][0]; g_rot[7] = m[2][1]; g_rot[8] = m[2][2];
    g_pos[0] = m[0][3]; g_pos[1] = m[1][3]; g_pos[2] = m[2][3];
    g_have_pose = true;
}

bool have_pose() { return g_have_pose; }

bool controller_pose(int hand, float out3x4[12])
{
    if (hand < 0 || hand > 1 || !g_ctl_valid[hand]) return false;
    std::memcpy(out3x4, g_ctl[hand], sizeof(g_ctl[hand]));
    return true;
}

bool controller_connected(int hand)
{
    return (hand >= 0 && hand <= 1) && g_ctl_seen[hand];
}

unsigned controller_sequence(int hand)
{
    return (hand >= 0 && hand <= 1) ? g_ctl_seq[hand] : 0u;
}

const char* controller_type(int hand)
{
    return (hand >= 0 && hand <= 1) ? g_ctl_type[hand] : "";
}

bool controller_input(int hand, ControllerInput& out)
{
    // Deliberately does NOT clear `out` up front. It used to, which meant a
    // refused read wrote stick[0] = 0 over the caller's good sample - and a
    // zero stick reads as "returned to centre", re-arming the snap-turn latch
    // while the stick was still held. Tracking flickers exactly when the
    // player raises the controller, so that produced a burst of snap turns.
    // Only `valid` changes on failure; the caller keeps the last good values.
    out.valid = false;
    if (!g_system || hand < 0 || hand > 1) return false;
    const vr::TrackedDeviceIndex_t idx = g_ctl_index[hand];
    if (idx == vr::k_unTrackedDeviceIndexInvalid) return false;

    vr::VRControllerState_t st = {};
    if (!g_system->GetControllerState(idx, &st, sizeof(st))) {
        // Legacy input is off (SteamVR can disable it when an application
        // declares an action manifest). Say so once - it is the difference
        // between "no buttons wired" and "buttons wired but ignored".
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("[input] GetControllerState refused for %s hand - SteamVR legacy input may be disabled",
                  hand ? "right" : "left");
        }
        return false;
    }
    out.valid = true;
    out.buttons = st.ulButtonPressed;
    for (int a = 0; a < 5; ++a) { out.axis[a][0] = st.rAxis[a].x; out.axis[a][1] = st.rAxis[a].y; }

    const int ax = g_stick_axis[hand];
    out.stick[0] = st.rAxis[ax].x;
    out.stick[1] = st.rAxis[ax].y;
    out.stick_axis = ax;
    out.stick_click_mask = g_stick_click[hand];
    out.trigger = st.rAxis[1].x;
    out.grip = st.rAxis[2].x;
    return true;
}

void orientation(float out3x3[9])
{
    std::memcpy(out3x3, g_rot, sizeof(g_rot));
}

void position(float out3[3])
{
    std::memcpy(out3, g_pos, sizeof(g_pos));
}

vr::IVRSystem* system() { return g_system; }

float user_ipd_meters()
{
    if (!g_system) return 0.0f;

    // Re-query on a slow cadence: the Index has a physical IPD slider the
    // user can move mid-session, and SteamVR updates the property live.
    static float cached = 0.0f;
    static ULONGLONG last_query = 0;
    const ULONGLONG now = GetTickCount64();
    if (cached == 0.0f || now - last_query > 3000) {
        last_query = now;
        vr::ETrackedPropertyError err = vr::TrackedProp_Success;
        const float v = g_system->GetFloatTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &err);
        if (err == vr::TrackedProp_Success && v > 0.03f && v < 0.09f) cached = v;
    }
    return cached;
}

void shutdown()
{
    if (g_system) {
        vr::VR_Shutdown();
        g_system = nullptr;
        VRLOG("[vr] OpenVR shut down");
    }
}

} // namespace vrtrack
