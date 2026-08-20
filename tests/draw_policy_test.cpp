// Deterministic checks for draw_policy: projection recovery round-trips,
// the viewmodel correction collapses to the global one when P_vm == P, and the
// classifier fails closed. Plain asserts, no framework - run it, exit code 0
// means pass. Built as its own console exe (see CMakeLists BFBC2VR_BUILD_TESTS).
#include "draw_policy.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace drawpolicy;
using m4::Mat4;
using m4::at;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_NEAR(x, y, eps) do { const float _a = (x), _b = (y); if (std::fabs(_a - _b) > (eps)) { std::printf("FAIL %s:%d  %s=%g vs %s=%g\n", __FILE__, __LINE__, #x, _a, #y, _b); ++g_failures; } } while (0)

static void make_view(const float eye[3], float yaw, Mat4 out)
{
    // Camera-to-world then affine inverse, same recipe as camera_override.
    Mat4 rot, mcw, tmp;
    m4::rotation_y(yaw, rot);
    m4::copy(rot, mcw);
    at(mcw,3,0) = eye[0]; at(mcw,3,1) = eye[1]; at(mcw,3,2) = eye[2];
    m4::identity(out);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) at(out, r, c) = at(mcw, c, r);
    for (int c = 0; c < 3; ++c)
        at(out,3,c) = -(eye[0]*at(out,0,c) + eye[1]*at(out,1,c) + eye[2]*at(out,2,c));
    (void)tmp;
}

static bool near_matrix(const Mat4 a, const Mat4 b, float eps)
{
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > eps) return false;
    return true;
}

static void test_recover_roundtrip()
{
    ProjParams p; p.a = 1.3f; p.b = 2.1f; p.q = 1.0005f; p.t = -0.05f; p.perspective = true;
    Mat4 P; make_projection(p, P);

    const float eye[3] = { 10.0f, 2.0f, -4.0f };
    Mat4 V; make_view(eye, 0.7f, V);

    // Rigid world with a translation (object 0.4m in front, slightly right).
    Mat4 W, tmp; const float axis[3] = { 0.0f, 1.0f, 0.0f };
    m4::rotation_axis(axis, -0.3f, W);
    at(W,3,0) = 10.2f; at(W,3,1) = 1.9f; at(W,3,2) = -3.7f;

    Mat4 WV, M;
    m4::multiply(W, V, WV);
    m4::multiply(WV, P, M);

    float stored[16]; matrix_to_stored(M, stored);
    Mat4 M2; stored_to_matrix(stored, M2);
    CHECK(near_matrix(M, M2, 1e-6f));

    ProjParams r;
    CHECK(recover_projection(M, r));
    CHECK_NEAR(r.a, p.a, 1e-4f);
    CHECK_NEAR(r.b, p.b, 1e-4f);
    CHECK_NEAR(r.q, p.q, 1e-4f);
    CHECK_NEAR(r.t, p.t, 1e-4f);
    CHECK_NEAR(r.near_z(), -p.t / p.q, 1e-4f);

    // View origin = object origin through W*V.
    float o[3]; CHECK(view_origin(M, r, o));
    CHECK_NEAR(o[0], at(WV,3,0), 1e-3f);
    CHECK_NEAR(o[1], at(WV,3,1), 1e-3f);
    CHECK_NEAR(o[2], at(WV,3,2), 1e-3f);

    // P * P^-1 = I
    Mat4 Pi, I; CHECK(invert_projection(p, Pi));
    m4::multiply(P, Pi, I);
    Mat4 ident; m4::identity(ident);
    CHECK(near_matrix(I, ident, 1e-5f));

    // Uniform object scale must not disturb a/b.
    Mat4 S, WS, WSV, MS; m4::scale(0.01f, 0.01f, 0.01f, S);
    m4::multiply(S, W, WS); m4::multiply(WS, V, WSV); m4::multiply(WSV, P, MS);
    ProjParams rs; CHECK(recover_projection(MS, rs));
    CHECK_NEAR(rs.a, p.a, 1e-3f);
    CHECK_NEAR(rs.b, p.b, 1e-3f);
    (void)tmp;
}

static void test_rejects_non_perspective()
{
    Mat4 ortho; m4::identity(ortho);
    at(ortho,0,0) = 0.0025f; at(ortho,1,1) = 0.0033f; at(ortho,3,0) = -1.0f; at(ortho,3,1) = 1.0f;
    ProjParams r;
    CHECK(!recover_projection(ortho, r));
    CHECK(!r.perspective);

    // A bone row block (3x4 packed) is not A*P either.
    Mat4 bones = { 1,0,0,0.3f, 0,1,0,-0.1f, 0,0,1,0.8f, 0.5f,0.2f,0.1f,0 };
    CHECK(!recover_projection(bones, r));
}

static void test_compare_and_classify()
{
    ProjParams world; world.a = 1.0f; world.b = 1.5f; world.q = 1.001f; world.t = -0.1f; world.perspective = true;
    ProjParams same = world;
    ProjParams fov = world; fov.a = 2.4f; fov.b = 4.2f;
    ProjParams depth = world; depth.t = -0.02f;

    CHECK(compare_projection(same, world) == ProjClass::Same);
    CHECK(compare_projection(fov, world) == ProjClass::FovDiffers);
    CHECK(compare_projection(depth, world) == ProjClass::DepthDiffers);
    ProjParams np; CHECK(compare_projection(np, world) == ProjClass::NotPerspective);

    Thresholds th;
    DrawSignature s;
    CHECK(classify(s, th) == DrawClass::Unclassified);               // no wvp

    s.has_wvp = true; s.proj = ProjClass::Same; s.has_bones = true;
    s.view_origin[2] = 0.3f; s.view_dist = 0.35f;
    CHECK(classify(s, th) == DrawClass::Viewmodel);                  // near + bones

    s.has_bones = false;
    CHECK(classify(s, th) == DrawClass::Unclassified);               // bones required
    th.require_bones = false;
    CHECK(classify(s, th) == DrawClass::Viewmodel);

    th = Thresholds{};
    s.has_bones = true; s.view_dist = 5.0f; s.view_origin[2] = 5.0f;
    CHECK(classify(s, th) == DrawClass::Unclassified);               // far
    s.proj = ProjClass::FovDiffers;
    CHECK(classify(s, th) == DrawClass::Viewmodel);                  // own FOV is enough
    th.accept_on_projection = false;
    CHECK(classify(s, th) == DrawClass::OwnProjection);              // still needs its own P
    th = Thresholds{};
    s.proj = ProjClass::DepthDiffers;
    CHECK(classify(s, th) == DrawClass::OwnProjection);              // near plane alone != weapon
    s.has_bones = false; s.proj = ProjClass::FovDiffers;
    CHECK(classify(s, th) == DrawClass::OwnProjection);              // no bones: own P, no offset
    s.proj = ProjClass::Both; s.has_bones = true;
    CHECK(classify(s, th) == DrawClass::Viewmodel);

    s.proj = ProjClass::NotPerspective; s.view_dist = 0.1f; s.view_origin[2] = 0.1f;
    CHECK(classify(s, Thresholds{}) == DrawClass::Unclassified);     // ortho never
}

static void test_viewmodel_correction_collapses_to_global()
{
    ProjParams p; p.a = 1.2f; p.b = 2.0f; p.q = 1.0002f; p.t = -0.03f; p.perspective = true;
    Mat4 P; make_projection(p, P);
    const float eye[3] = { 3.0f, 1.7f, -2.0f };
    Mat4 V; make_view(eye, -1.1f, V);
    Mat4 VP; m4::multiply(V, P, VP);

    // A world-space correction r: rotate about the eye and shift.
    Mat4 rot, to0, back, t1, r_about, shift, r;
    const float up[3] = { 0, 1, 0 };
    m4::rotation_axis(up, 0.4f, rot);
    m4::translation(-eye[0], -eye[1], -eye[2], to0);
    m4::translation(eye[0], eye[1], eye[2], back);
    m4::multiply(to0, rot, t1); m4::multiply(t1, back, r_about);
    m4::translation(0.1f, -0.05f, 0.2f, shift);
    m4::multiply(r_about, shift, r);

    // Global: VP^-1 * r * VP
    Mat4 VPi, g1, global; CHECK(m4::invert(VP, VPi));
    m4::multiply(VPi, r, g1); m4::multiply(g1, VP, global);

    // View-space form: c_view = V^-1 * r * V, then P^-1 * I * c_view * P
    Mat4 Vi, c1, c_view; CHECK(m4::invert(V, Vi));
    m4::multiply(Vi, r, c1); m4::multiply(c1, V, c_view);
    Mat4 I; m4::identity(I);
    Mat4 vm; CHECK(build_viewmodel_correction(p, I, c_view, p, vm));
    CHECK(near_matrix(vm, global, 1e-3f));

    // With a push, the object moves along view +z by exactly the push.
    Mat4 push; m4::translation(0, 0, 0.4f, push);
    Mat4 ident_c; m4::identity(ident_c);
    Mat4 vm2; CHECK(build_viewmodel_correction(p, push, ident_c, p, vm2));
    Mat4 W; m4::translation(3.1f, 1.6f, -1.7f, W);
    Mat4 WV, M, M2; m4::multiply(W, V, WV); m4::multiply(WV, P, M); m4::multiply(M, vm2, M2);
    float o[3]; CHECK(view_origin(M2, p, o));
    CHECK_NEAR(o[0], at(WV,3,0), 1e-3f);
    CHECK_NEAR(o[1], at(WV,3,1), 1e-3f);
    CHECK_NEAR(o[2], at(WV,3,2) + 0.4f, 1e-3f);

    // Swapping the projection keeps the object where it is in view space.
    ProjParams world = p; world.a = 0.8f; world.b = 0.9f;
    Mat4 vm3; CHECK(build_viewmodel_correction(p, ident_c, ident_c, world, vm3));
    Mat4 M3; m4::multiply(M, vm3, M3);
    ProjParams r3; CHECK(recover_projection(M3, r3));
    CHECK_NEAR(r3.a, world.a, 1e-3f);
    CHECK_NEAR(r3.b, world.b, 1e-3f);
    float o3[3]; CHECK(view_origin(M3, r3, o3));
    CHECK_NEAR(o3[2], at(WV,3,2), 1e-3f);

    // Hybrid: world field, own depth.
    ProjParams h = select_projection(ProjSelect::Hybrid, p, world);
    CHECK_NEAR(h.a, world.a, 0.0f); CHECK_NEAR(h.t, p.t, 0.0f);
}

// THE 2026-08-20 FINDING. BFBC2 renders ~94% of the scene with projections
// whose near plane is 7.48 m or 21.34 m (depth slices) while VP carries the
// 0.1 m one. The global correction VP^-1 r VP applied to such a draw leaves a
// P' P^-1 residue that multiplies every TRANSLATION in r (eye offset, 6DOF,
// push) by t'/t = 75 or 213, while rotation stays exact. Looking around
// worked; stereo and leaning warped the world. The per-draw correction built
// around the draw's own P is exact.
static void test_depth_slice_translation_is_exact()
{
    ProjParams p; p.a = 1.8062f; p.b = 2.4083f; p.q = 1.0001f; p.t = -0.1f * p.q; p.perspective = true;
    ProjParams p2 = p; p2.t = -7.48f * p2.q;           // the 7.48 m slice
    Mat4 P, P2; make_projection(p, P); make_projection(p2, P2);
    const float eye[3] = { 243.0f, 174.0f, -148.0f };
    Mat4 V; make_view(eye, 0.6f, V);
    Mat4 VP; m4::multiply(V, P, VP);

    // r = pure translation (an eye shift of 3.3 cm along world x).
    Mat4 r; m4::translation(0.033f, 0.0f, 0.0f, r);
    Mat4 W; m4::translation(250.0f, 176.0f, -160.0f, W);   // an object ~15 m out
    Mat4 WV, M2; m4::multiply(W, V, WV); m4::multiply(WV, P2, M2);   // drawn with the slice P

    // Ground truth: W * r * V * P2.
    Mat4 Wr, WrV, truth; m4::multiply(W, r, Wr); m4::multiply(Wr, V, WrV); m4::multiply(WrV, P2, truth);

    // Global correction: wrong by the slice ratio.
    Mat4 VPi, g1, global, Mg; CHECK(m4::invert(VP, VPi));
    m4::multiply(VPi, r, g1); m4::multiply(g1, VP, global); m4::multiply(M2, global, Mg);
    float og[3], ot[3];
    ProjParams rg; CHECK(recover_projection(Mg, rg)); CHECK(view_origin(Mg, rg, og));
    ProjParams rt; CHECK(recover_projection(truth, rt)); CHECK(view_origin(truth, rt, ot));
    auto dist3 = [](const float* a, const Mat4 m) {
        const float dx = a[0]-at(m,3,0), dy = a[1]-at(m,3,1), dz = a[2]-at(m,3,2);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    };
    const float shift_truth  = dist3(ot, WV);
    const float shift_global = dist3(og, WV);
    CHECK_NEAR(shift_truth, 0.033f, 2e-3f);
    CHECK(shift_global > 1.0f);                         // ~75x the intended shift

    // Per-draw correction around P2: exact.
    Mat4 Vi, c1, c_view; CHECK(m4::invert(V, Vi));
    m4::multiply(Vi, r, c1); m4::multiply(c1, V, c_view);
    Mat4 I; m4::identity(I);
    Mat4 own, Mo; CHECK(build_viewmodel_correction(p2, I, c_view, p2, own));
    m4::multiply(M2, own, Mo);
    CHECK(near_matrix(Mo, truth, 1e-2f));

    // Snapping: a/b within tolerance take the world's, q/t stay the draw's.
    ProjParams d = p2; d.a *= 1.01f;
    ProjParams c = correction_projection(d, p);
    CHECK_NEAR(c.a, p.a, 0.0f); CHECK_NEAR(c.t, p2.t, 0.0f);
    d.a = p.a * 1.55f;
    c = correction_projection(d, p);
    CHECK_NEAR(c.a, d.a, 0.0f);
}

int main()
{
    test_depth_slice_translation_is_exact();
    test_recover_roundtrip();
    test_rejects_non_perspective();
    test_compare_and_classify();
    test_viewmodel_correction_collapses_to_global();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("draw_policy tests passed\n");
    return 0;
}
