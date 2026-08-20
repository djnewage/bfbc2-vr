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

enum class DrawClass : unsigned char { Unclassified = 0, Viewmodel };

// Fail-closed: Viewmodel only when every required leg holds.
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

} // namespace drawpolicy
