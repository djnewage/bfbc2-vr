// Shadow copy of the D3D9 vertex shader float constant file.
//
// This is the matrix-discovery tool. Logging every SetVertexShaderConstantF
// call would emit thousands of lines per frame and bury the signal, so instead
// we mirror the whole constant file and dump it on demand.
//
// Workflow for finding the camera:
//   1. Stand still in game, press F10  -> marks a baseline
//   2. Rotate the camera ~90 degrees
//   3. Press F11                       -> dumps ONLY the registers that changed
//
// Registers that change when the camera rotates and nothing else moves are the
// view transform. Repeat with an ADS toggle instead of a rotation to isolate
// projection, since that changes FOV rather than orientation.
//
// STABILITY TRACKING
//
// Measured on BFBC2: ~5900 vec4 writes per frame across only c0..c18. The game
// recycles the same small register block for every draw call, so a plain
// snapshot captures whatever the last draw happened to leave behind - usually a
// per-object matrix, not the camera.
//
// The separator is how a register behaves WITHIN one frame:
//   STABLE  - written many times but always the same value for the whole frame.
//             Camera-ish: view, projection, view-projection, fog, time.
//   VARYING - takes different values during a single frame.
//             Per-object: world matrices, bone palettes, per-material data.
//
// The camera lives in a register that is STABLE within a frame but CHANGES
// between frames when you move. That pair of conditions is the actual signal.
#pragma once

#include <cstddef>

namespace vrconst {

// vs_3_0 exposes 256 float constant registers. Anything above this is clamped
// and reported, rather than silently dropped.
constexpr size_t kMaxRegisters = 256;

void record(unsigned start_register, const float* data, unsigned vec4_count);

void mark_baseline();
void dump_snapshot();          // full constant file, with matrix heuristics
void dump_diff_vs_baseline();  // only what changed - the discovery signal

// Called once per Present. Polls hotkeys and emits periodic frame stats.
void on_present();

} // namespace vrconst
