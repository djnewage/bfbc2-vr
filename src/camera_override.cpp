#include "camera_override.h"
#include "shader_registry.h"
#include "vr_tracking.h"
#include "vr_compositor.h"
#include "draw_policy.h"
#include "draw_diag.h"
#include "render_passes.h"
#include "memscan.h"
#include "vrinput.h"
#include "aim_policy.h"
#include "mat4.h"
#include "logger.h"

#include <openvr.h>   // GetProjectionRaw, for auto FOV widening

#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace camover {
namespace {

using namespace m4;

constexpr float kPi = 3.14159265358979f;
constexpr float kDegPerSecond = 25.0f;   // slow enough to read as deliberate

bool  g_correct_on = false;
bool  g_transposed = true;    // HLSL default packing is column-major (stored = M^T)
float g_angle_rad  = 0.0f;

// Head tracking. When an HMD pose is available the correction R comes from the
// head's yaw/pitch delta against a reference captured at enable time; the
// synthetic spin only runs when there is no pose, so the old test still works
// with SteamVR closed. Axis signs are field-calibrated (F8/F6) because the
// mapping between OpenVR axes and this engine's axes is empirical.
bool  g_hmd_active   = false;   // pose seen and reference captured
float g_ref_yaw      = 0.0f;
float g_ref_pitch    = 0.0f;
// Field-calibrated 2026-08-13: default signs were inverted on both axes
// (user confirmed one F8 and one F6 press fixed look direction).
float g_yaw_sign     = -1.0f;
float g_pitch_sign   = +1.0f;   // 2026-08-20: in-headset, -1 looked DOWN when looking up; F6 toggles
float g_hmd_yaw      = 0.0f;    // current delta, radians
float g_hmd_pitch    = 0.0f;

// 6DOF. The HMD's positional delta from its reference, mapped through the game
// camera's basis into world space. Frostbite is metric and OpenVR reports
// meters, so this is 1:1 - lean 20cm in the room, lean 20cm in the world.
// END toggles; F5 recenters position along with orientation.
bool  g_pos_enabled  = true;
float g_ref_pos[3]   = {};

// The reference head POSE (OpenVR 3x4), captured at recenter. The Euler
// reference above is kept for the status line, the HUD anchor and the turn
// accumulator - all of which genuinely want a scalar heading - but the view
// correction uses this, so head ROLL survives and the yaw/pitch composition
// cannot inject roll of its own. See draw_policy.h for the derivation.
float g_ref_head_pose[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
bool  g_have_ref_pose = false;

// Deliberate turns (snap turn) rotate the virtual body by this much. It used
// to be folded into g_ref_yaw; with a full-orientation reference it has to be
// its own term, or a turn would be indistinguishable from a head movement.
// Deliberate body turns (snap turn): the view is MEANT to follow these, and
// they must never be clamped or bled away.
float g_turn_yaw = 0.0f;

// The aim loop's view-hold offset: how far the presented view leads the body
// because the aim loop turned the body under it. Opposite in intent to
// g_turn_yaw - the view must NOT follow - which is why the two cannot share one
// variable. They did, and the result was that a snap turn and an aim turn moved
// the same number in opposite directions.
float g_aim_view_offset = 0.0f;

// What the view actually uses: both contributions together.
float view_turn_yaw() { return g_turn_yaw + g_aim_view_offset; }

// The controller direction that means "aligned with the game's aim", captured
// rather than assumed. Without it the raw grip axis was treated as the barrel -
// OpenVR's legacy pose is the GRIP pose, whose forward runs along the handle,
// not the muzzle - which left the loop chasing a standing ~100 degree error it
// could never close.
float g_aim_ref_dir[3] = { 0.0f, 0.0f, 1.0f };
bool  g_aim_ref_valid = false;

// The view may lead the body by at most this much, and drifts back toward
// alignment when the loop is idle. Unbounded it reached -40 degrees, at which
// point "straight ahead" no longer meant "where my body faces".
// How far the presented view may diverge from the body while the aim loop is
// holding it still. In continuous mode the loop tracks the gun all the time so
// the error at any instant is a degree or two and 25 is never approached. In
// firing mode the player aims freely and pulls the trigger with the gun 40-70
// degrees off the body; the whole swing has to be absorbed or the excess shows
// up as a view jump ("my head snapped") and then bleeds back as a slow drift.
constexpr float kMaxTurnOffsetContinuous = 25.0f  * kPi / 180.0f;
constexpr float kMaxTurnOffsetFiring     = 120.0f * kPi / 180.0f;
float g_max_turn_offset = kMaxTurnOffsetContinuous;
constexpr float kTurnBleedPerFrame = 0.02f * kPi / 180.0f;
bool  g_full_orientation = true;   // 'headroll off' falls back to yaw+pitch
float g_hmd_dpos[3]  = {};      // delta in OpenVR axes (+X right, +Y up, -Z fwd)

// Alternate-eye rendering state. The IPD is in WORLD units and the engine's
// world scale is not yet measured, so it is runtime-adjustable: if the world
// feels like a miniature, the offset is too big; like a giant's world, too
// small. F2/F3 tune it, F1 swaps eyes (swapped eyes = depth pops inward).
// Largest single-frame change in the world frustum we will believe. The player
// cannot go from 124 to 18.5 degrees between two Presents; a jump that big is
// another camera's projection, not theirs.
constexpr float kMaxProjShrinkPerFrame = 0.5f;
// How many consecutive implausible readings before we conclude the projection
// really did change and accept it. Bounds the cost of a wrong hold to a few
// frames, and stops the gate latching onto whatever it saw first.
constexpr unsigned kWorldProjRunToAccept = 10;
// Same idea for the camera heading. Kept short: every frame spent refusing is a
// frame the aim loop cannot run and a snap turn would be discarded.
constexpr unsigned kCamYawRunToAccept = 10;
unsigned g_world_proj_rejected = 0;
unsigned g_world_proj_run = 0;

int   g_frame_eye    = 0;       // eye the CURRENT frame is being rendered for
int   g_last_eye     = 0;       // eye the just-presented frame used
float g_ipd_world    = 0.064f;  // world-units offset between eyes
float g_eye_swap     = 1.0f;    // -1 swaps left/right

// Zero-calibration: IPD comes from the headset's own setting (Frostbite is
// metric, so meters ARE world units), and tracking engages by itself when a
// pose appears. F2/F3 switch to manual mode for world-scale experiments;
// F7 remains the master toggle and remembers an explicit off.
bool g_ipd_manual    = false;
bool g_auto_engaged  = false;   // auto-enable fired once
bool g_user_disabled = false;   // user explicitly F7'd off - respect it

// Yaw/pitch of the HMD forward vector in OpenVR space (right-handed, Y-up,
// forward -Z). Yaw 0 = facing -Z; positive = turning toward -X (left).
void hmd_yaw_pitch(float& yaw, float& pitch)
{
    float r[9];
    vrtrack::orientation(r);
    // forward = R * (0,0,-1), rows are r[0..2], r[3..5], r[6..8]
    const float fx = -r[2], fy = -r[5], fz = -r[8];
    yaw   = std::atan2(fx, -fz);
    const float len = std::sqrt(fx * fx + fz * fz);
    pitch = std::atan2(fy, len > 1e-6f ? len : 1e-6f);
}

// Write-pattern evidence. "Which (start,count) shapes arrive per frame, and
// how many did the probe actually modify?" separates a base that hits one
// consistent matrix slot from one that corrupts mixed data - the difference
// between the world rotating and the screen tearing.
struct WriteShape { unsigned start, count, calls; };
constexpr size_t kMaxShapes = 32;
WriteShape g_shapes[kMaxShapes];
size_t     g_shape_count    = 0;
unsigned   g_modified_this_frame = 0;
unsigned   g_modified_last_frame = 0;

void note_shape(unsigned start, unsigned count)
{
    for (size_t i = 0; i < g_shape_count; ++i) {
        if (g_shapes[i].start == start && g_shapes[i].count == count) { ++g_shapes[i].calls; return; }
    }
    if (g_shape_count < kMaxShapes) g_shapes[g_shape_count++] = { start, count, 1 };
}

// Row-major, row-vector convention (v' = v * M) - what Phase 2 measured.
// Mat4 and its helpers live in mat4.h so the pure policy code shares them.

float g_vp[16]         = {};   // last seen view-projection, from c185-c188
float g_vp_inv[16]     = {};   // its inverse, refreshed with the correction
bool  g_have_vp        = false;
bool  g_have_vp_inv    = false;
// Declared here rather than beside g_eye below: promote_player_camera() sets it
// and is defined with the candidate table, above that point.
bool  g_have_eye       = false;

// Fingerprint cache for "constants"-blob shaders: (shader, write start) ->
// matrix offset within the write in vec4s, or -1 for "scanned, none found".
// Guarded by g_fp_mutex; only touched when the active shader is uncharted.
std::mutex g_fp_mutex;
std::map<std::pair<void*, unsigned>, int> g_fingerprints;
unsigned g_fp_found = 0, g_fp_rejected = 0;
float g_eye[3]         = {};   // camera position, from c192
float g_cam_right[3]   = { 1, 0, 0 };   // camera right axis, from c189 row 0
float g_cam_rows[9]    = { 1,0,0, 0,1,0, 0,0,1 };   // full camera-to-world 3x3

// Camera candidates seen during the current frame.
//
// c185..c192 is ONE contiguous block holding both the view-projection and the
// camera-to-world transform, and every pass that renders geometry writes it -
// shadow cascades, reflections, the main scene. Latching "whoever wrote last"
// meant a foreign pass could take both at once: the world field collapsed from
// 58.9 to 18.5 degrees (black-boxing the headset and making the weapon
// unclassifiable) while g_cam_rows simultaneously took that camera's heading,
// which stalled the aim loop and threw away every snap turn.
//
// So collect them instead, and pick the player's at end of frame.
constexpr int kMaxCamCandidates = 8;
struct CamCandidate {
    float vp[16] = {};
    float rows[9] = {};
    float right[3] = {};
    float eye[3] = {};
    bool  have_cam = false;
    drawpolicy::ProjParams proj{};
    unsigned weight = 0;          // constant writes that used this camera
};
CamCandidate g_cand[kMaxCamCandidates];
int g_cand_n = 0;
int g_cand_cur = -1;              // candidate the current write belongs to
unsigned g_cam_candidates_last = 0;
unsigned g_cam_promote_failed = 0;
drawpolicy::ProjParams g_prev_player_proj{};

bool tangents_match(const drawpolicy::ProjParams& a, const drawpolicy::ProjParams& b)
{
    if (a.perspective != b.perspective) return false;
    if (!a.perspective) return true;          // all non-perspective lumped together
    const float ah = a.tan_half_h(), bh = b.tan_half_h();
    const float av = a.tan_half_v(), bv = b.tan_half_v();
    if (!(ah > 1e-6f) || !(bh > 1e-6f) || !(av > 1e-6f) || !(bv > 1e-6f)) return false;
    return std::fabs(ah / bh - 1.0f) < 0.01f && std::fabs(av / bv - 1.0f) < 0.01f;
}

void note_camera_candidate(const float* vp)
{
    // Fast path. A pass writes the same view-projection for every draw in it,
    // so the overwhelmingly common case is "identical to the one we just saw".
    // The old code here was a 64-byte memcpy; recovering the projection on
    // every write instead would put real arithmetic on a very hot path.
    if (g_cand_cur >= 0 && g_cand_cur < g_cand_n &&
        std::memcmp(vp, g_cand[g_cand_cur].vp, sizeof(g_cand[g_cand_cur].vp)) == 0) {
        ++g_cand[g_cand_cur].weight;
        rpass::on_camera_write(g_cand[g_cand_cur].proj);
        return;
    }

    drawpolicy::ProjParams p{};
    drawpolicy::recover_projection(vp, p);    // failure leaves perspective=false
    rpass::on_camera_write(p);
    for (int i = 0; i < g_cand_n; ++i) {
        if (!tangents_match(g_cand[i].proj, p)) continue;
        ++g_cand[i].weight;
        std::memcpy(g_cand[i].vp, vp, sizeof(g_cand[i].vp));
        g_cand_cur = i;
        return;
    }
    if (g_cand_n >= kMaxCamCandidates) { g_cand_cur = -1; return; }
    CamCandidate& c = g_cand[g_cand_n];
    c = CamCandidate{};
    std::memcpy(c.vp, vp, sizeof(c.vp));
    c.proj = p;
    c.weight = 1;
    g_cand_cur = g_cand_n;
    ++g_cand_n;
}

void attach_camera_block(const float* rows)
{
    // The camera-to-world block belongs to whichever view-projection was named
    // most recently. They are usually written by the same call.
    if (g_cand_cur < 0 || g_cand_cur >= g_cand_n) return;
    CamCandidate& c = g_cand[g_cand_cur];
    const float* row3 = rows + 3 * 4;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            c.rows[r * 3 + k] = rows[r * 4 + k];
    c.right[0] = c.rows[0]; c.right[1] = c.rows[1]; c.right[2] = c.rows[2];
    c.eye[0] = row3[0]; c.eye[1] = row3[1]; c.eye[2] = row3[2];
    c.have_cam = true;
    rpass::on_camera_heading(c.rows);
}

// End of frame: promote the player's camera, or keep the last good one.
void promote_player_camera(float aspect)
{
    g_cam_candidates_last = static_cast<unsigned>(g_cand_n);
    if (g_cand_n > 0) {
        drawpolicy::ProjParams cands[kMaxCamCandidates];
        unsigned weights[kMaxCamCandidates];
        for (int i = 0; i < g_cand_n; ++i) { cands[i] = g_cand[i].proj; weights[i] = g_cand[i].weight; }
        const int win = drawpolicy::choose_player_camera(
            cands, weights, g_cand_n, aspect,
            g_prev_player_proj.perspective ? &g_prev_player_proj : nullptr);
        if (win >= 0) {
            const CamCandidate& c = g_cand[win];
            std::memcpy(g_vp, c.vp, sizeof(g_vp));
            g_have_vp = true;
            if (c.have_cam) {
                std::memcpy(g_cam_rows, c.rows, sizeof(g_cam_rows));
                std::memcpy(g_cam_right, c.right, sizeof(g_cam_right));
                std::memcpy(g_eye, c.eye, sizeof(g_eye));
                g_have_eye = true;
            }
            g_prev_player_proj = c.proj;
            rpass::on_camera_elected(c.proj);
        } else {
            // Nothing plausible this frame - hold what we had rather than
            // adopting a pass we have already judged is not the player's.
            ++g_cam_promote_failed;
        }
    }
    g_cand_n = 0;
    g_cand_cur = -1;
}

// FOV widening: scale the camera's x/y axes down inside the correction, so the
// game renders a WIDER field into the same image. game_proj_tangents reports
// the widened tangents, so the compositor keeps displaying at correct angular
// size - the black border shrinks instead of the image stretching.
//
// AUTO: the factors are computed from the headset's own frustum
// (GetProjectionRaw) against the game's native tangents, per axis. They must
// be per-axis because the Index's frustum is nearly square (~2.6 x 2.8 in
// tangent units) while the game renders 16:9 - a uniform factor would either
// leave the top and bottom black or waste enormous horizontal field.
//
// Cost, stated plainly: the engine still CPU-culls at its native frustum, so
// edge pop-in grows with these factors, and the same backbuffer now covers
// more degrees, so the image softens. PgDn trades field back for sharpness.
// 2026-08-20: AUTO IS OFF BY DEFAULT. With K live (it was dead until today)
// the full 2.6x/3.4x match rendered far outside the ~58x45 deg cone the
// ENGINE culls against (settings.ini Fov does nothing - tangents identical at
// 55 and 90), so sky, terrain patches and trees outside it were never drawn:
// gray void over most of the eye. Until the engine's own FOV is patched
// (engine-hook track), the honest default is 1.0: correct geometry in a
// theater window. PgUp/PgDn widen manually; Shift+PgUp toggles the auto match
// back on to see the void for yourself.
float g_fov_widen_x = 1.0f;
float g_fov_widen_y = 1.0f;
float g_fov_manual  = 1.0f;   // PgUp/PgDn multiplier (on top of auto when auto is on)
bool  g_fov_auto    = false;

// Viewmodel handling (2026-08-20, after the BFVR study - docs/prior-art-bfvr.md).
//
// Distance-to-camera was the wrong discriminator: at 2.0m it matched ~700
// draws a frame. Draws are now classified per WRITE by the pure policy in
// draw_policy.cpp from three facts: the active shader declares a bone palette,
// the object's origin recovered in VIEW space sits at the eye, and - the
// decisive one - whether the draw's own projection (recovered from its WVP)
// differs from the world's. A viewmodel rendered with its own FOV/near plane
// is both the reason the weapon distorted under the global correction (which
// assumes P) and the cleanest way to recognise it.
//
// Classified draws get their own correction,  P_vm^-1 * Delta * C_view * P_sel,
// built from the frame's head/eye/FOV correction expressed in view space.
// Delta is the weapon's offset in the body camera's frame - an arm's-length
// push today, a controller grip delta later - applied BEFORE the eye offset so
// both eyes share one adjusted weapon pose. Minus/Equals tune the push,
// DELETE cycles which projection the weapon is re-rendered with.
// FINDING 2026-08-20 (census 08): ~94% of the scene is drawn with a near
// plane of 7.48 m or 21.34 m - the engine renders in DEPTH SLICES - while VP
// carries 0.1 m. For such a draw the global VP^-1 r VP leaves a P' P^-1
// residue that multiplies every translation in r (eye offset, 6DOF lean,
// push) by t'/t = 75 or 213 while rotation stays exact: looking around
// worked, stereo and leaning warped the world. So the correction is now
// built per draw around the draw's OWN projection (cached per distinct P,
// ~4 per frame); the global form is just the P' == P special case and stays
// as the fallback for writes whose projection cannot be recovered.
// Static weapon offset in the body camera's frame, metres: +x right, +y up,
// +z forward. The engine draws the viewmodel where a flat game wants it, and
// calibration cannot move it (calibration only zeroes the DELTA), so bringing
// the weapon to where a held rifle belongs needs this. z goes NEGATIVE to pull
// it toward the eye - the first version only allowed pushing away, which was
// the wrong direction for the complaint it was meant to answer.
float g_vm_off[3] = { 0.0f, 0.0f, 0.0f };
float& g_vm_push = g_vm_off[2];   // '-'/'=' and the 'push' command drive z

// PHASE 7a - the weapon follows a motion controller.
//
// The offset slot the push occupies is exactly the slot BFVR feeds its grip
// delta into (World * sourceView * gripDelta * residualEye * P), so no engine
// reverse engineering is needed for the gun to follow the hand: the delta is
// head-relative controller motion since calibration, applied in the body
// camera's frame, BEFORE the per-eye offset so both eyes share one weapon pose.
//
// PRESENTATION ONLY. The engine still owns aim, fire, recoil, reload and
// projectiles - the bullet does not follow the barrel yet, and pretending
// otherwise would be worse than not doing it. Fire direction needs either the
// engine's fire basis or synthesised look input (docs/bc2-engine.md).
// On by default: it is the headline feature, and requiring a command after
// every launch meant a session where the gun simply sat still and looked
// broken. 'grip off' still disables it.
bool  g_grip_on = true;
// Which controller holds the weapon. `input left|right` drives this now: it was
// previously settable only by the separate `grip` command, so after `input left`
// the aim loop dedup'd and gated on one controller while measuring the other,
// and the weapon model followed a third choice.
int   g_grip_hand = 1;            // 0 left, 1 right
float g_grip_units_per_metre = 1.0f;   // Frostbite is metric
float g_grip_smooth = 0.0f;       // 0 = raw; otherwise per-frame lerp factor
bool  g_grip_calibrated = false;
float g_grip_ref_head[16] = {};
float g_grip_ref_grip[16] = {};
float g_grip_delta[16] = {};      // current, view space
float g_grip_prev[16] = {};       // last accepted, for the discontinuity gate
bool  g_grip_have_prev = false;
bool  g_grip_active = false;      // a usable delta this frame
unsigned g_grip_resets = 0;
constexpr float kGripMaxJump = 0.5f;   // metres between accepted samples
int   g_vm_mode   = 2;       // weapon projection: 0 own P, 1 own P, 2 hybrid (world field, own depth), 3 world P
bool  g_ownproj_on = true;   // Shift+']' toggles, for A/B against the old global-only path
drawpolicy::Thresholds g_vm_th;
drawpolicy::ProjParams g_world_proj;      // recovered from VP once per frame
drawpolicy::ProjParams g_weapon_proj;     // the viewmodel's own P (last classified write)
unsigned g_weapon_proj_frame = 0;
float g_cview[16] = {};                   // V^-1 * r * V: the frame's correction in view space
bool  g_have_cview = false;
unsigned g_vm_hits = 0, g_vm_hits_last = 0;          // classified writes per frame
unsigned g_vm_ownp = 0, g_vm_ownp_last = 0;          // of which: own-projection evidence
unsigned g_frame_index = 0;

// Per-frame cache of the viewmodel correction, keyed by the projection it was
// built for - the weapon typically uses one P, so this is a 1-entry cache.
struct VmCorrection {
    drawpolicy::ProjParams key;
    float    zoom = 1.0f;
    bool     with_offset = false;
    unsigned frame = ~0u;
    float    c[16] = {};
    bool     valid = false;
};

// View-space distance histogram of every corrected write, so the scene's
// distribution stays measurable (and the viewmodel band visible).
constexpr float kBuckets[5] = { 0.1f, 0.5f, 1.0f, 2.0f, 5.0f };
unsigned g_vm_hist[6] = {}, g_vm_hist_last[6] = {};

// Render-state / render-target shadow for the draw census. D3D9 state is
// device-global and the game renders from one thread, so plain globals.
unsigned g_rs_z = 1, g_rs_zw = 1, g_rs_ab = 0;
bool     g_rt_is_bb = true;
unsigned g_rt_w = 0, g_rt_h = 0;

// The last WVP analysed on this thread, attributed to the draw that follows.
struct PendingWvp {
    const void* shader = nullptr;
    bool  valid = false;
    drawpolicy::ProjParams p;
    drawpolicy::ProjClass  pc = drawpolicy::ProjClass::NoWvp;
    drawpolicy::DrawClass  cls = drawpolicy::DrawClass::Unclassified;
    float view_origin[3] = {};
    float view_dist = -1.0f;
    float w33 = 0.0f, w23 = 0.0f;
    bool  have_bone0 = false;
    float bone0[12] = {};
    bool  bones_at_write = false;
    bool  require_bones  = true;
};
thread_local PendingWvp t_pending;

// Hidden viewmodel shaders (by bytecode hash prefix): their draws are skipped
// while the write was classified Viewmodel - the first-person arms reach
// behind the near plane at headset FOV and slice into blades; a floating gun
// is the VR convention and what the controller-held gun wants anyway.
constexpr size_t kMaxHidden = 16;
// Default: the first-person ARMS + gloves shader (FNV-1a of its bytecode,
// stable across launches; found by elimination with eye shots 2026-08-20).
// The gun body (2906751e...) and its parts stay.
unsigned long long g_hide[kMaxHidden] = { 0x76fbfb1ef6146ed9ull };
unsigned g_hide_bits[kMaxHidden] = { 16 };   // number of hex digits given (prefix match)
size_t g_hide_n = 1;
unsigned g_hidden_draws = 0, g_hidden_draws_last = 0;

// A frame must carry at least this many corrected 3D transform writes before
// any draw in it can be treated as HUD (see on_draw).
constexpr unsigned kHudMin3DWrites = 50;

bool hash_hidden(unsigned long long h)
{
    for (size_t i = 0; i < g_hide_n; ++i) {
        const unsigned shift = 64 - g_hide_bits[i] * 4;
        if ((h >> shift) == (g_hide[i] >> shift)) return true;
    }
    return false;
}

float g_correction[16] = {};   // VP^-1 * R * VP, rebuilt once per frame
bool  g_have_correction = false;


// The game's native (un-widened) projection half-angle tangents, recovered
// from VP.
//
// BUG FIXED 2026-08-20: this used the ROWS of VP (1/|row|). In row-vector
// math VP = V * P puts the projection factors in the COLUMNS: column 0 is
// a * (V column 0), so |col0.xyz| = a exactly; a row mixes a, b and q with the
// camera rotation, so the "tangent" drifted with head pitch/yaw (the log's
// v=0.41..0.98 while "standing still" - read at the time as the game animating
// its FOV). While K was dead that only breathed the compositor bounds; once K
// went live it scaled the render as well, and the world warped with every head
// movement. The column recovery is the same one draw_policy uses per draw.
bool base_tangents(float& th, float& tv)
{
    if (!g_have_vp) return false;
    drawpolicy::ProjParams p;
    if (!drawpolicy::recover_projection(g_vp, p)) return false;
    th = p.tan_half_h();
    tv = p.tan_half_v();
    return th > 1e-6f && tv > 1e-6f;
}

// Match the rendered field to the headset's frustum, per axis. Recomputed
// rarely - the frustum is a device constant, but VP is only known once the
// game starts drawing.
void update_auto_fov()
{
    if (!g_fov_auto) { g_fov_widen_x = g_fov_widen_y = 1.0f; return; }
    auto* sys = vrtrack::system();
    if (!sys) return;

    float th = 0.0f, tv = 0.0f;
    if (!base_tangents(th, tv)) return;

    float need_h = 0.0f, need_v = 0.0f;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        sys->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
        need_h = (std::max)(need_h, (std::max)(std::fabs(l), std::fabs(r)));
        need_v = (std::max)(need_v, (std::max)(std::fabs(t), std::fabs(b)));
    }
    if (need_h < 1e-6f || need_v < 1e-6f) return;

    // Recomputed EXACTLY every frame, with no change threshold. The game
    // animates its own FOV (sprint, ADS, movement bob) - the log showed
    // tangents drifting 0.683 -> 0.701 within seconds. A stale widen factor
    // turns that drift into a breathing field of view, which is both wrong in
    // VR and mismatched against the compositor bounds. Tracking it exactly
    // pins the rendered field to the headset's frustum as a constant, so the
    // game's zoom no longer reaches the eyes.
    const float wx = (std::min)((std::max)(need_h / th, 1.0f), 6.0f);
    const float wy = (std::min)((std::max)(need_v / tv, 1.0f), 6.0f);

    const bool worth_logging = std::fabs(wx - g_fov_widen_x) > 0.15f ||
                               std::fabs(wy - g_fov_widen_y) > 0.15f;
    g_fov_widen_x = wx;
    g_fov_widen_y = wy;
    if (worth_logging) {
        VRLOG("[fov] auto: game tangents h=%.4f v=%.4f, headset needs h=%.4f v=%.4f -> widen %.2fx / %.2fx",
              th, tv, need_h, need_v, wx, wy);
    }
}


bool key_pressed(int vk)
{
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return fired;
}

bool covers(unsigned start, unsigned count, unsigned base)
{
    return start <= base && (start + count) >= (base + 4);
}

// Does the 4-register window at `window` (transposed storage) hold a WVP?
// If S^T really is World * VP, then S^T * VP^-1 recovers World, which must be
// affine: last column (0,0,0,1). Bone rows, packed params, and lighting data
// do not survive that test.
bool window_is_wvp(const float* window)
{
    if (!g_have_vp_inv) return false;

    Mat4 m, world;
    transpose(reinterpret_cast<const float(&)[16]>(*window), m);
    multiply(m, g_vp_inv, world);

    const float w33 = at(world, 3, 3);
    if (std::fabs(w33 - 1.0f) > 0.02f) return false;
    for (int r = 0; r < 3; ++r) {
        if (std::fabs(at(world, r, 3)) > 0.02f) return false;
    }
    // Degenerate guard: an all-zero window would pass the column test trivially
    // if VP^-1 had zeros in the right places; require a non-trivial diagonal.
    if (std::fabs(at(world, 0, 0)) + std::fabs(at(world, 1, 1)) + std::fabs(at(world, 2, 2)) < 0.1f) return false;
    return true;
}

// For a write from an uncharted ("constants"-blob) shader: find the WVP window
// within it, scanning once per (shader, write start) and caching the result.
int fingerprint_offset(void* shader, unsigned start, const float* data, unsigned vec4_count)
{
    const auto key = std::make_pair(shader, start);
    {
        std::lock_guard<std::mutex> lock(g_fp_mutex);
        auto it = g_fingerprints.find(key);
        if (it != g_fingerprints.end()) return it->second;
    }

    int found = -1;
    if (vec4_count >= 4) {
        for (unsigned off = 0; off + 4 <= vec4_count; ++off) {
            if (window_is_wvp(data + off * 4)) { found = static_cast<int>(off); break; }
        }
    }

    std::lock_guard<std::mutex> lock(g_fp_mutex);
    g_fingerprints[key] = found;
    if (found >= 0) {
        ++g_fp_found;
        VRLOG("[fingerprint] shader %p write c%u+%u: WVP at offset %d (c%u)",
              shader, start, vec4_count, found, start + found);
    } else {
        ++g_fp_rejected;
    }
    return found;
}

// The REFERENCE head pose, captured at recenter: YAW ONLY.
//
// The reference answers "which way is the body facing", and nothing else. An
// earlier version stored the whole head orientation, which meant any pitch or
// roll present at the moment of capture became a permanent offset - capture
// while the headset is tilted on a desk, or at an angle on the player's head,
// and the world is tilted forever after, even when looking straight ahead.
// That is exactly what happened in the headset.
//
// Keeping only yaw makes the reference gravity-aligned, so the head's pitch and
// roll are always measured against level. BFVR does the same for the same
// reason: its anchor stores a yaw-only base so pitch and roll are never zeroed.
bool head_reference_now(float out[12])
{
    if (!vrtrack::have_pose()) return false;
    float yaw = 0.0f, pitch = 0.0f;
    hmd_yaw_pitch(yaw, pitch);
    float pos[3];
    vrtrack::position(pos);

    // A pure yaw about world up, in OpenVR's column-vector convention, with
    // yaw measured the way hmd_yaw_pitch reports it (0 = facing -Z).
    const float c = std::cos(yaw), sn = std::sin(yaw);
    const float R[3][3] = { {  c, 0.0f, sn },
                            { 0.0f, 1.0f, 0.0f },
                            { -sn, 0.0f,  c } };
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) out[r * 4 + col] = R[r][col];
        out[r * 4 + 3] = pos[r];
    }
    return true;
}

// The live head pose as an OpenVR 3x4, the form draw_policy converts.
bool head_pose_now(float out[12])
{
    if (!vrtrack::have_pose()) return false;
    float rot[9], pos[3];
    vrtrack::orientation(rot);
    vrtrack::position(pos);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = rot[r * 3 + c];
        out[r * 4 + 3] = pos[r];
    }
    return true;
}

// Head-relative controller motion since calibration, in view space. Fails
// closed: any missing or implausible sample drops the weapon back to its
// native pose rather than putting it somewhere invented.
void update_grip_delta()
{
    g_grip_active = false;
    if (!g_grip_on || !g_hmd_active) { g_grip_calibrated = false; return; }

    float grip_pose[12], head_rot[9], head_pos[3];
    if (!vrtrack::controller_pose(g_grip_hand, grip_pose)) { g_grip_calibrated = false; return; }
    vrtrack::orientation(head_rot);
    vrtrack::position(head_pos);

    // The head as an OpenVR-style 3x4, so both go through one conversion.
    float head_pose[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) head_pose[r * 4 + c] = head_rot[r * 3 + c];
        head_pose[r * 4 + 3] = head_pos[r];
    }

    Mat4 head_view, grip_view;
    drawpolicy::openvr_pose_to_view(head_pose, head_view);
    drawpolicy::openvr_pose_to_view(grip_pose, grip_view);

    if (!g_grip_calibrated) {
        std::memcpy(g_grip_ref_head, head_view, sizeof(g_grip_ref_head));
        std::memcpy(g_grip_ref_grip, grip_view, sizeof(g_grip_ref_grip));
        g_grip_calibrated = true;
        identity(g_grip_delta);
        identity(g_grip_prev);
        g_grip_have_prev = true;
        g_grip_active = true;
        VRLOG("[grip] calibrated on the %s controller - the weapon now follows it",
              g_grip_hand ? "right" : "left");
        return;
    }

    Mat4 delta;
    if (!drawpolicy::make_grip_delta(head_view, grip_view,
                                     reinterpret_cast<const float(&)[16]>(*g_grip_ref_head),
                                     reinterpret_cast<const float(&)[16]>(*g_grip_ref_grip),
                                     g_grip_units_per_metre, delta)) {
        g_grip_calibrated = false; ++g_grip_resets;
        return;
    }
    if (g_grip_have_prev &&
        !drawpolicy::grip_delta_step_is_sane(reinterpret_cast<const float(&)[16]>(*g_grip_prev),
                                             delta, kGripMaxJump * g_grip_units_per_metre)) {
        // A jump this big in one frame is a tracking glitch, not a movement:
        // recalibrate instead of teleporting the weapon.
        g_grip_calibrated = false; g_grip_have_prev = false; ++g_grip_resets;
        VRLOG("[grip] discontinuity - recalibrating (%u)", g_grip_resets);
        return;
    }
    std::memcpy(g_grip_prev, delta, sizeof(g_grip_prev));
    g_grip_have_prev = true;
    if (g_grip_smooth > 0.001f) {
        const float a = (std::min)(g_grip_smooth, 1.0f);
        for (int i = 0; i < 16; ++i) g_grip_delta[i] += (delta[i] - g_grip_delta[i]) * a;
    } else {
        std::memcpy(g_grip_delta, delta, sizeof(g_grip_delta));
    }
    g_grip_active = true;
}

// CORRECTION = VP^-1 * RotAboutEye * VP.
// Valid for any matrix ending in clip space, so the same value corrects a
// per-object WVP and the global VP alike.
void rebuild_correction()
{
    g_have_correction = false;
    if (!g_have_vp || !g_have_eye) return;

    Mat4 vp_inv;
    if (!invert(g_vp, vp_inv)) {
        static bool warned = false;
        if (!warned) { warned = true; VRLOG("[override] VP is singular - cannot build correction"); }
        g_have_vp_inv = false;
        return;
    }
    std::memcpy(g_vp_inv, vp_inv, sizeof(g_vp_inv));
    g_have_vp_inv = true;

    // The rotation. The FULL relative head orientation - roll included, and
    // composed so that yaw and pitch together cannot cant the horizon. The old
    // Euler pair did both wrong; docs/head-orientation.md has the measurements.
    Mat4 rot;
    bool have_full = false;
    float head_now[12];
    if (g_hmd_active && g_full_orientation && g_have_ref_pose && head_pose_now(head_now)) {
        have_full = drawpolicy::head_world_rotation(head_now, g_ref_head_pose, view_turn_yaw(),
                                                    g_cam_rows, rot);
    }
    if (!have_full) {
        if (g_hmd_active) {
            // Fallback ('headroll off', or before the first reference pose):
            // the original yaw+pitch construction, kept so the mod still runs
            // if the full path ever refuses.
            const float wy[3] = { 0.0f, 1.0f, 0.0f };
            Mat4 ry, rp;
            rotation_axis(wy, g_yaw_sign * (g_hmd_yaw + view_turn_yaw()), ry);
            rotation_axis(g_cam_right, g_pitch_sign * g_hmd_pitch, rp);
            multiply(rp, ry, rot);
        } else {
            rotation_y(g_angle_rad, rot);
        }
    }

    Mat4 to_origin, back, r, tmp;
    translation(-g_eye[0], -g_eye[1], -g_eye[2], to_origin);
    translation(g_eye[0], g_eye[1], g_eye[2], back);
    multiply(to_origin, rot, tmp);
    multiply(tmp, back, r);          // rotate about the eye, not the origin

    // 6DOF: map the head's positional delta through the camera basis and move
    // the WORLD the opposite way, which is the same as moving the camera.
    // OpenVR is +X right / +Y up / -Z forward, hence the negated Z term.
    if (g_pos_enabled && g_hmd_active) {
        float off[3];
        // Rotate the delta into the frame the recenter defined before mapping
        // it through the camera basis. Mapping the raw tracking axes straight
        // through - as this did originally - is only correct when the
        // reference happened to face the tracking origin's forward; at any
        // other heading, leaning sideways slid the player the wrong way.
        if (!(g_have_ref_pose &&
              drawpolicy::lean_in_world(g_hmd_dpos, g_ref_head_pose, view_turn_yaw(), g_cam_rows, off))) {
            const float* right = &g_cam_rows[0];
            const float* up    = &g_cam_rows[3];
            const float* fwd   = &g_cam_rows[6];
            for (int i = 0; i < 3; ++i) {
                off[i] = g_hmd_dpos[0] * right[i]
                       + g_hmd_dpos[1] * up[i]
                       + (-g_hmd_dpos[2]) * fwd[i];
            }
        }
        Mat4 move, r2;
        translation(-off[0], -off[1], -off[2], move);
        multiply(r, move, r2);
        std::memcpy(r, r2, sizeof(r));
    }

    // Stereo eye offset: shift the camera half an IPD along its right axis.
    // Shifting the CAMERA right by d == shifting the WORLD left by d, so the
    // world translation is -d for the right eye, +d for the left. After the
    // rotation above the fixed camera's axes ARE the head's axes, so this is
    // the head's right.
    if (g_hmd_active) {
        // Note: F1 eye-swap is applied at SUBMISSION (last_rendered_eye), not
        // here - applying it in both places would cancel itself out.
        const float half = 0.5f * g_ipd_world;
        const float side = (g_frame_eye == 0) ? +half : -half;
        // Along the HEAD's right, not the body's: a tilted head must tilt the
        // stereo baseline with it, or the eyes stay level while the view rolls.
        float axis[3] = { g_cam_right[0], g_cam_right[1], g_cam_right[2] };
        if (!(g_have_ref_pose && head_pose_now(head_now) &&
              drawpolicy::head_right_in_world(head_now, g_ref_head_pose, view_turn_yaw(), g_cam_rows, axis))) {
            axis[0] = g_cam_right[0]; axis[1] = g_cam_right[1]; axis[2] = g_cam_right[2];
        }
        Mat4 eye_shift, r2;
        translation(side * axis[0], side * axis[1], side * axis[2], eye_shift);
        multiply(r, eye_shift, r2);
        std::memcpy(r, r2, sizeof(r));
    }

    // Camera-to-world (M_cw) and its affine inverse (V) from c189-c192. Needed
    // by the FOV scale below and by the view-space form of the correction.
    Mat4 mcw, view;
    identity(mcw);
    for (int r2 = 0; r2 < 3; ++r2)
        for (int c2 = 0; c2 < 3; ++c2)
            at(mcw, r2, c2) = g_cam_rows[r2 * 3 + c2];
    at(mcw, 3, 0) = g_eye[0]; at(mcw, 3, 1) = g_eye[1]; at(mcw, 3, 2) = g_eye[2];
    identity(view);
    for (int r2 = 0; r2 < 3; ++r2)
        for (int c2 = 0; c2 < 3; ++c2)
            at(view, r2, c2) = g_cam_rows[c2 * 3 + r2];
    for (int c2 = 0; c2 < 3; ++c2) {
        at(view, 3, c2) = -(g_eye[0] * at(view, 0, c2) +
                            g_eye[1] * at(view, 1, c2) +
                            g_eye[2] * at(view, 2, c2));
    }

    // FOV widening: K = V * S * M_cw, a camera-space lateral shrink conjugated
    // into world space. Scaling camera x/y by 1/widen before projection makes
    // the same image cover widen-times the field.
    //
    // Applied LAST, in the projecting camera's frame. BUG FIXED 2026-08-20:
    // since 0afea3b this product was multiplied into `rot` AFTER `r` had been
    // built from it, so K never reached g_correction - the game kept rendering
    // its native field while game_proj_tangents told the compositor the image
    // was 2-3x wider, and the bounds simply MAGNIFIED the native image. That
    // magnification is a large part of why the weapon sat in the face.
    const float wx = g_fov_widen_x * g_fov_manual;
    const float wy = g_fov_widen_y * g_fov_manual;
    if ((wx > 1.001f || wy > 1.001f) && g_have_eye) {
        Mat4 sc, tmp2, k, r2;
        scale(1.0f / wx, 1.0f / wy, 1.0f, sc);
        multiply(view, sc, tmp2);
        multiply(tmp2, mcw, k);
        multiply(r, k, r2);
        std::memcpy(r, r2, sizeof(r));
    }

    multiply(vp_inv, r, tmp);
    multiply(tmp, g_vp, g_correction);
    g_have_correction = true;

    // The same correction in VIEW space: C_view = V^-1 * r * V = M_cw * r * V.
    // The viewmodel path needs it because it swaps the projection around it.
    {
        Mat4 t1;
        multiply(mcw, r, t1);
        multiply(t1, view, g_cview);
        g_have_cview = true;
    }

    // Controller-held weapon: refresh the grip delta for this frame.
    update_grip_delta();

    // The world projection, recovered from VP the same way per-draw
    // projections are, so comparisons are like against like.
    //
    // Gated, because everything downstream trusts it: the eye bounds are
    // computed from these tangents, and the viewmodel classifier keys on the
    // weapon's projection DIFFERING from this one. A frame where a shadow,
    // reflection or scope pass wrote VP last reports a frustum that is not the
    // player's, and taking it costs the whole image - a collapse to 18.5 deg
    // black-boxed the headset and made the gun unclassifiable in one go.
    //
    // The test is on a single-frame COLLAPSE, not on an absolute value, so a
    // legitimately narrow field (a scope) still comes through as long as it
    // arrives at a believable rate.
    drawpolicy::ProjParams fresh{};
    if (!drawpolicy::recover_projection(g_vp, fresh)) {
        g_world_proj = drawpolicy::ProjParams{};
    } else if (g_world_proj.perspective && fresh.perspective) {
        const float was = g_world_proj.tan_half_v(), now = fresh.tan_half_v();
        if (was > 1e-4f && now > 1e-4f &&
            (now < was * kMaxProjShrinkPerFrame || was < now * kMaxProjShrinkPerFrame) &&
            ++g_world_proj_run < kWorldProjRunToAccept) {
            ++g_world_proj_rejected;   // hold the last believable one
        } else {
            // Persisting means it is real, not a stray pass - a genuine scope
            // toggle, or a first frame that captured the wrong camera. Refusing
            // forever would latch the mod to whichever projection it happened to
            // see first, which is a worse failure than the one being guarded.
            g_world_proj_run = 0;
            g_world_proj = fresh;
        }
    } else {
        g_world_proj = fresh;
    }
    // Per-draw corrections are rebuilt lazily; frame index keys the cache.
}

// Correction for a draw rendered with its own projection `p`, cached per
// frame. with_offset = the weapon (push / grip delta, projection per DELETE
// mode); without = own-projection scene passes (corrected around their own P,
// which is kept, no offset). Null = caller uses the global correction.
constexpr size_t kVmCache = 8;
VmCorrection g_owncache[kVmCache];   // per distinct projection this frame
unsigned g_owncache_next = 0;

const float* viewmodel_correction(const drawpolicy::ProjParams& p, bool with_offset)
{
    if (!g_have_cview || !g_ownproj_on) return nullptr;

    // Weapon: offset + projection per mode. Everything else: around its own
    // P (a/b snapped to the world's when within tolerance), no offset.
    drawpolicy::ProjParams psel = p;
    // ADS / scope. We hold the engine's FOV at the headset's field, so the
    // game's own zoom never reaches the render. Re-apply it here as a
    // magnification of the projection instead - BFVR's
    // ComputeD3D8ScopeProjectionScale, scale = tan(normal/2)/tan(zoom/2) -
    // which keeps the optical centre and the near/far interval intact.
    const float zoom = memscan::engine_zoom();
    if (with_offset) {
        drawpolicy::ProjSelect sel = drawpolicy::ProjSelect::Viewmodel;
        if (g_vm_mode == 2) sel = drawpolicy::ProjSelect::Hybrid;
        if (g_vm_mode == 3) sel = drawpolicy::ProjSelect::World;
        psel = drawpolicy::select_projection(sel, p, g_world_proj);
    } else {
        psel = drawpolicy::correction_projection(p, g_world_proj);
    }
    if (zoom > 1.001f) { psel.a *= zoom; psel.b *= zoom; }

    // Cache per (p, with_offset) for this frame.
    VmCorrection* slot = nullptr;
    for (size_t i = 0; i < kVmCache; ++i) {
        VmCorrection& vc = g_owncache[i];
        // A weapon slot is only reusable within the frame it was built for -
        // the grip delta changes every frame, and the frame check covers that.
        if (vc.valid && vc.frame == g_frame_index && vc.with_offset == with_offset &&
            vc.key.a == p.a && vc.key.b == p.b && vc.key.q == p.q && vc.key.t == p.t &&
            vc.zoom == zoom) return vc.c;
        if (!slot && (!vc.valid || vc.frame != g_frame_index)) slot = &vc;
    }
    if (!slot) slot = &g_owncache[g_owncache_next++ % kVmCache];   // round-robin if a frame has more

    // The weapon's offset in the body camera's frame: the controller delta
    // when one is live, otherwise the manual push.
    Mat4 delta;
    const bool any_offset = std::fabs(g_vm_off[0]) + std::fabs(g_vm_off[1]) + std::fabs(g_vm_off[2]) > 0.001f;
    if (with_offset && g_grip_active) {
        std::memcpy(delta, g_grip_delta, sizeof(delta));
        if (any_offset) {
            // Applied after the grip delta, so the offset lives in the
            // weapon's own frame and travels with the hand rather than being
            // pinned to the body.
            Mat4 off, combined;
            translation(g_vm_off[0], g_vm_off[1], g_vm_off[2], off);
            multiply(delta, off, combined);
            std::memcpy(delta, combined, sizeof(delta));
        }
    } else if (with_offset && any_offset) {
        translation(g_vm_off[0], g_vm_off[1], g_vm_off[2], delta);
    } else {
        identity(delta);
    }

    slot->key = p;
    slot->zoom = zoom;
    slot->with_offset = with_offset;
    slot->frame = g_frame_index;
    slot->valid = drawpolicy::build_viewmodel_correction(p, delta, g_cview, psel, slot->c);
    return slot->valid ? slot->c : nullptr;
}

} // namespace

bool transform(unsigned start_register, const float* data, unsigned vec4_count,
               std::vector<float>& scratch)
{
    if (!data || vec4_count == 0) return false;

    note_shape(start_register, vec4_count);

    // Record this camera as a candidate. It is NOT adopted here: which of the
    // frame's cameras is the player's is decided in promote_player_camera().
    if (covers(start_register, vec4_count, kViewProjBase)) {
        note_camera_candidate(data + (kViewProjBase - start_register) * 4);
    }
    if (covers(start_register, vec4_count, kCamWorldBase)) {
        attach_camera_block(data + (kCamWorldBase - start_register) * 4);
    }

    if (!g_correct_on || !g_have_correction) return false;

    // Spans the ACTIVE shader's own constant table declares as clip-space
    // transforms; for "constants"-blob shaders that declare nothing useful,
    // fall back to a cached value fingerprint (see window_is_wvp).
    shaderreg::Span spans[shaderreg::kMaxSpans];
    size_t n = shaderreg::active_transform_spans(spans);
    if (n == 0) {
        if (!shaderreg::active_shader_uncharted()) return false;
        const int off = fingerprint_offset(shaderreg::active_shader(), start_register, data, vec4_count);
        if (off < 0) return false;
        spans[0] = { start_register + static_cast<unsigned>(off), 4 };
        n = 1;
    }

    bool any = false;
    const shaderreg::ShaderFacts* facts = shaderreg::active_facts();
    const bool census = drawdiag::capturing();

    for (size_t s = 0; s < n; ++s) {
        if (!covers(start_register, vec4_count, spans[s].start)) continue;

        if (!any) scratch.assign(data, data + vec4_count * 4);
        any = true;

        float* target = scratch.data() + (spans[s].start - start_register) * 4;

        // Analyse the ORIGINAL block: its own projection, the object's
        // view-space origin, and how it compares to the world projection.
        const float* corr = g_correction;
        if (g_transposed) {
            Mat4 M;
            drawpolicy::stored_to_matrix(target, M);
            drawpolicy::ProjParams p;
            const bool persp = drawpolicy::recover_projection(M, p);
            drawpolicy::DrawSignature sig;
            sig.has_wvp   = true;
            sig.has_bones = facts && facts->has_bones;
            sig.proj      = drawpolicy::compare_projection(p, g_world_proj);
            if (persp && drawpolicy::view_origin(M, p, sig.view_origin)) {
                sig.view_dist = std::sqrt(sig.view_origin[0]*sig.view_origin[0] +
                                          sig.view_origin[1]*sig.view_origin[1] +
                                          sig.view_origin[2]*sig.view_origin[2]);
                int b = 5;
                for (int i = 0; i < 5; ++i) { if (sig.view_dist < kBuckets[i]) { b = i; break; } }
                ++g_vm_hist[b];
            }
            const drawpolicy::DrawClass cls = drawpolicy::classify(sig, g_vm_th);
            if (cls == drawpolicy::DrawClass::Viewmodel) {
                ++g_vm_hits;
                g_weapon_proj = p; g_weapon_proj_frame = g_frame_index;
                const float* vm = viewmodel_correction(p, true);
                if (vm) corr = vm;
            } else if (persp) {
                // Every recoverable draw is corrected around ITS projection.
                // Same P as the world -> identical to the global correction;
                // a depth slice -> the translations stay true to scale.
                if (cls == drawpolicy::DrawClass::OwnProjection) ++g_vm_ownp;
                const float* vm = viewmodel_correction(p, false);
                if (vm) corr = vm;
            }

            // Hand the analysis to the draw that follows (census only).
            PendingWvp& pend = t_pending;
            pend.shader = shaderreg::active_shader();
            pend.valid  = true;
            pend.p = p; pend.pc = sig.proj; pend.cls = cls;
            std::memcpy(pend.view_origin, sig.view_origin, sizeof(pend.view_origin));
            pend.view_dist = persp ? sig.view_dist : -1.0f;
            pend.have_bone0 = false;
            pend.bones_at_write = sig.has_bones;
            pend.require_bones  = g_vm_th.require_bones;
            if (census && g_have_vp_inv) {
                Mat4 res;
                multiply(M, g_vp_inv, res);
                pend.w33 = at(res, 3, 3);
                pend.w23 = at(res, 2, 3);
            }
        }

        Mat4 out;
        if (g_transposed) {
            // Stored block is M^T (HLSL column-major packing):
            //   (M * C)^T = C^T * M^T  ->  left-multiply by C^T
            Mat4 ct;
            transpose(reinterpret_cast<const float(&)[16]>(*corr), ct);
            multiply(ct, reinterpret_cast<const float(&)[16]>(*target), out);
        } else {
            // Stored block is M row-by-row: right-multiply as derived.
            multiply(reinterpret_cast<const float(&)[16]>(*target),
                     reinterpret_cast<const float(&)[16]>(*corr), out);
        }
        std::memcpy(target, out, sizeof(out));
        ++g_modified_this_frame;
    }

    // Bone palette sample for the census: the first bone (3 registers) of a
    // boneMatrices write tells whether bones are object-space (near-identity
    // 3x3, small translation) or carry the view.
    if (census && facts && facts->has_bones && facts->bone_count >= 3 &&
        start_register <= facts->bone_start && start_register + vec4_count >= facts->bone_start + 3) {
        PendingWvp& pend = t_pending;
        std::memcpy(pend.bone0, data + (facts->bone_start - start_register) * 4, sizeof(pend.bone0));
        pend.have_bone0 = true;
    }
    return any;
}

int on_draw(const void* return_address, bool indexed, unsigned prim_count)
{
    const shaderreg::ShaderFacts* facts = shaderreg::active_facts();
    const void* shader = shaderreg::active_shader();

    // Hidden viewmodel shader? Only when THIS draw's write was the weapon.
    if (g_hide_n && facts && t_pending.valid && t_pending.shader == shader &&
        t_pending.cls == drawpolicy::DrawClass::Viewmodel && hash_hidden(facts->hash)) {
        ++g_hidden_draws;
        return 1;
    }
    // HUD: alpha-blended, depth-off, non-perspective draws into the backbuffer
    // AFTER the frame's 3D geometry.
    //
    // The "after 3D" gate is not optional (learned the hard way 2026-08-20:
    // without it the main menu - every draw of which is alpha-blended and
    // depth-off - went into the HUD texture and the screen went black). A
    // frame only qualifies once it carries corrected WVP writes, and that
    // count resets every Present, so menus and loading screens never match.
    int verdict = 0;
    if (g_rt_is_bb && g_rs_z == 0 && g_rs_ab != 0 && g_hmd_active &&
        g_modified_this_frame > kHudMin3DWrites && g_world_proj.perspective) {
        const bool own_persp = t_pending.valid && t_pending.shader == shader &&
            t_pending.pc != drawpolicy::ProjClass::NotPerspective && t_pending.pc != drawpolicy::ProjClass::NoWvp;
        if (!own_persp) verdict = 2;
    }
    rpass::on_draw(facts ? facts->hash : 0ull, prim_count);
    if (!drawdiag::capturing()) return verdict;

    drawdiag::Record rec;
    rec.shader = shader;
    if (facts) {
        rec.shader_hash  = facts->hash;
        rec.shader_bytes = facts->bytes;
        rec.has_bones    = facts->has_bones;
    }
    const PendingWvp& pend = t_pending;
    if (pend.valid && pend.shader == shader) {
        rec.had_wvp = true;
        rec.proj = pend.pc; rec.p = pend.p; rec.cls = pend.cls;
        std::memcpy(rec.view_origin, pend.view_origin, sizeof(rec.view_origin));
        rec.view_dist = pend.view_dist;
        rec.world_dist = pend.view_dist;   // V is rigid: same number
        rec.residue_w33 = pend.w33; rec.residue_w23 = pend.w23;
        rec.have_bone0 = pend.have_bone0;
        std::memcpy(rec.bone0, pend.bone0, sizeof(rec.bone0));
        rec.bones_at_write = pend.bones_at_write;
        rec.require_bones  = pend.require_bones;
    }
    rec.z_enable = g_rs_z; rec.z_write = g_rs_zw; rec.alpha_blend = g_rs_ab;
    rec.rt_is_backbuffer = g_rt_is_bb; rec.rt_w = g_rt_w; rec.rt_h = g_rt_h;
    rec.indexed = indexed; rec.prims = prim_count; rec.ret = return_address;
    drawdiag::on_draw(rec);
    return verdict;
}

// Moving the view means moving the HMD reference. g_hmd_yaw/pitch for this
// frame were already computed from the old reference by on_present(), so both
// must change together or the turn lands a frame late and shimmers.
void adjust_view_reference(float d_yaw, float d_pitch)
{
    if (!std::isfinite(d_yaw) || !std::isfinite(d_pitch)) return;
    g_ref_yaw   += d_yaw;
    g_ref_pitch += d_pitch;
    g_hmd_yaw   -= d_yaw;
    g_hmd_pitch -= d_pitch;
}

void request_turn(float radians)
{
    // A deliberate turn of the virtual body. With a full-orientation reference
    // this must be its own term: folding it into the reference yaw would make
    // it indistinguishable from a head movement.
    if (!std::isfinite(radians) || std::fabs(radians) > aimpolicy::kPi) return;
    g_turn_yaw = aimpolicy::wrap_pi(g_turn_yaw + radians);
    adjust_view_reference(aimpolicy::turn_ref_delta(radians, g_yaw_sign), 0.0f);
}

void compensate_body_turn(float body_delta_radians)
{
    if (!std::isfinite(body_delta_radians)) return;
    adjust_view_reference(aimpolicy::compensation_ref_delta(body_delta_radians, g_yaw_sign), 0.0f);
}

unsigned g_cam_yaw_rejected = 0;
unsigned camera_yaw_rejections() { return g_cam_yaw_rejected; }
unsigned world_proj_rejections() { return g_world_proj_rejected; }

bool game_camera_angles(float& yaw, float& pitch)
{
    if (!g_have_eye) return false;
    const float f[3] = { g_cam_rows[6], g_cam_rows[7], g_cam_rows[8] };
    static float s_last_yaw = 0.0f;
    static bool  s_have_last = false;
    const float y = aimpolicy::yaw_from_forward(f, s_last_yaw);

    // g_cam_rows holds whichever camera wrote c189 LAST this frame, and shadow,
    // reflection and scope passes all write it. A frame where one of those went
    // last reports a heading that has nothing to do with the player, and the aim
    // loop would turn the body to chase it. A player cannot rotate 90 degrees
    // between two Presents, so a jump that large is someone else's camera.
    //
    // It MUST be able to give up. s_last_yaw only advances on the accept path,
    // so once a foreign camera takes the c189 slot its heading is compared
    // against a stale player yaw forever and can never be accepted again. That
    // is not hypothetical: it rejected 15472 of 15960 frames - 97 percent - and
    // since controller_dir_now() and turn_body() both bail when this returns
    // false, it killed the aim loop and threw away every snap turn the stick
    // asked for. Same escape hatch as the world projection gate above.
    constexpr float kMaxYawJump = 90.0f * kPi / 180.0f;
    static unsigned s_reject_run = 0;
    if (s_have_last && std::fabs(aimpolicy::body_delta(y, s_last_yaw)) > kMaxYawJump &&
        ++s_reject_run < kCamYawRunToAccept) {
        ++g_cam_yaw_rejected;
        return false;
    }

    s_reject_run = 0;
    yaw = y;
    s_last_yaw = y;
    s_have_last = true;
    pitch = aimpolicy::pitch_from_forward(f);
    return true;
}

bool body_frame(float& yaw, float& pitch, float pos[3])
{
    if (!g_hmd_active) return false;
    yaw = g_ref_yaw; pitch = g_ref_pitch;
    vrtrack::position(pos);          // anchor at the head's CURRENT position
    return true;
}

bool head_frame(float& yaw, float& pitch, float pos[3])
{
    if (!vrtrack::have_pose()) return false;
    hmd_yaw_pitch(yaw, pitch);
    vrtrack::position(pos);
    return true;
}

void note_render_state(unsigned state, unsigned value)
{
    switch (state) {
    case D3DRS_ZENABLE:          g_rs_z  = value; break;
    case D3DRS_ZWRITEENABLE:     g_rs_zw = value; break;
    case D3DRS_ALPHABLENDENABLE: g_rs_ab = value; break;
    default: break;
    }
}

void note_render_target(IDirect3DSurface9* surface)
{
    vrcomp::hud_on_game_set_render_target();
    IDirect3DSurface9* bb = vrcomp::backbuffer_surface();
    g_rt_is_bb = (surface != nullptr) && (surface == bb);
    if (g_rt_is_bb) {
        vrcomp::backbuffer_size(g_rt_w, g_rt_h);
    } else if (surface) {
        D3DSURFACE_DESC d = {};
        if (SUCCEEDED(surface->GetDesc(&d))) { g_rt_w = d.Width; g_rt_h = d.Height; }
        else { g_rt_w = g_rt_h = 0; }
    } else {
        g_rt_w = g_rt_h = 0;
    }
}

void recenter()
{
    g_angle_rad = 0.0f;
    if (g_hmd_active) {
        float yaw, pitch, pos[3];
        hmd_yaw_pitch(yaw, pitch);
        vrtrack::position(pos);
        g_ref_yaw = yaw; g_ref_pitch = pitch;
        std::memcpy(g_ref_pos, pos, sizeof(g_ref_pos));
        g_have_ref_pose = head_reference_now(g_ref_head_pose);
        g_turn_yaw = 0.0f;
        g_aim_view_offset = 0.0f;
        g_aim_ref_valid = false;          // the aim reference is relative to this
        VRLOG("[vr] reference recentered (full orientation%s + position)",
              g_have_ref_pose ? "" : " UNAVAILABLE - falling back to yaw/pitch");
    } else {
        VRLOG("[correct] angle reset; eye=(%.2f, %.2f, %.2f)", g_eye[0], g_eye[1], g_eye[2]);
    }
}

void on_present()
{
    static ULONGLONG last_tick = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last_tick ? (now - last_tick) / 1000.0f : 0.0f;
    last_tick = now;

    // Alternate-eye cadence, driven by CONTENT rather than Present count. The
    // game sometimes Presents without rendering (menu overlays, pump frames);
    // toggling the eye on those churns the phase - the fresh copy keeps
    // landing in one eye while the other eye's texture goes stale and gets
    // resubmitted as a seconds-old view (the head-still double vision the
    // stereo screenshots showed). Only a frame that actually contained
    // corrected 3D draws advances the eye.
    // g_modified_this_frame still holds the just-finished frame's count here -
    // the roll that zeroes it happens further down in this function.
    const bool had_3d = (g_modified_this_frame > 0);
    if (had_3d) {
        g_last_eye  = g_frame_eye;
        g_frame_eye ^= 1;
    }

    // Head tracking: refresh the pose and, while enabled, convert it to a
    // yaw/pitch delta against the reference captured when tracking engaged.
    vrtrack::update();

    // Auto-engage: the first valid pose turns VR mode on, like a native VR
    // title - no keypress required. An explicit F7-off stays off.
    if (!g_auto_engaged && !g_user_disabled && !g_correct_on && vrtrack::have_pose()) {
        g_correct_on = true;
        g_auto_engaged = true;
        VRLOG("[vr] auto-engaged - HMD pose available");
    }

    update_auto_fov();

    // Auto-IPD: track the headset's real setting until the user goes manual.
    if (!g_ipd_manual) {
        const float real = vrtrack::user_ipd_meters();
        if (real > 0.0f && std::fabs(real - g_ipd_world) > 0.0005f) {
            g_ipd_world = real;
            VRLOG("[stereo] IPD from headset: %.4f m", g_ipd_world);
        }
    }

    if (g_correct_on && vrtrack::have_pose()) {
        float yaw, pitch;
        hmd_yaw_pitch(yaw, pitch);
        float pos[3];
        vrtrack::position(pos);
        if (!g_hmd_active) {
            g_hmd_active = true;
            g_ref_yaw = yaw; g_ref_pitch = pitch;
            std::memcpy(g_ref_pos, pos, sizeof(g_ref_pos));
            g_have_ref_pose = head_reference_now(g_ref_head_pose);
            g_turn_yaw = 0.0f;
            g_aim_view_offset = 0.0f;
            g_aim_ref_valid = false;
            VRLOG("[vr] head tracking engaged (ref yaw=%.1f pitch=%.1f deg, pos %.2f/%.2f/%.2f)",
                  yaw * 180.0f / kPi, pitch * 180.0f / kPi, pos[0], pos[1], pos[2]);
        }
        g_hmd_yaw   = yaw   - g_ref_yaw;
        g_hmd_pitch = pitch - g_ref_pitch;
        for (int i = 0; i < 3; ++i) g_hmd_dpos[i] = pos[i] - g_ref_pos[i];
    } else if (g_hmd_active && !g_correct_on) {
        g_hmd_active = false;
        VRLOG("[vr] head tracking disengaged");
    }

    if (g_correct_on && !g_hmd_active) {
        g_angle_rad += dt * kDegPerSecond * kPi / 180.0f;
        if (g_angle_rad > 2.0f * kPi) g_angle_rad -= 2.0f * kPi;
    }
    // Decide which of the frame's cameras was the player's, before anything
    // reads g_vp or g_cam_rows. The aim loop (via vrinput::on_present) and
    // rebuild_correction() both do, so this has to come first.
    //
    // Aspect is passed as 0 (test skipped) deliberately: the collapsed frustum
    // that caused this is 0.1630/0.1223 = 1.333, the same 4:3 as the healthy
    // 0.5536/0.4152, so an aspect test cannot separate them. The field-of-view
    // range is what does the work. The parameter stays in the API for a pass
    // that does differ in shape.
    promote_player_camera(0.0f);

    // Controller input runs here: after the head deltas exist (it may move the
    // view reference) and before the correction is built from them.
    vrinput::on_present();

    ++g_frame_index;
    rebuild_correction();
    drawdiag::on_present();

    g_modified_last_frame = g_modified_this_frame;
    g_modified_this_frame = 0;

    static unsigned frames = 0;
    ++frames;
    g_vm_hits_last = g_vm_hits;  g_vm_hits = 0;
    g_hidden_draws_last = g_hidden_draws; g_hidden_draws = 0;
    g_vm_ownp_last = g_vm_ownp;  g_vm_ownp = 0;
    std::memcpy(g_vm_hist_last, g_vm_hist, sizeof(g_vm_hist));
    std::memset(g_vm_hist, 0, sizeof(g_vm_hist));

    if (g_correct_on && frames % 300 == 0) {
        VRLOG("[correct] %s: modified %u transform writes last frame (fingerprints: %u found, %u none)",
              g_transposed ? "transposed" : "row-registers", g_modified_last_frame,
              g_fp_found, g_fp_rejected);
        VRLOG("[viewmodel] mode=%d push=%.2fm ownproj=%d require_bones=%d: %u VIEWMODEL writes, %u depth-slice/own-P writes; world P tan=%.4f/%.4f near=%.3f",
              g_vm_mode, g_vm_push, g_ownproj_on ? 1 : 0, g_vm_th.require_bones ? 1 : 0, g_vm_hits_last, g_vm_ownp_last,
              g_world_proj.tan_half_h(), g_world_proj.tan_half_v(), g_world_proj.near_z());
        VRLOG("[vm-hist] view-space <0.1m:%u  <0.5m:%u  <1m:%u  <2m:%u  <5m:%u  >5m:%u",
              g_vm_hist_last[0], g_vm_hist_last[1], g_vm_hist_last[2],
              g_vm_hist_last[3], g_vm_hist_last[4], g_vm_hist_last[5]);
    }

    // Stereo phase forensics: eye parity vs corrected-write count per frame.
    // If eye parity and frame content ever fall out of phase - empty frames,
    // double Presents - each panel receives the OTHER eye's viewpoint half the
    // time, which doubles the world permanently and no calibration can fix it.
    // A short burst every ~5s makes that visible in the log.
    if (g_correct_on && g_hmd_active && frames % 300 < 8) {
        VRLOG("[phase] frame %u eye=%d modified=%u ipd=%.4f", frames, g_last_eye, g_modified_last_frame, g_ipd_world);
    }
    if (frames % 300 == 0) {
        for (size_t i = 0; i < g_shape_count; ++i) g_shapes[i].calls = 0;
    }

    if (key_pressed(VK_F7)) {
        g_correct_on = !g_correct_on;
        g_user_disabled = !g_correct_on;   // an explicit off suppresses auto-engage
        VRLOG("[correct] %s  (convention=%s, vp=%d eye=%d correction=%d)",
              g_correct_on ? "ON" : "off", g_transposed ? "transposed" : "row-registers",
              (int)g_have_vp, (int)g_have_eye, (int)g_have_correction);
    }
    // With head tracking live, F8/F6 calibrate axis signs; the transposed
    // convention is settled and only needs changing if geometry breaks again.
    if (key_pressed(VK_F8)) {
        if (g_hmd_active) {
            g_yaw_sign = -g_yaw_sign;
            VRLOG("[vr] yaw sign -> %+0.f", g_yaw_sign);
        } else {
            g_transposed = !g_transposed;
            VRLOG("[correct] convention -> %s", g_transposed ? "transposed" : "row-registers");
        }
    }
    if (key_pressed(VK_F6) && g_hmd_active) {
        g_pitch_sign = -g_pitch_sign;
        VRLOG("[vr] pitch sign -> %+0.f", g_pitch_sign);
    }
    if (key_pressed(VK_F5)) recenter();

    // FOV trim on top of the automatic match (PgUp wider / PgDn narrower).
    // PgDn trades field for sharpness and less edge pop-in.
    if (key_pressed(VK_PRIOR)) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_fov_auto = !g_fov_auto;
            VRLOG("[fov] auto headset match %s (expect engine-cull void beyond ~58x45 deg when on)", g_fov_auto ? "ON" : "off");
        } else {
            g_fov_manual = (std::min)(g_fov_manual + 0.05f, 3.5f);
            VRLOG("[fov] widen -> %.2fx (auto %s %.2f/%.2f)", g_fov_manual, g_fov_auto ? "on" : "off", g_fov_widen_x, g_fov_widen_y);
        }
    }
    if (key_pressed(VK_NEXT)) {
        g_fov_manual = (std::max)(g_fov_manual - 0.05f, 0.5f);
        VRLOG("[fov] widen -> %.2fx (auto %s %.2f/%.2f)", g_fov_manual, g_fov_auto ? "on" : "off", g_fov_widen_x, g_fov_widen_y);
    }

    if (key_pressed(VK_END)) {
        g_pos_enabled = !g_pos_enabled;
        VRLOG("[vr] 6DOF position %s", g_pos_enabled ? "ON" : "off (orientation only)");
    }

    // Viewmodel distance: '-' pulls the gun closer, '=' pushes it away.
    if (key_pressed(VK_OEM_MINUS)) {
        g_vm_push = (std::max)(g_vm_push - 0.05f, -1.5f);
        VRLOG("[viewmodel] offset forward -> %.2f m%s", g_vm_push, g_vm_push == 0.0f ? " (none)" : "");
    }
    if (key_pressed(VK_OEM_PLUS)) {
        g_vm_push = (std::min)(g_vm_push + 0.05f, 1.0f);
        VRLOG("[viewmodel] push -> %.2f m", g_vm_push);
    }
    // DELETE (or '[' on keyboards without it) cycles the viewmodel projection
    // mode. With Shift: toggles the classifier's bone requirement instead.
    if (key_pressed(VK_DELETE) || key_pressed(VK_OEM_4)) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_vm_th.require_bones = !g_vm_th.require_bones;
            VRLOG("[viewmodel] classifier require_bones=%d", g_vm_th.require_bones ? 1 : 0);
        } else {
            g_vm_mode = (g_vm_mode + 1) % 4;
            static const char* names[4] = { "own P", "own P (distortion fix)", "hybrid (world field, own depth)", "world P" };
            VRLOG("[viewmodel] mode %d: %s", g_vm_mode, names[g_vm_mode]);
        }
    }
    // ']' : draw census - 4 frames of per-draw signatures to bfbc2vr_draws_NN.txt.
    // (Was INSERT; not every keyboard has one.)
    if (key_pressed(VK_OEM_6)) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_ownproj_on = !g_ownproj_on;
            VRLOG("[viewmodel] per-draw own-projection correction %s (off = old global-only path, expect the depth-slice warp)",
                  g_ownproj_on ? "ON" : "OFF");
        } else {
            drawdiag::request_capture(4);
        }
    }

    // Stereo calibration.
    if (key_pressed(VK_F1)) {
        g_eye_swap = -g_eye_swap;
        VRLOG("[stereo] eyes %s", g_eye_swap > 0 ? "normal" : "SWAPPED");
    }
    if (key_pressed(VK_F2)) {
        g_ipd_manual = true;
        g_ipd_world *= 0.8f;
        // The multiplicative step approaches zero but never reaches it; floor
        // to a true zero so "identical corrections in both eyes" is testable.
        if (g_ipd_world < 0.0005f) g_ipd_world = 0.0f;
        if (g_ipd_world == 0.0f) VRLOG("[stereo] IPD -> 0 (disabled, manual)");
        else                     VRLOG("[stereo] IPD -> %.4f world units (manual)", g_ipd_world);
    }
    if (key_pressed(VK_F3)) {
        // Shift+F3: back to automatic headset IPD. Plain F3: manual step up.
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_ipd_manual = false;
            VRLOG("[stereo] IPD back to automatic (headset)");
        } else {
            g_ipd_manual = true;
            g_ipd_world = (g_ipd_world == 0.0f) ? 0.008f : g_ipd_world * 1.25f;
            VRLOG("[stereo] IPD -> %.4f world units (manual)", g_ipd_world);
        }
    }
}

int  last_rendered_eye() { return (g_eye_swap > 0) ? g_last_eye : (g_last_eye ^ 1); }
bool stereo_active()     { return g_hmd_active && g_correct_on; }
unsigned modified_last_frame() { return g_modified_last_frame; }

bool headset_tangents(float& tan_half_h, float& tan_half_v)
{
    auto* sys = vrtrack::system();
    if (!sys) return false;
    float need_h = 0.0f, need_v = 0.0f;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        sys->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
        need_h = (std::max)(need_h, (std::max)(std::fabs(l), std::fabs(r)));
        need_v = (std::max)(need_v, (std::max)(std::fabs(t), std::fabs(b)));
    }
    if (need_h < 1e-6f || need_v < 1e-6f) return false;
    tan_half_h = need_h; tan_half_v = need_v;
    return true;
}

// The controller's pointing direction in the body frame, in whatever axis the
// runtime's pose happens to use - which is exactly why it is only ever compared
// against a reference captured in the same axis.
// Game camera yaw when the aim reference was captured. The body's rotation
// since then is read from the camera rather than accumulated, because the
// camera is ground truth: it already includes snap turns, the aim loop's own
// turns, the player's mouse, and anything the game itself does to the heading.
// Accumulating instead meant every turn the loop did not personally emit went
// missing from the mapping.
float g_aim_ref_cam_yaw = 0.0f;

bool controller_dir_now(float out[3])
{
    if (!g_have_ref_pose) return false;
    float pose[12];
    if (!vrtrack::controller_pose(g_grip_hand, pose)) return false;

    float cam_yaw = 0.0f, cam_pitch = 0.0f;
    if (!game_camera_angles(cam_yaw, cam_pitch)) return false;
    // Negated because the parameter carries the controller direction the
    // opposite way to the body's own rotation.
    const float body = -aimpolicy::wrap_pi(cam_yaw - g_aim_ref_cam_yaw);

    return drawpolicy::controller_dir_in_body(pose, g_ref_head_pose, body, out);
}

void recalibrate_aim()
{
    // Snapshot the heading FIRST, so the direction is measured relative to the
    // same instant it is stored against.
    float cam_yaw = 0.0f, cam_pitch = 0.0f;
    if (!game_camera_angles(cam_yaw, cam_pitch)) { g_aim_ref_valid = false; return; }
    g_aim_ref_cam_yaw = cam_yaw;

    float dir[3];
    g_aim_ref_valid = controller_dir_now(dir);
    if (g_aim_ref_valid) {
        std::memcpy(g_aim_ref_dir, dir, sizeof(g_aim_ref_dir));
        VRLOG("[aim] reference captured - the gun now reads as aligned with the game's aim");
    }
}

bool aim_calibrated() { return g_aim_ref_valid; }

void set_grip_hand(int hand)
{
    if (hand < 0 || hand > 1 || hand == g_grip_hand) return;
    g_grip_hand = hand;
    // Both calibrations were captured against the other controller.
    g_grip_calibrated = false;
    g_aim_ref_valid = false;
    VRLOG("[grip] weapon hand is now %s", hand ? "right" : "left");
}

// Last yaw authority, kept so the status block can distinguish "the loop is
// idle" from "the loop is declining because you are pointing at the sky".
float g_aim_authority = 0.0f;
float aim_authority() { return g_aim_authority; }

bool aim_error(float& yaw_error, float& pitch_error, int& reason)
{
    yaw_error = 0.0f; pitch_error = 0.0f; reason = 0;
    if (!g_hmd_active) { reason = 1; return false; }
    if (!g_have_ref_pose) { reason = 2; return false; }

    float dir[3];
    if (!controller_dir_now(dir)) { reason = 3; return false; }

    if (!g_aim_ref_valid) {
        // The first usable sample DEFINES "aligned", instead of assuming the
        // pose's own forward axis is the barrel.
        recalibrate_aim();
        VRLOG("[aim] reference captured on first sample");
        return false;                      // no correction on the baseline frame
    }
    float authority = 0.0f;
    if (!drawpolicy::aim_deviation(dir, g_aim_ref_dir, yaw_error, pitch_error, authority)) {
        g_aim_authority = 0.0f;
        reason = 4;
        return false;
    }
    g_aim_authority = authority;
    // Scale to the horizontal displacement the turn would actually produce.
    // Unscaled, aiming vertically spun the body: up there the yaw reading is
    // large and jittery while the gun has barely moved horizontally at all.
    yaw_error *= authority;
    return true;
}

void compensate_aim_turn(float body_yaw_delta)
{
    if (!std::isfinite(body_yaw_delta)) return;
    // view_yaw = body_yaw + head_yaw + turn_offset, so absorbing the body's
    // rotation into the offset holds the presented view still. A deliberate
    // turn deliberately does NOT come through here.
    // Bounded: this is a standing divergence between what the player sees and
    // where their body faces, and unbounded it became a permanent one.
    g_aim_view_offset = aimpolicy::wrap_pi(g_aim_view_offset - body_yaw_delta);
    g_aim_view_offset = (std::min)((std::max)(g_aim_view_offset, -g_max_turn_offset), g_max_turn_offset);
}

float aim_view_offset() { return g_aim_view_offset; }

void clear_aim_view_offset() { g_aim_view_offset = 0.0f; }

void set_turn_offset_mode(bool firing)
{
    g_max_turn_offset = firing ? kMaxTurnOffsetFiring : kMaxTurnOffsetContinuous;
}

void bleed_turn_offset()
{
    // Called while the aim loop is idle: let the view and body converge again,
    // slowly enough to be imperceptible.
    if (g_aim_view_offset > kTurnBleedPerFrame)       g_aim_view_offset -= kTurnBleedPerFrame;
    else if (g_aim_view_offset < -kTurnBleedPerFrame) g_aim_view_offset += kTurnBleedPerFrame;
    else                                              g_aim_view_offset = 0.0f;
}

bool weapon_tangents(float& tan_half_h, float& tan_half_v)
{
    if (!g_weapon_proj.perspective || g_frame_index - g_weapon_proj_frame > 120) return false;
    tan_half_h = g_weapon_proj.tan_half_h();
    tan_half_v = g_weapon_proj.tan_half_v();
    return true;
}

bool world_tangents(float& tan_half_h, float& tan_half_v)
{
    if (!g_world_proj.perspective) return false;
    tan_half_h = g_world_proj.tan_half_h();
    tan_half_v = g_world_proj.tan_half_v();
    return true;
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    char a1[64] = {};
    if (args) sscanf_s(args, "%63s", a1, static_cast<unsigned>(sizeof(a1)));
    const bool has1 = a1[0] != 0;
    const bool on = has1 && (!_stricmp(a1, "on") || !strcmp(a1, "1"));

    if (!strcmp(cmd, "widen")) {
        if (has1) g_fov_manual = (std::min)((std::max)(static_cast<float>(atof(a1)), 0.5f), 3.5f);
        _snprintf_s(reply, n, _TRUNCATE, "widen manual=%.2f auto=%s (%.2f/%.2f)", g_fov_manual, g_fov_auto ? "on" : "off", g_fov_widen_x, g_fov_widen_y);
        return true;
    }
    if (!strcmp(cmd, "auto")) {
        if (has1) g_fov_auto = on;
        _snprintf_s(reply, n, _TRUNCATE, "auto FOV match %s", g_fov_auto ? "on" : "off");
        return true;
    }
    if (!strcmp(cmd, "mode")) {
        if (has1) g_vm_mode = ((atoi(a1) % 4) + 4) % 4;
        _snprintf_s(reply, n, _TRUNCATE, "viewmodel mode %d", g_vm_mode);
        return true;
    }
    if (!strcmp(cmd, "push")) {
        if (has1) g_vm_push = (std::min)((std::max)(static_cast<float>(atof(a1)), -1.5f), 2.0f);
        _snprintf_s(reply, n, _TRUNCATE, "weapon offset forward %.2f m (negative pulls it toward you)", g_vm_push);
        return true;
    }
    if (!strcmp(cmd, "gripoffset") || !strcmp(cmd, "weaponoffset")) {
        char b1[32] = {}, b2[32] = {}, b3[32] = {};
        if (args) sscanf_s(args, "%31s %31s %31s", b1, static_cast<unsigned>(sizeof(b1)),
                           b2, static_cast<unsigned>(sizeof(b2)), b3, static_cast<unsigned>(sizeof(b3)));
        if (b1[0] && b2[0] && b3[0]) {
            const float v[3] = { static_cast<float>(atof(b1)), static_cast<float>(atof(b2)), static_cast<float>(atof(b3)) };
            for (int i = 0; i < 3; ++i) g_vm_off[i] = (std::min)((std::max)(v[i], -1.5f), 2.0f);
        }
        _snprintf_s(reply, n, _TRUNCATE, "weapon offset (right %.2f, up %.2f, forward %.2f) m",
                    g_vm_off[0], g_vm_off[1], g_vm_off[2]);
        return true;
    }
    if (!strcmp(cmd, "ownproj")) {
        if (has1) g_ownproj_on = on;
        _snprintf_s(reply, n, _TRUNCATE, "per-draw own-projection correction %s", g_ownproj_on ? "on" : "off");
        return true;
    }
    if (!strcmp(cmd, "bones")) {
        if (has1) g_vm_th.require_bones = on;
        _snprintf_s(reply, n, _TRUNCATE, "classifier require_bones=%d", g_vm_th.require_bones ? 1 : 0);
        return true;
    }
    if (!strcmp(cmd, "pos")) {
        if (has1) g_pos_enabled = on;
        _snprintf_s(reply, n, _TRUNCATE, "6DOF position %s", g_pos_enabled ? "on" : "off");
        return true;
    }
    if (!strcmp(cmd, "ipd")) {
        if (has1) { g_ipd_manual = true; g_ipd_world = static_cast<float>(atof(a1)); }
        _snprintf_s(reply, n, _TRUNCATE, "ipd %.4f (%s)", g_ipd_world, g_ipd_manual ? "manual" : "auto");
        return true;
    }
    if (!strcmp(cmd, "recenter")) { recenter(); g_grip_calibrated = false; _snprintf_s(reply, n, _TRUNCATE, "recentered"); return true; }
    if (!strcmp(cmd, "grip")) {
        if (has1) {
            if (!_stricmp(a1, "on"))  { g_grip_on = true;  g_grip_calibrated = false; }
            else if (!_stricmp(a1, "off")) { g_grip_on = false; g_grip_calibrated = false; g_grip_active = false; }
            else if (!_stricmp(a1, "left"))  { g_grip_hand = 0; g_grip_calibrated = false; }
            else if (!_stricmp(a1, "right")) { g_grip_hand = 1; g_grip_calibrated = false; }
            else if (!_stricmp(a1, "recal")) { g_grip_calibrated = false; g_grip_have_prev = false; }
            else if (!_stricmp(a1, "scale")) { /* handled below */ }
            else { g_grip_units_per_metre = (std::min)((std::max)(static_cast<float>(atof(a1)), 0.05f), 20.0f); }
        }
        _snprintf_s(reply, n, _TRUNCATE, "grip %s hand=%s scale=%.2f calibrated=%d active=%d resets=%u ctl(l=%d r=%d)",
                    g_grip_on ? "on" : "off", g_grip_hand ? "right" : "left", g_grip_units_per_metre,
                    g_grip_calibrated ? 1 : 0, g_grip_active ? 1 : 0, g_grip_resets,
                    vrtrack::controller_connected(0) ? 1 : 0, vrtrack::controller_connected(1) ? 1 : 0);
        return true;
    }
    if (!strcmp(cmd, "gripsmooth")) {
        if (has1) g_grip_smooth = (std::min)((std::max)(static_cast<float>(atof(a1)), 0.0f), 1.0f);
        _snprintf_s(reply, n, _TRUNCATE, "grip smoothing %.2f (0 = raw)", g_grip_smooth);
        return true;
    }
    if (!strcmp(cmd, "hide")) {
        if (!has1 || g_hide_n >= kMaxHidden) { _snprintf_s(reply, n, _TRUNCATE, "usage: hide <hash-prefix-hex> (%zu/%zu used)", g_hide_n, kMaxHidden); return true; }
        const size_t digits = strlen(a1) > 16 ? 16 : strlen(a1);
        unsigned long long v = 0;
        for (size_t i = 0; i < digits; ++i) {
            const char c = a1[i];
            const unsigned d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            v = (v << 4) | d;
        }
        v <<= (64 - digits * 4);
        g_hide[g_hide_n] = v; g_hide_bits[g_hide_n] = static_cast<unsigned>(digits); ++g_hide_n;
        _snprintf_s(reply, n, _TRUNCATE, "hiding viewmodel shaders with hash prefix %s (%zu hidden)", a1, g_hide_n);
        return true;
    }
    if (!strcmp(cmd, "unhide")) { g_hide_n = 0; _snprintf_s(reply, n, _TRUNCATE, "all shaders visible"); return true; }
    if (!strcmp(cmd, "hidden")) {
        _snprintf_s(reply, n, _TRUNCATE, "%zu hidden prefixes; %u draws hidden last frame", g_hide_n, g_hidden_draws_last);
        return true;
    }
    if (!strcmp(cmd, "headroll")) {
        if (has1) g_full_orientation = (!_stricmp(a1, "on") || !strcmp(a1, "1"));
        _snprintf_s(reply, n, _TRUNCATE, "full head orientation %s (reference pose %s)",
                    g_full_orientation ? "on" : "off (yaw+pitch fallback)",
                    g_have_ref_pose ? "captured" : "MISSING");
        return true;
    }
    if (!strcmp(cmd, "pitchsign") || !strcmp(cmd, "yawsign")) {
        float& sgn = (cmd[0] == 'p') ? g_pitch_sign : g_yaw_sign;
        if (has1) sgn = (atof(a1) < 0) ? -1.0f : 1.0f;
        _snprintf_s(reply, n, _TRUNCATE, "%s = %+.0f", cmd, sgn);
        return true;
    }
    if (!strcmp(cmd, "correct")) {
        if (has1) { g_correct_on = on; g_user_disabled = !on; }
        _snprintf_s(reply, n, _TRUNCATE, "correction %s", g_correct_on ? "on" : "off");
        return true;
    }
    return false;
}

void status(FILE* f)
{
    float yaw = 0.0f, pitch = 0.0f;
    if (g_hmd_active) { yaw = g_hmd_yaw; pitch = g_hmd_pitch; }
    fprintf(f, "correct=%d hmd=%d eye=%d ipd=%.4f%s pos6dof=%d frame=%u modified/frame=%u\n",
            g_correct_on ? 1 : 0, g_hmd_active ? 1 : 0, g_frame_eye, g_ipd_world, g_ipd_manual ? "(manual)" : "",
            g_pos_enabled ? 1 : 0, g_frame_index, g_modified_last_frame);
    fprintf(f, "head: full-orientation=%d ref-pose=%d turn=%.1f deg aim-view-offset=%.1f deg cam-yaw-rejected=%u\n",
            g_full_orientation ? 1 : 0, g_have_ref_pose ? 1 : 0,
            g_turn_yaw * 180.0f / kPi, g_aim_view_offset * 180.0f / kPi, g_cam_yaw_rejected);
    fprintf(f, "hmd yaw=%.1f pitch=%.1f deg  dpos=(%.3f %.3f %.3f)\n",
            yaw * 180.0f / kPi, pitch * 180.0f / kPi, g_hmd_dpos[0], g_hmd_dpos[1], g_hmd_dpos[2]);
    fprintf(f, "fov: auto=%d manual=%.2f widen=%.2f/%.2f  world tan=%.4f/%.4f near=%.3f far=%.1f (deg %.1f x %.1f)\n",
            g_fov_auto ? 1 : 0, g_fov_manual, g_fov_widen_x, g_fov_widen_y,
            g_world_proj.tan_half_h(), g_world_proj.tan_half_v(), g_world_proj.near_z(), g_world_proj.far_z(),
            2.0f * std::atan(g_world_proj.tan_half_h()) * 180.0f / kPi, 2.0f * std::atan(g_world_proj.tan_half_v()) * 180.0f / kPi);
    fprintf(f, "     world-proj-rejected=%u cam-candidates=%u promote-failed=%u\n",
            g_world_proj_rejected, g_cam_candidates_last, g_cam_promote_failed);
    fprintf(f, "weapon P: tan=%.4f/%.4f (deg %.1f x %.1f) near=%.3f\n",
            g_weapon_proj.tan_half_h(), g_weapon_proj.tan_half_v(),
            2.0f * std::atan(g_weapon_proj.tan_half_h()) * 180.0f / kPi, 2.0f * std::atan(g_weapon_proj.tan_half_v()) * 180.0f / kPi,
            g_weapon_proj.near_z());
    fprintf(f, "viewmodel: mode=%d push=%.2f ownproj=%d require_bones=%d  VIEWMODEL writes=%u own-P writes=%u hidden=%u/%zu\n",
            g_vm_mode, g_vm_push, g_ownproj_on ? 1 : 0, g_vm_th.require_bones ? 1 : 0, g_vm_hits_last, g_vm_ownp_last, g_hidden_draws_last, g_hide_n);
    fprintf(f, "vm-hist view-space <0.1:%u <0.5:%u <1:%u <2:%u <5:%u >5:%u\n",
            g_vm_hist_last[0], g_vm_hist_last[1], g_vm_hist_last[2], g_vm_hist_last[3], g_vm_hist_last[4], g_vm_hist_last[5]);
    fprintf(f, "weapon offset: right=%.2f up=%.2f forward=%.2f m\n", g_vm_off[0], g_vm_off[1], g_vm_off[2]);
    fprintf(f, "grip: %s hand=%s calibrated=%d active=%d scale=%.2f smooth=%.2f resets=%u controllers(l=%d r=%d) delta=(%.3f %.3f %.3f)\n",
            g_grip_on ? "on" : "off", g_grip_hand ? "right" : "left", g_grip_calibrated ? 1 : 0,
            g_grip_active ? 1 : 0, g_grip_units_per_metre, g_grip_smooth, g_grip_resets,
            vrtrack::controller_connected(0) ? 1 : 0, vrtrack::controller_connected(1) ? 1 : 0,
            g_grip_delta[12], g_grip_delta[13], g_grip_delta[14]);
    fprintf(f, "camera eye=(%.2f %.2f %.2f) fwd=(%.3f %.3f %.3f)\n", g_eye[0], g_eye[1], g_eye[2], g_cam_rows[6], g_cam_rows[7], g_cam_rows[8]);
}

bool game_proj_tangents(float& tan_half_h, float& tan_half_v)
{
    float th = 0.0f, tv = 0.0f;
    if (!base_tangents(th, tv)) return false;
    // Report the field ACTUALLY submitted, not the game's native projection,
    // so the compositor's bounds track the widened image.
    tan_half_h = th * g_fov_widen_x * g_fov_manual;
    tan_half_v = tv * g_fov_widen_y * g_fov_manual;
    return true;
}

} // namespace camover
