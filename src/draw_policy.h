// Pure draw-classification and viewmodel-correction math. No Windows, no D3D,
// no logging - so it can be unit-tested on a desk and reasoned about in
// isolation. The hook code (camera_override.cpp) feeds it matrices and acts on
// what it returns. Modelled on BFVR's stereo/ policy modules: the routing
// decision is a function of evidence, tested deterministically, and the hook
// file stays thin.
//
// WHY THIS EXISTS (2026-08-20): distance-to-camera was the wrong way to find
// the first-person weapon - it matched ~700 draws a frame. BFVR hit the same
// wall and classified by call site + shader identity + the viewmodel's OWN
// projection. In BFBC2 the last one is the interesting lever: if the 1P
// weapon is drawn with its own FOV/near plane, then every WVP it submits is
// W * V * P_vm with P_vm != P, and a correction built as VP^-1 * R * VP
// (which assumes P) mixes R with perspective terms - exactly the "stretched
// black arc" the weapon showed in Phase 3. The fix and the discriminator are
// the same fact, so the first job is to RECOVER P from the stored matrix.
//
// CONVENTIONS
//   Everything here is row-vector, row-major (see mat4.h). A matrix the game
//   stores in a constant span is the transpose; callers transpose first
//   (stored_to_matrix). With M = A * P, A = World * View affine (last column
//   0,0,0,1) and P the D3D left-handed projection
//
//        [ a 0 0 0 ]
//   P =  [ 0 b 0 0 ]      clip.w = z_view,  clip.z = q*z + t
//        [ 0 0 q 1 ]      near = -t/q,  far = q*near/(q-1)
//        [ 0 0 t 0 ]
//
//   the columns of M are   M[.][0] = a*A[.][0]   M[.][1] = b*A[.][1]
//                          M[.][2] = q*A[.][2] + t*A[.][3]   M[.][3] = A[.][2]
//   so for rows 0..2 (A[i][3]=0):  q = M[i][2]/M[i][3]
//      row 3:                      t = M[3][2] - q*M[3][3]
//   and if A's 3x3 is a uniformly scaled rotation (rigid object, scale s),
//   |M[0..2][3]| = s, |M[0..2][0]| = a*s, |M[0..2][1]| = b*s.
//   The same formulas on the global VP (A = V, s = 1) give the world P, and
//   reproduce camera_override's base_tangents (tan = 1/a, 1/b).
#pragma once

#include "mat4.h"

namespace drawpolicy {

struct ProjParams {
    float a = 0.0f, b = 0.0f, q = 0.0f, t = 0.0f;
    bool  perspective = false;

    float near_z() const { return (q != 0.0f) ? -t / q : 0.0f; }
    float far_z()  const { return (q != 1.0f) ? q * near_z() / (q - 1.0f) : 0.0f; }
    float tan_half_h() const { return (a != 0.0f) ? 1.0f / a : 0.0f; }
    float tan_half_v() const { return (b != 0.0f) ? 1.0f / b : 0.0f; }
};

// Stored span (register-major, HLSL column packing) -> row-vector M.
void stored_to_matrix(const float stored[16], m4::Mat4 M);
void matrix_to_stored(const m4::Mat4 M, float stored[16]);

// Recover projection parameters from M = A * P. Returns false (and
// perspective=false) for orthographic / pre-transformed / degenerate matrices,
// or when the three q estimates disagree (not of the assumed form).
bool recover_projection(const m4::Mat4 M, ProjParams& out);

void make_projection(const ProjParams& p, m4::Mat4 out);
// P^-1 in closed form: [[1/a,0,0,0],[0,1/b,0,0],[0,0,0,1/t],[0,0,1,-q/t]].
bool invert_projection(const ProjParams& p, m4::Mat4 out);

// Object origin in view space (x right, y up, z forward, metres if the game
// is metric): row 3 of A = M * P^-1.
bool view_origin(const m4::Mat4 M, const ProjParams& p, float out[3]);

enum class ProjClass : unsigned char {
    NoWvp = 0,        // draw carried no clip-space transform we know of
    NotPerspective,   // ortho / screen-space / degenerate
    Same,             // matches the global world projection
    FovDiffers,       // a/b differ from world (own field of view)
    DepthDiffers,     // near plane differs from world
    Both,
};
const char* proj_class_name(ProjClass c);

struct Tolerances {
    float fov_ratio_eps  = 0.02f;   // |a'/a - 1| above this = own FOV
    float near_ratio_eps = 0.05f;   // |n'/n - 1| above this = own near plane
};
ProjClass compare_projection(const ProjParams& draw, const ProjParams& world,
                             const Tolerances& tol = Tolerances{});

// The projection a draw should be corrected AROUND. a/b are snapped to the
// world's when within tolerance (a non-rigid World would otherwise leak into
// the recovered field and warp that one draw); q/t are always the draw's own,
// because the engine renders the scene in depth slices (near 0.1 / 7.48 /
// 21.34 m seen) and the near plane is exactly what must not be assumed.
ProjParams correction_projection(const ProjParams& draw, const ProjParams& world,
                                 const Tolerances& tol = Tolerances{});

// Everything the classifier is allowed to look at. Filled by the hook from
// the stored matrix, the active shader's CTAB, and the global VP.
struct DrawSignature {
    bool      has_wvp     = false;
    bool      has_bones   = false;   // shader declares boneMatrices/boneVectors
    ProjClass proj        = ProjClass::NoWvp;
    float     view_origin[3] = { 0.0f, 0.0f, 0.0f };
    float     view_dist   = 1e9f;    // |view_origin|
};

struct Thresholds {
    float max_view_dist = 0.6f;      // object origin within this of the eye
    float max_view_z    = 1.0f;      // and not behind / not far ahead
    bool  require_bones = true;      // fail closed unless the dump says otherwise
    bool  accept_on_projection = true; // own P alone is sufficient evidence
};

enum class DrawClass : unsigned char {
    Unclassified = 0,   // global correction
    OwnProjection,      // rendered with its own P (far-scene pass with a pushed
                        // near plane, skybox...): corrected around ITS P, no offset
    Viewmodel,          // the first-person weapon/arms: own P AND offset (push/grip)
};
const char* draw_class_name(DrawClass c);

// Fail-closed. Viewmodel = bones (if required) AND (own FIELD OF VIEW, or
// origin at the eye). A different near plane alone is not a weapon - the
// engine draws distant passes that way - but such draws still need the
// correction built around their own P, hence OwnProjection.
DrawClass classify(const DrawSignature& s, const Thresholds& th);

// Which projection the corrected viewmodel should be rendered with.
enum class ProjSelect : unsigned char {
    Viewmodel = 0,   // keep its own P: distortion fix only
    Hybrid,          // x/y field from the world P, depth range from its own
    World,           // the world P outright
};
ProjParams select_projection(ProjSelect sel, const ProjParams& vm, const ProjParams& world);

// C_vm = P_vm^-1 * delta_view * c_view * P_sel
//
//   M' = M * C_vm = W * V * delta_view * c_view * P_sel
//
// where c_view = V^-1 * r * V is the frame's head/6DOF/eye/FOV correction
// expressed in view space, and delta_view is the weapon's own offset (an
// arm's-length push today; a controller grip delta later) - placed BEFORE
// c_view so it is applied in the body camera's frame and both eyes share one
// adjusted weapon pose (BFVR: World * sourceView * gripDelta * residualEye * P).
bool build_viewmodel_correction(const ProjParams& p_vm, const m4::Mat4 delta_view,
                                const m4::Mat4 c_view, const ProjParams& p_sel,
                                m4::Mat4 out);

// ---------------------------------------------------------------------------
// Controller-held weapon (Phase 7a: presentation only).
//
// An OpenVR device pose is a 3x4 column-vector matrix in a right-handed space
// (+X right, +Y up, -Z forward, metres). Ours is row-vector and the game's view
// space is left-handed with +Z forward. `openvr_pose_to_view` converts one to
// the other: transpose the rotation into row-vector form, then conjugate by
// C = diag(1,1,-1) so the handedness flips without moving anything.
void openvr_pose_to_view(const float pose3x4[12], m4::Mat4 out);

// The weapon's offset for this frame: how the grip has moved RELATIVE TO THE
// HEAD since calibration.
//
//   grip_in_head    = grip * inverse(head)
//   delta           = inverse(grip_in_head_at_calibration) * grip_in_head
//
// Head-relative is what makes shared head-and-hand motion (walking, leaning)
// leave the weapon alone. The delta is a FULL rigid transform, never
// rotation-only: rotating a controller in place produces a compensating
// translation that keeps the grip pivot still, and dropping it would swing the
// weapon about the eye instead (BFVR's dead-end #3).
//
// `units_per_metre` scales the translation into world units (1.0 for a metric
// engine such as Frostbite). Returns false if either pose is degenerate.
bool make_grip_delta(const m4::Mat4 head_view, const m4::Mat4 grip_view,
                     const m4::Mat4 ref_head_view, const m4::Mat4 ref_grip_view,
                     float units_per_metre, m4::Mat4 out);

// ---------------------------------------------------------------------------
// Head orientation (2026-08-27).
//
// The correction used to be built from two Euler scalars - yaw about world up
// and pitch about the game camera's right axis. That has two defects, and the
// horizon tilting in the headset was both of them at once:
//
//   1. Head ROLL was never extracted at all, so tilting the head did not
//      counter-rotate the image and the virtual horizon rolled with the player.
//   2. Pitch was applied about the BODY's right axis and yaw afterwards, so
//      once pitched, turning rotated about an axis that was no longer the
//      head's right - the composite was not a roll-free orientation, and the
//      horizon tilted while merely looking around.
//
// Both disappear by carrying the head's full relative orientation, which is
// what BFVR does (MakeD3D8ViewFromOpenXRPose is quaternion -> 3x3, never
// Euler). Roll is stripped only where a consumer genuinely wants gravity
// alignment - the HUD panel - not here.
//
// Derivation, row-vector and left-handed throughout:
//   B    = R_now * R_ref^-1        head rotation, in reference-frame coords
//   T    = yaw(turn)               deliberate turn of the virtual body
//   A    = B * T                   camera rotation relative to the body
//   W    = V * A * M_cw            the same rotation in world space
//   rot  = W^-1 = V * T^-1 * B^-1 * M_cw
// where M_cw is the game's camera-to-world basis (rows right/up/forward) and
// V = M_cw^-1 = M_cw^T (it is orthonormal to six decimals - phase2-results.md).

// Rotation part only of an OpenVR 3x4, in our row-vector left-handed
// convention. Same transpose-and-conjugate as openvr_pose_to_view.
void openvr_rotation_to_view(const float pose3x4[12], m4::Mat4 out);

// The camera-to-world basis as a matrix (rows right/up/forward, no translation).
void camera_basis(const float cam_rows[9], m4::Mat4 out);

// The world-space rotation the correction needs. False if either pose is
// degenerate.
bool head_world_rotation(const float head_now[12], const float head_ref[12],
                         float turn_yaw, const float cam_rows[9], m4::Mat4 out);

// The head's right axis in world space - the axis the stereo eye baseline must
// ride, so the eyes tilt with a tilted head instead of staying level.
bool head_right_in_world(const float head_now[12], const float head_ref[12],
                         float turn_yaw, const float cam_rows[9], float out[3]);

// A positional head delta (raw OpenVR tracking axes, metres) expressed in the
// game's world space. Rotating it into reference-frame coordinates first is
// what makes leaning correct at any recenter heading; mapping the raw tracking
// axes straight through the camera basis - as the first version did - is only
// right when the reference happened to face the tracking origin's forward.
bool lean_in_world(const float dpos_openvr[3], const float head_ref[12],
                   float turn_yaw, const float cam_rows[9], float out[3]);

// Where a controller points, expressed in the GAME CAMERA's frame.
//
// This is what makes the aim error simple. The game's own aim is, by
// definition, (0,0,1) in camera coordinates - so the error between the barrel
// and the game's aim is just the controller's direction re-expressed in that
// frame, and the body's world heading cancels out entirely. No world
// handedness enters, which is the whole class of sign bug avoided.
//
// `turn_yaw` is the deliberate turn offset held by the presentation.
bool controller_dir_in_body(const float ctrl_pose[12], const float head_ref[12],
                            float turn_yaw, float out[3]);

// A discontinuity this large BETWEEN CONSECUTIVE ACCEPTED SAMPLES means
// tracking glitched or the weapon changed; the caller re-calibrates rather
// than teleporting the weapon (BFVR uses 0.50 m).
//
// Note it is the step, not the total offset from calibration: an earlier
// version gated on the total and made the weapon snap whenever the hand
// travelled half a metre from where it started, which is ordinary movement.
bool grip_delta_step_is_sane(const m4::Mat4 previous, const m4::Mat4 current,
                             float max_step);

} // namespace drawpolicy
