#include "draw_policy.h"

#include <cmath>

namespace drawpolicy {

using m4::Mat4;
using m4::at;

void stored_to_matrix(const float stored[16], Mat4 M)
{
    Mat4 s;
    m4::copy(reinterpret_cast<const float(&)[16]>(*stored), s);
    m4::transpose(s, M);
}

void matrix_to_stored(const Mat4 M, float stored[16])
{
    Mat4 t;
    m4::transpose(M, t);
    m4::copy(t, reinterpret_cast<float(&)[16]>(*stored));
}

namespace {

float norm3(float x, float y, float z) { return std::sqrt(x*x + y*y + z*z); }

} // namespace

bool recover_projection(const Mat4 M, ProjParams& out)
{
    out = ProjParams{};

    // Column 3 of the upper 3x3 is A[.][2] scaled: its norm is the object's
    // uniform scale. If it vanishes, clip.w does not depend on position -
    // orthographic or pre-transformed, not a perspective WVP.
    const float s = norm3(at(M,0,3), at(M,1,3), at(M,2,3));
    if (s < 1e-6f) return false;

    // q from each of the three usable rows; they must agree or M is not of
    // the form A * P (bone rows, packed params, lighting data...).
    float q_est[3]; int n = 0;
    for (int i = 0; i < 3; ++i) {
        const float w = at(M, i, 3);
        if (std::fabs(w) > 1e-4f * s) q_est[n++] = at(M, i, 2) / w;
    }
    if (n < 2) return false;
    float q = 0.0f;
    for (int i = 0; i < n; ++i) q += q_est[i];
    q /= static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        if (std::fabs(q_est[i] - q) > 1e-3f * (std::fabs(q) + 1e-3f)) return false;
    }
    if (std::fabs(q) < 1e-6f || std::fabs(q - 1.0f) < 1e-6f) return false;

    const float t = at(M, 3, 2) - q * at(M, 3, 3);
    const float a = norm3(at(M,0,0), at(M,1,0), at(M,2,0)) / s;
    const float b = norm3(at(M,0,1), at(M,1,1), at(M,2,1)) / s;
    if (a < 1e-6f || b < 1e-6f) return false;

    out.a = a; out.b = b; out.q = q; out.t = t;
    out.perspective = true;
    return true;
}

void make_projection(const ProjParams& p, Mat4 out)
{
    m4::identity(out);
    at(out,0,0) = p.a;
    at(out,1,1) = p.b;
    at(out,2,2) = p.q; at(out,2,3) = 1.0f;
    at(out,3,2) = p.t; at(out,3,3) = 0.0f;
}

bool invert_projection(const ProjParams& p, Mat4 out)
{
    if (!p.perspective || std::fabs(p.a) < 1e-9f || std::fabs(p.b) < 1e-9f || std::fabs(p.t) < 1e-9f) return false;
    m4::identity(out);
    at(out,0,0) = 1.0f / p.a;
    at(out,1,1) = 1.0f / p.b;
    at(out,2,2) = 0.0f;        at(out,2,3) = 1.0f / p.t;
    at(out,3,2) = 1.0f;        at(out,3,3) = -p.q / p.t;
    return true;
}

bool view_origin(const Mat4 M, const ProjParams& p, float out[3])
{
    Mat4 pinv, A;
    if (!invert_projection(p, pinv)) return false;
    m4::multiply(M, pinv, A);
    const float w = at(A, 3, 3);
    if (std::fabs(w) < 1e-6f) return false;
    out[0] = at(A,3,0) / w; out[1] = at(A,3,1) / w; out[2] = at(A,3,2) / w;
    return true;
}

const char* proj_class_name(ProjClass c)
{
    switch (c) {
    case ProjClass::NoWvp:          return "no-wvp";
    case ProjClass::NotPerspective: return "ortho";
    case ProjClass::Same:           return "same";
    case ProjClass::FovDiffers:     return "fov-differs";
    case ProjClass::DepthDiffers:   return "depth-differs";
    case ProjClass::Both:           return "fov+depth-differ";
    }
    return "?";
}

ProjClass compare_projection(const ProjParams& draw, const ProjParams& world, const Tolerances& tol)
{
    if (!draw.perspective) return ProjClass::NotPerspective;
    if (!world.perspective) return ProjClass::Same;   // nothing to compare against yet
    const bool fov   = std::fabs(draw.a / world.a - 1.0f) > tol.fov_ratio_eps ||
                       std::fabs(draw.b / world.b - 1.0f) > tol.fov_ratio_eps;
    const float nw = world.near_z(), nd = draw.near_z();
    const bool depth = (std::fabs(nw) > 1e-6f) && std::fabs(nd / nw - 1.0f) > tol.near_ratio_eps;
    if (fov && depth) return ProjClass::Both;
    if (fov)   return ProjClass::FovDiffers;
    if (depth) return ProjClass::DepthDiffers;
    return ProjClass::Same;
}

ProjParams correction_projection(const ProjParams& draw, const ProjParams& world, const Tolerances& tol)
{
    ProjParams p = draw;
    if (!draw.perspective || !world.perspective) return p;
    if (std::fabs(draw.a / world.a - 1.0f) <= tol.fov_ratio_eps) p.a = world.a;
    if (std::fabs(draw.b / world.b - 1.0f) <= tol.fov_ratio_eps) p.b = world.b;
    return p;
}

const char* draw_class_name(DrawClass c)
{
    switch (c) {
    case DrawClass::Unclassified:  return "-";
    case DrawClass::OwnProjection: return "own-proj";
    case DrawClass::Viewmodel:     return "VIEWMODEL";
    }
    return "?";
}

DrawClass classify(const DrawSignature& s, const Thresholds& th)
{
    if (!s.has_wvp) return DrawClass::Unclassified;
    if (s.proj == ProjClass::NoWvp || s.proj == ProjClass::NotPerspective) return DrawClass::Unclassified;

    const bool own_fov   = s.proj == ProjClass::FovDiffers || s.proj == ProjClass::Both;
    const bool own_depth = s.proj == ProjClass::DepthDiffers || s.proj == ProjClass::Both;
    const bool near = s.view_dist >= 0.0f && s.view_dist <= th.max_view_dist &&
                      s.view_origin[2] >= -th.max_view_z && s.view_origin[2] <= th.max_view_z;

    const bool bones_ok = s.has_bones || !th.require_bones;
    if (bones_ok && ((th.accept_on_projection && own_fov) || near)) return DrawClass::Viewmodel;
    if (own_fov || own_depth) return DrawClass::OwnProjection;
    return DrawClass::Unclassified;
}

ProjParams select_projection(ProjSelect sel, const ProjParams& vm, const ProjParams& world)
{
    if (!world.perspective) return vm;
    switch (sel) {
    case ProjSelect::Viewmodel: return vm;
    case ProjSelect::World:     return world;
    case ProjSelect::Hybrid: {
        ProjParams p = vm;          // depth range stays the weapon's own
        p.a = world.a; p.b = world.b;
        return p;
    }
    }
    return vm;
}

bool build_viewmodel_correction(const ProjParams& p_vm, const Mat4 delta_view,
                                const Mat4 c_view, const ProjParams& p_sel, Mat4 out)
{
    Mat4 pinv, psel, t1, t2;
    if (!invert_projection(p_vm, pinv)) return false;
    if (!p_sel.perspective) return false;
    make_projection(p_sel, psel);
    m4::multiply(pinv, delta_view, t1);
    m4::multiply(t1, c_view, t2);
    m4::multiply(t2, psel, out);
    return true;
}

void openvr_pose_to_view(const float pose3x4[12], Mat4 out)
{
    m4::identity(out);
    // Row-vector form: our M[r][c] is the column-vector R[c][r].
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            at(out, r, c) = pose3x4[c * 4 + r];
    at(out, 3, 0) = pose3x4[0 * 4 + 3];
    at(out, 3, 1) = pose3x4[1 * 4 + 3];
    at(out, 3, 2) = pose3x4[2 * 4 + 3];

    // Conjugate by C = diag(1,1,-1): negate every element with exactly one z.
    at(out, 0, 2) = -at(out, 0, 2);
    at(out, 1, 2) = -at(out, 1, 2);
    at(out, 2, 0) = -at(out, 2, 0);
    at(out, 2, 1) = -at(out, 2, 1);
    at(out, 3, 2) = -at(out, 3, 2);
}

bool make_grip_delta(const Mat4 head_view, const Mat4 grip_view,
                     const Mat4 ref_head_view, const Mat4 ref_grip_view,
                     float units_per_metre, Mat4 out)
{
    Mat4 head_inv, ref_head_inv;
    if (!m4::invert(head_view, head_inv)) return false;
    if (!m4::invert(ref_head_view, ref_head_inv)) return false;

    Mat4 grip_in_head, ref_grip_in_head;
    m4::multiply(grip_view, head_inv, grip_in_head);
    m4::multiply(ref_grip_view, ref_head_inv, ref_grip_in_head);

    Mat4 ref_inv;
    if (!m4::invert(ref_grip_in_head, ref_inv)) return false;
    m4::multiply(ref_inv, grip_in_head, out);

    if (units_per_metre != 1.0f) {
        at(out, 3, 0) *= units_per_metre;
        at(out, 3, 1) *= units_per_metre;
        at(out, 3, 2) *= units_per_metre;
    }
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

void openvr_rotation_to_view(const float pose3x4[12], Mat4 out)
{
    m4::identity(out);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            at(out, r, c) = pose3x4[c * 4 + r];      // transpose to row-vector
    // Conjugate by C = diag(1,1,-1): negate the elements with exactly one z.
    at(out, 0, 2) = -at(out, 0, 2);
    at(out, 1, 2) = -at(out, 1, 2);
    at(out, 2, 0) = -at(out, 2, 0);
    at(out, 2, 1) = -at(out, 2, 1);
}

void camera_basis(const float cam_rows[9], Mat4 out)
{
    m4::identity(out);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            at(out, r, c) = cam_rows[r * 3 + c];
}

namespace {

// Rotations are orthonormal, so the inverse is the transpose - cheaper and
// numerically kinder than a general inverse.
void rotation_inverse(const Mat4 in, Mat4 out) { m4::transpose(in, out); }

// v * M for a direction (no translation).
void rotate_dir(const float v[3], const Mat4 M, float out[3])
{
    for (int c = 0; c < 3; ++c)
        out[c] = v[0] * at(M, 0, c) + v[1] * at(M, 1, c) + v[2] * at(M, 2, c);
}

// B = R_now * R_ref^-1 : the head's rotation since the reference, expressed in
// the frame the reference defined (which is the game camera's frame).
bool head_relative(const float head_now[12], const float head_ref[12], Mat4 out)
{
    Mat4 rnow, rref, rinv;
    openvr_rotation_to_view(head_now, rnow);
    openvr_rotation_to_view(head_ref, rref);
    rotation_inverse(rref, rinv);
    m4::multiply(rnow, rinv, out);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

} // namespace

bool head_world_rotation(const float head_now[12], const float head_ref[12],
                         float turn_yaw, const float cam_rows[9], Mat4 out)
{
    Mat4 B;
    if (!head_relative(head_now, head_ref, B)) return false;

    Mat4 T, A;
    m4::rotation_y(turn_yaw, T);
    m4::multiply(B, T, A);            // camera rotation relative to the body

    // Conjugate into world space and invert: the correction rotates the WORLD,
    // which is the inverse of rotating the camera.
    //   W   = V * A * M_cw
    //   rot = W^-1 = V * A^-1 * M_cw      (V = M_cw^-1 = M_cw^T)
    Mat4 mcw, v, ainv, t1;
    camera_basis(cam_rows, mcw);
    rotation_inverse(mcw, v);
    rotation_inverse(A, ainv);
    m4::multiply(v, ainv, t1);
    m4::multiply(t1, mcw, out);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

bool head_right_in_world(const float head_now[12], const float head_ref[12],
                         float turn_yaw, const float cam_rows[9], float out[3])
{
    Mat4 B;
    if (!head_relative(head_now, head_ref, B)) return false;
    Mat4 T, A, mcw;
    m4::rotation_y(turn_yaw, T);
    m4::multiply(B, T, A);
    camera_basis(cam_rows, mcw);

    const float right_local[3] = { 1.0f, 0.0f, 0.0f };
    float right_cam[3];
    rotate_dir(right_local, A, right_cam);      // head right, in camera coords
    rotate_dir(right_cam, mcw, out);            // ... and in world
    for (int i = 0; i < 3; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

bool lean_in_world(const float dpos_openvr[3], const float head_ref[12],
                   float turn_yaw, const float cam_rows[9], float out[3])
{
    Mat4 rref, rinv;
    openvr_rotation_to_view(head_ref, rref);
    rotation_inverse(rref, rinv);

    // OpenVR is right-handed with -Z forward; our view space is left-handed
    // with +Z forward, so the z component flips (C = diag(1,1,-1)).
    const float d_lh[3] = { dpos_openvr[0], dpos_openvr[1], -dpos_openvr[2] };

    float d_ref[3], d_body[3];
    rotate_dir(d_lh, rinv, d_ref);              // into the reference's frame
    Mat4 T; m4::rotation_y(turn_yaw, T);
    rotate_dir(d_ref, T, d_body);               // carried by a deliberate turn

    Mat4 mcw; camera_basis(cam_rows, mcw);
    rotate_dir(d_body, mcw, out);
    for (int i = 0; i < 3; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// Horizontal component below which yaw carries no usable information at all
// (~81 degrees of pitch), and the component above which it is fully trusted
// (~50 degrees). Between them the authority ramps linearly, so there is no cliff
// where the body stops following part-way through a gesture.
constexpr float kYawRefuse = 0.15f;
constexpr float kYawFull   = 0.64f;

bool aim_deviation(const float dir_now[3], const float dir_ref[3],
                   float& yaw_error, float& pitch_error, float& authority)
{
    yaw_error = 0.0f; pitch_error = 0.0f; authority = 0.0f;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(dir_now[i]) || !std::isfinite(dir_ref[i])) return false;

    const float hn = std::sqrt(dir_now[0]*dir_now[0] + dir_now[2]*dir_now[2]);
    const float hr = std::sqrt(dir_ref[0]*dir_ref[0] + dir_ref[2]*dir_ref[2]);
    // Near the pole a direction has no meaningful yaw; refuse rather than
    // inventing one (BFVR's rule). The reference counts too - a reference
    // captured while pointing at the sky has no usable yaw either.
    const float h = (hn < hr) ? hn : hr;
    if (h < kYawRefuse) return false;

    authority = (h - kYawRefuse) / (kYawFull - kYawRefuse);
    if (authority > 1.0f) authority = 1.0f;

    const float yaw_now = std::atan2(dir_now[0], dir_now[2]);
    const float yaw_ref = std::atan2(dir_ref[0], dir_ref[2]);
    constexpr float kPiF = 3.14159265358979f;
    float d = yaw_now - yaw_ref;
    while (d <= -kPiF) d += 2.0f * kPiF;
    while (d >   kPiF) d -= 2.0f * kPiF;
    yaw_error = d;

    pitch_error = std::atan2(dir_now[1], hn) - std::atan2(dir_ref[1], hr);
    return true;
}

bool controller_dir_in_body(const float ctrl_pose[12], const float head_ref[12],
                            float turn_yaw, float out[3])
{
    Mat4 rctl, rref, rinv;
    openvr_rotation_to_view(ctrl_pose, rctl);
    openvr_rotation_to_view(head_ref, rref);
    rotation_inverse(rref, rinv);

    // The controller's pointing axis. In our left-handed view convention the
    // forward of a converted OpenVR pose is +z (OpenVR's -Z), i.e. row 2.
    const float fwd_local[3] = { 0.0f, 0.0f, 1.0f };
    float fwd_track[3], fwd_ref[3];
    rotate_dir(fwd_local, rctl, fwd_track);   // into tracking space
    rotate_dir(fwd_track, rinv, fwd_ref);     // into the reference's frame

    Mat4 T; m4::rotation_y(turn_yaw, T);
    rotate_dir(fwd_ref, T, out);              // carried by a deliberate turn

    for (int i = 0; i < 3; ++i) if (!std::isfinite(out[i])) return false;
    const float len = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    if (len < 1e-4f) return false;
    for (int i = 0; i < 3; ++i) out[i] /= len;
    return true;
}

bool grip_delta_step_is_sane(const Mat4 previous, const Mat4 current, float max_step)
{
    const float dx = at(current, 3, 0) - at(previous, 3, 0);
    const float dy = at(current, 3, 1) - at(previous, 3, 1);
    const float dz = at(current, 3, 2) - at(previous, 3, 2);
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;
    return (dx * dx + dy * dy + dz * dz) <= max_step * max_step;
}

} // namespace drawpolicy
