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

} // namespace

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

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    // Predict slightly ahead; for a monitor-steering test the exact photon
    // time is not critical.
    g_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.02f,
                                              poses, vr::k_unMaxTrackedDeviceCount);

    const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid) return;

    const auto& m = hmd.mDeviceToAbsoluteTracking.m;
    // Upper-left 3x3 of the 3x4 device-to-absolute transform, stored row-major.
    g_rot[0] = m[0][0]; g_rot[1] = m[0][1]; g_rot[2] = m[0][2];
    g_rot[3] = m[1][0]; g_rot[4] = m[1][1]; g_rot[5] = m[1][2];
    g_rot[6] = m[2][0]; g_rot[7] = m[2][1]; g_rot[8] = m[2][2];
    g_have_pose = true;
}

bool have_pose() { return g_have_pose; }

void orientation(float out3x3[9])
{
    std::memcpy(out3x3, g_rot, sizeof(g_rot));
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
