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
// TARGETING (2026-08-13, post-CTAB): fixed-register probing is gone. The
// correction now applies only to shaders whose own constant table declares a
// clip-space transform (worldViewProj / worldViewProjMatrix / viewProjMatrix),
// at exactly the registers the table names. Bone palettes and packed params
// never get touched, because those shaders do not declare these names.
//
// CONVENTION: the rolled-but-coherent world in the recording is the signature
// of a transposed-matrix mismatch. HLSL packs matrices column-major into
// registers by default (stored block = M^T), in which case the correction must
// be applied as S' = C^T * S rather than S' = S * C. Both modes are one key
// apart; the one that yaws the world cleanly about a vertical axis is correct.
//
//   F7  toggle the correction on/off
//   F8  toggle convention: transposed <-> row-registers
//   F5  reset the synthetic angle
#pragma once

#include <d3d9.h>
#include <cstdio>
#include <cstddef>

#include <vector>

namespace camover {

// Global camera blocks from Phase 2 - still tracked as the SOURCE for building
// the correction (VP and eye position), no longer used as override targets.
constexpr unsigned kViewProjBase = 185;   // c185..c188, view-projection
constexpr unsigned kCamWorldBase = 189;   // c189..c192, camera-to-world

// If this write covers the probed block, fills `scratch` with a corrected copy
// and returns true; the caller forwards `scratch` instead of the game's buffer.
// Draw census + per-draw classification plumbing (2026-08-20). The wrapper
// reports every draw with the game's call site; the render-state and
// render-target shadows let the census key on z/alpha state and on whether
// the draw lands in the backbuffer. All cheap no-ops unless a census is armed.
void on_draw(const void* return_address, bool indexed, unsigned prim_count);

// Command channel (console.h): widen/auto/mode/push/ownproj/bones/recenter/fov.
// Returns true if handled; reply goes to `reply`.
bool command(const char* cmd, const char* args, char* reply, size_t reply_size);
// Append live state to a status file.
void status(FILE* f);
// The world projection's half-angle tangents recovered from VP this frame.
bool world_tangents(float& tan_half_h, float& tan_half_v);
void note_render_state(unsigned state, unsigned value);
void note_render_target(IDirect3DSurface9* surface);

bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch);

// Once per presented frame: advance the angle, rebuild the correction, poll keys.
void on_present();

// Alternate-eye rendering. Each frame is rendered from one eye's viewpoint
// (correction includes a +/- half-IPD lateral offset along the camera's right
// axis); the eye alternates at every Present. The compositor submits the
// fresh texture for this frame's eye and last frame's texture for the other.
//   0 = left, 1 = right
int  last_rendered_eye();
bool stereo_active();

// Corrected transform writes during the last completed frame. Zero means no
// 3D world was drawn - a menu or loading screen - where stereo is meaningless
// and alternate-eye staleness is glaring (panning 2D art + load hitches put
// SECONDS of difference between the eyes). The compositor drops to mono
// submission whenever this is zero.
unsigned modified_last_frame();

// Half-angle tangents of the game's live projection, recovered from the VP
// matrix (rows 0/1 of VP are rows of a rotation scaled by the projection's
// x/y factors, so tan = 1/|row|). Returns false until a VP has been seen.
// The compositor needs these to tell SteamVR which slice of each eye's
// frustum the game image actually covers.
bool game_proj_tangents(float& tan_half_h, float& tan_half_v);

} // namespace camover
