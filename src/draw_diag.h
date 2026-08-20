// Draw-call signature census. Bounded, off by default, one hotkey.
//
// Purpose: answer "which draws ARE the first-person weapon?" with evidence
// instead of a distance guess. Every draw that happens while a capture is
// armed is folded into a signature keyed by (shader, call-site RVA, z/alpha
// state, render target, projection class, bones), and per signature we keep
// counts plus the recovered projection and view-space distance band. INSERT
// arms a capture of a few frames; when it completes the table is written to
// bfbc2vr_draws_NN.txt next to the log, sorted by draw count, and a short
// summary goes to the log.
//
// The record is assembled by camera_override (it owns the last WVP seen for
// the active shader, the render-state shadow, and the frame's global
// projection); the wrapper supplies the call site and primitive count.
#pragma once

#include "draw_policy.h"

namespace drawdiag {

struct Record {
    const void*        shader       = nullptr;
    unsigned long long shader_hash  = 0;
    unsigned           shader_bytes = 0;
    bool               has_bones    = false;
    bool               had_wvp      = false;   // a transform span was written for this shader
    drawpolicy::ProjClass  proj     = drawpolicy::ProjClass::NoWvp;
    drawpolicy::ProjParams p;                  // recovered from the draw's own WVP
    float              view_origin[3] = { 0, 0, 0 };
    float              view_dist    = -1.0f;   // |view_origin|, -1 = unknown
    float              world_dist   = -1.0f;   // |world origin - eye|, -1 = unknown
    float              residue_w33  = 0.0f;    // (M * VP^-1)[3][3]  (1 if P_vm == P)
    float              residue_w23  = 0.0f;    // (M * VP^-1)[2][3]  (0 if P_vm == P)
    unsigned           z_enable = 0, z_write = 0, alpha_blend = 0;
    bool               rt_is_backbuffer = false;
    unsigned           rt_w = 0, rt_h = 0;
    bool               indexed      = false;
    unsigned           prims        = 0;
    const void*        ret          = nullptr; // game call site (absolute)
    bool               have_bone0   = false;
    float              bone0[12]    = {};      // first 3 registers of boneMatrices
    drawpolicy::DrawClass cls       = drawpolicy::DrawClass::Unclassified;
};

// Arm a capture of `frames` Presents. Safe to call while one is running (no-op).
void request_capture(unsigned frames);
bool capturing();

// Fold one draw into the census. Cheap no-op while not capturing.
void on_draw(const Record& r);

// Frame boundary: counts captured frames, dumps when the window closes.
void on_present();

} // namespace drawdiag
