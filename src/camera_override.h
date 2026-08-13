// Phase 3a: write-side control over the camera.
//
// FINDING (2026-08-13): overriding c185-c188 made fog and tree limbs SHIFT
// slightly - not vanish, not spin - and c189-c192 did nothing visible. So
// those blocks are genuine camera data, but only a subset of shaders reads
// them (billboarded vegetation, fog), and the main opaque geometry does not.
// A small shift rather than a full sweep says the value is consumed for a
// secondary purpose, not as the primary transform. BFBC2 pre-multiplies
// world-view-projection per object on the CPU - ~456 draws per frame each
// rewriting c6..c18.
//
// THE CORRECTION TRICK
//
// A per-object matrix maps object space straight to clip space:
//     v_clip = v_object * WVP,   where WVP = World * View * Proj
//
// We cannot edit View directly because it is already baked in. But we can
// right-multiply by a correction that undoes the old camera and applies a new
// one. Wanting  WVP' = World * (V'P')  with  V'P' = R * VP :
//
//     World       = WVP * VP^-1
//     WVP'        = WVP * VP^-1 * R * VP
//     CORRECTION  = VP^-1 * R * VP          <- computed ONCE per frame
//     WVP'        = WVP * CORRECTION        <- one multiply per draw
//
// VP comes free from c185-c188, which Phase 2 already identified. And the same
// correction is valid for VP itself, since VP * (VP^-1 * R * VP) = R * VP - so
// one operation handles both per-object and global matrices.
//
// This is also exactly the mechanism Phases 4 and 5 need: substituting a
// per-eye projection or an HMD pose is just a different R.
//
// PROBE CONTROLS - sweep the register file to find what drives the picture:
//   F7  toggle the probe on/off
//   F8  probe base + 1        (watch the log for the current base)
//   F6  probe base - 1
//   F5  reset the synthetic angle
#pragma once

#include <vector>

namespace camover {

// Register blocks located in Phase 2. See docs/phase2-results.md.
constexpr unsigned kViewProjBase = 185;   // c185..c188, view-projection
constexpr unsigned kCamWorldBase = 189;   // c189..c192, camera-to-world

// Default probe start: the per-draw block, the prime suspect for the real
// world-view-projection.
constexpr unsigned kProbeDefault = 6;

// If this write covers the probed block, fills `scratch` with a corrected copy
// and returns true; the caller forwards `scratch` instead of the game's buffer.
bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch);

// Once per presented frame: advance the angle, rebuild the correction, poll keys.
void on_present();

} // namespace camover
