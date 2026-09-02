// Render-pass recorder. Off by default; `passes [frames]` arms it.
//
// The mod's model of a frame has been three bits - is the render target the
// backbuffer, and its width and height, on slot 0 only. That is why "which
// pass is the player's" was a heuristic that got the camera wrong: shadow
// cascades, reflections and the main scene all write c185..c192, and nothing
// recorded which surfaces each of them drew into, in what order, with what.
//
// A "pass" here is a contiguous run of draws with the same render-target set.
// Per pass we keep: every bound colour target and the depth-stencil, with
// identity, size, format and multisample; the Clear calls; draw and primitive
// counts; the vertex shaders used; and every camera block (c185 projection +
// c189 heading) written during it, plus whether that projection is the one the
// end-of-frame election adopted. The dump is one line per pass, in order, so
// a frame can be read top to bottom.
//
// This is a recorder, not a corrector: nothing here feeds back into rendering.
#pragma once

#include "draw_policy.h"

#include <d3d9.h>
#include <cstdio>

namespace rpass {

// Wrapper hooks. All are one-branch no-ops while not armed.
void on_set_render_target(DWORD slot, IDirect3DSurface9* surface, bool is_backbuffer);
void on_set_depth_stencil(IDirect3DSurface9* surface);
void on_clear(DWORD flags, D3DCOLOR color, float z, DWORD stencil);
void on_begin_scene();
void on_end_scene();

// From camera_override: a draw landed (hash may be 0), a camera block was
// written, and which projection the election chose for the frame.
void on_draw(unsigned long long vs_hash, unsigned prims);
void on_camera_write(const drawpolicy::ProjParams& proj);
void on_camera_heading(const float rows[9]);
void on_camera_elected(const drawpolicy::ProjParams& proj);

// Frame boundary; dumps when the armed window closes.
void on_present();

void request_capture(unsigned frames);
bool capturing();

bool command(const char* cmd, const char* args, char* reply, size_t n);
void status(FILE* f);

} // namespace rpass
