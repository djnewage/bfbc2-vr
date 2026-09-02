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
#include <cstdio>

namespace shaderreg {

// Parse the CTAB from bytecode and register the shader. Called from
// CreateVertexShader with the game-supplied function blob.
void on_create_vertex_shader(const DWORD* bytecode, IDirect3DVertexShader9* shader);

// Track the active shader so constant writes can later be attributed.
void on_set_vertex_shader(IDirect3DVertexShader9* shader);

// Pixel shaders carry the same CTAB. They are not part of any correction -
// the mod never touches a pixel constant - but their tables name the texture
// slots and per-pixel parameters, which the engine map needs.
void on_create_pixel_shader(const DWORD* bytecode, IDirect3DPixelShader9* shader);
void on_set_pixel_shader(IDirect3DPixelShader9* shader);

// Write every parsed table - vertex and pixel, every register set, with type
// info and bind counts - to a stamped dump file, plus a register -> names index
// for the vertex float file. Console verb `shaders`. Returns false if the file
// could not be opened; path_out receives the path either way.
bool dump(char* path_out, size_t path_size);
bool command(const char* cmd, const char* args, char* reply, size_t n);
void status(FILE* f);

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

// The active shader, for keying per-shader caches (e.g. fingerprinted matrix
// offsets in shaders that declare only an anonymous "constants" blob).
IDirect3DVertexShader9* active_shader();

// True if the active shader declares float constants but none of the named
// transform spans - the "constants"-blob shaders that need fingerprinting.
bool active_shader_uncharted();

// Identity and facts about a shader that do not change after creation.
// Shader POINTERS are recycled across level loads; the bytecode hash is the
// stable identity (BFVR keys its draw-signature tables the same way).
struct ShaderFacts {
    unsigned long long hash        = 0;     // FNV-1a over the token stream
    unsigned           bytes       = 0;     // token stream length in bytes
    unsigned           ordinal     = 0;     // creation order, 1-based
    bool               has_ctab    = false;
    bool               has_bones   = false; // declares boneMatrices / boneVectors
    unsigned           bone_start  = 0;     // first register of boneMatrices (if any)
    unsigned           bone_count  = 0;
    unsigned           transform_span_count = 0;
};

// Facts for the ACTIVE shader, or null. Lock-free on the hot path: the pointer
// is resolved once per SetVertexShader and cached thread-locally.
const ShaderFacts* active_facts();

} // namespace shaderreg
