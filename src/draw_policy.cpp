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

} // namespace drawpolicy
