// Phase 3b: real head tracking via OpenVR.
//
// The synthetic yaw proved the correction pipeline end to end. This swaps the
// fake rotation for the HMD's actual orientation, using OpenVR in Background
// application mode - which hands us poses WITHOUT owning rendering or the
// compositor. The picture stays on the monitor; the headset just steers it.
// That is deliberately the smallest possible step toward VR: it exercises
// runtime init, pose acquisition, and coordinate conversion inside a 32-bit
// process, while stereo submission stays out of the loop until Phase 4.
//
// Why OpenVR and not OpenXR for this step: OpenXR sessions require a graphics
// binding to deliver poses (SteamVR has no headless extension), which drags
// the whole Vulkan interop story into what should be a tracking smoke test.
// OpenVR's Background mode needs nothing but the DLL. openRBRVR ships both
// APIs for exactly this reason. OpenXR arrives with stereo in Phase 4.
#pragma once

namespace vr { struct HmdMatrix34_t; struct TrackedDevicePose_t; class IVRSystem; }

namespace vrtrack {

// Pose handoff from the compositor loop. WaitGetPoses returns the pose the
// compositor PREDICTS for the frame about to be rendered - rendering with any
// other pose (e.g. a freely sampled "now" pose) makes the compositor's
// reprojection misalign the image by the difference, which in alternate-eye
// stereo doubles the world during head motion. When a render pose has been
// provided since the last update(), it wins over the sampled pose.
void set_render_pose(const vr::HmdMatrix34_t& pose, bool valid);

// The whole pose array from the compositor's WaitGetPoses, so hands and head
// come from one instant. Call alongside set_render_pose.
void set_render_poses(const vr::TrackedDevicePose_t* poses, unsigned count);

// Attempt OpenVR init in Background mode. Safe to call every frame; retries
// on a slow cadence until SteamVR is up, then sticks. Returns current state.
bool ensure_init();

// Refresh poses. Call once per presented frame.
void update();

// True when a valid HMD pose was seen this frame.
bool have_pose();

// Motion controllers. The pose is the device-to-absolute 3x4 (OpenVR
// convention: column-vector, +X right / +Y up / -Z forward, metres) flattened
// row-major into 12 floats - the same block OpenVR hands us, unconverted.
// `hand`: 0 = left, 1 = right. False when that controller has no valid,
// actively tracked pose this frame (a valid-but-untracked pose is refused:
// OpenVR keeps reporting an inferred last-known pose, which must never be
// mistaken for real tracking - BFVR's rule).
bool controller_pose(int hand, float out3x4[12]);
bool controller_connected(int hand);

// Bumped only when a controller's pose actually changes. The game presents
// faster than the runtime produces poses (alternate-eye rendering), so any
// per-sample logic - the aim loop above all - must dedup on this rather than
// acting once per rendered frame.
unsigned controller_sequence(int hand);

// Buttons and analog axes, from OpenVR's legacy controller state. `buttons` is
// the raw ulButtonPressed mask (see vr::EVRButtonId); stick is axis 0 and
// trigger axis 1 on both Index and Touch-style controllers.
struct ControllerInput {
    bool     valid = false;
    unsigned long long buttons = 0;
    float    stick[2] = { 0.0f, 0.0f };
    float    trigger = 0.0f;
    float    grip = 0.0f;
};
bool controller_input(int hand, ControllerInput& out);

// The HMD orientation as a row-major 3x3 in OpenVR's right-handed, Y-up,
// -Z-forward convention, relative to the seated/standing origin.
void orientation(float out3x3[9]);

// HMD position in meters, same convention (+X right, +Y up, -Z forward),
// relative to the standing-universe origin. Drives 6DOF.
void position(float out3[3]);

void shutdown();

// The live IVRSystem, or null before init. Needed by the compositor path for
// per-eye projection queries (GetProjectionRaw).
vr::IVRSystem* system();

// The user's real interpupillary distance in meters, from the headset's own
// setting (the Index's physical slider). 0 until known. Frostbite is a metric
// engine, so this is directly the correct world-space eye separation - the
// keystone of zero-calibration stereo.
float user_ipd_meters();

} // namespace vrtrack
