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

namespace vrtrack {

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

void shutdown();

} // namespace vrtrack
