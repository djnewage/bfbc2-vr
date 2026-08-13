// Per-shader constant discovery via the D3D9 CTAB.
//
// Finding (2026-08-13, see camera_override.h): BFBC2 has no fixed camera
// register for opaque geometry. Write shapes vary per shader - (c0,6), (c0,13),
// (c0,180) - because each compiled shader has its own constant layout. Probing
// a fixed register corrupted bone palettes and packed params on most draws
// (vertex explosion in the recording) while occasionally hitting a real matrix
// (the rolled-but-coherent world in the final frames).
//
// So we stop guessing and read the layout the game itself declares: D3D9
// vertex shader bytecode embeds a constant table (fourcc 'CTAB' inside a
// comment token) mapping each named constant to its register range. Parsing it
// at CreateVertexShader time gives us, per shader, where the transform lives -
// by the engine's own name for it.
//
// This pass is DISCOVERY ONLY: parse, aggregate, log every distinct constant
// name seen. Which names mark the WVP is a decision to make while reading the
// evidence, not while writing the parser.
#pragma once

#include <d3d9.h>

namespace shaderreg {

// Parse the CTAB from bytecode and register the shader. Called from
// CreateVertexShader with the game-supplied function blob.
void on_create_vertex_shader(const DWORD* bytecode, IDirect3DVertexShader9* shader);

// Track the active shader so constant writes can later be attributed.
void on_set_vertex_shader(IDirect3DVertexShader9* shader);

// Called per Present: occasionally flushes newly seen constant names to the log.
void on_present();

// Transform spans declared by the ACTIVE shader's own constant table.
// CTAB evidence (65 shaders, 0 stripped): the names below mark the matrices
// that end in clip space, which is exactly what the clip-space correction
// applies to. worldMatrix/viewMatrix are deliberately absent - correcting a
// world matrix with a clip-space correction would be wrong.
//
//   worldViewProj / worldViewProjMatrix / viewProjMatrix
//
// Returns the number of spans written to out[] (0 if none / no active shader).
struct Span { unsigned start; unsigned count; };
constexpr size_t kMaxSpans = 4;
size_t active_transform_spans(Span out[kMaxSpans]);

} // namespace shaderreg
