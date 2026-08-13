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
