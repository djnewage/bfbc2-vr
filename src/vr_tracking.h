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

namespace vr { struct HmdMatrix34_t; class IVRSystem; }

namespace vrtrack {

// Pose handoff from the compositor loop. WaitGetPoses returns the pose the
// compositor PREDICTS for the frame about to be rendered - rendering with any
// other pose (e.g. a freely sampled "now" pose) makes the compositor's
// reprojection misalign the image by the difference, which in alternate-eye
// stereo doubles the world during head motion. When a render pose has been
// provided since the last update(), it wins over the sampled pose.
void set_render_pose(const vr::HmdMatrix34_t& pose, bool valid);

// Attempt OpenVR init in Background mode. Safe to call every frame; retries
// on a slow cadence until SteamVR is up, then sticks. Returns current state.
bool ensure_init();

// Refresh poses. Call once per presented frame.
void update();

// True when a valid HMD pose was seen this frame.
bool have_pose();

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
