// Phase 3a: write-side control over the camera.
//
// Before involving OpenXR at all, we need to prove one thing: that writing our
// own values into the camera registers actually moves the rendered view. If it
// does, every later phase is a matter of choosing what to write. If it does not,
// the registers we found are read by something other than the main scene
// transform and we need to know that now, not after building a VR runtime layer.
//
// The test is a synthetic yaw that advances on its own, independent of the
// mouse. Two separate toggles, because Phase 2 found two candidate blocks and
// only measurement can say which one drives the picture:
//
//   F7  toggle override of c185-c188  (view-projection)
//   F8  toggle override of c189-c192  (camera-to-world)
//   F6  reset the synthetic angle to zero
//
// Turn one on. If the world starts swinging around you while your mouse sits
// still, that block drives rendering and Phase 3 is proven.
#pragma once

#include <vector>

namespace camover {

// Register blocks located in Phase 2. See docs/phase2-results.md.
constexpr unsigned kViewProjBase  = 185;   // c185..c188, view-projection
constexpr unsigned kCamWorldBase  = 189;   // c189..c192, camera-to-world

// If this write touches an overridden block, fills `scratch` with a modified
// copy and returns true; the caller must then forward `scratch` instead of the
// game's buffer. Returns false when nothing needs changing, which is the
// overwhelmingly common case and stays allocation-free.
bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch);

// Called once per presented frame: advances the synthetic angle and polls keys.
void on_present();

bool any_override_active();

} // namespace camover
