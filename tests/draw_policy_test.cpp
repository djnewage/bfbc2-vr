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

// A controller pose that has not moved relative to the head must produce an
// identity delta no matter where the player physically is - the property that
// keeps walking and leaning from dragging the weapon around.
static void test_grip_delta()
{
    // OpenVR 3x4 (column-vector, RH): rotation about Y by a, translation t.
    auto pose = [](float a, float tx, float ty, float tz, float out[12]) {
        const float c = std::cos(a), s = std::sin(a);
        const float m[12] = { c, 0, s, tx,
                              0, 1, 0, ty,
                             -s, 0, c, tz };
        for (int i = 0; i < 12; ++i) out[i] = m[i];
    };

    float h0[12], g0[12], h1[12], g1[12];
    pose(0.0f, 0.0f, 1.6f, 0.0f, h0);
    pose(0.0f, 0.2f, 1.2f, -0.3f, g0);
    Mat4 H0, G0; openvr_pose_to_view(h0, H0); openvr_pose_to_view(g0, G0);

    // 1. Head and hand both move 1 m sideways: nothing relative changed.
    pose(0.0f, 1.0f, 1.6f, 0.0f, h1);
    pose(0.0f, 1.2f, 1.2f, -0.3f, g1);
    Mat4 H1, G1, d; openvr_pose_to_view(h1, H1); openvr_pose_to_view(g1, G1);
    CHECK(make_grip_delta(H1, G1, H0, G0, 1.0f, d));
    Mat4 I; m4::identity(I);
    CHECK(near_matrix(d, I, 1e-4f));

    // 2. Hand alone moves 10 cm right and 5 cm up.
    pose(0.0f, 0.3f, 1.25f, -0.3f, g1);
    openvr_pose_to_view(g1, G1);
    CHECK(make_grip_delta(H0, G1, H0, G0, 1.0f, d));
    CHECK_NEAR(at(d,3,0), 0.1f, 1e-4f);
    CHECK_NEAR(at(d,3,1), 0.05f, 1e-4f);
    CHECK_NEAR(at(d,3,2), 0.0f, 1e-4f);

    // 3. Handedness: OpenVR forward is -Z, view space forward is +Z, so a hand
    //    pushed away from the player is +z in view space.
    pose(0.0f, 0.2f, 1.2f, -0.6f, g1);          // 0.3 m further from the player
    openvr_pose_to_view(g1, G1);
    CHECK(make_grip_delta(H0, G1, H0, G0, 1.0f, d));
    CHECK_NEAR(at(d,3,2), 0.3f, 1e-4f);

    // 4. Rotation in place: the delta carries a COMPENSATING TRANSLATION (a
    //    rotation-only delta is therefore wrong - it would swing the weapon
    //    about the eye). The invariant is that the grip POINT stays fixed.
    pose(0.4f, 0.2f, 1.2f, -0.3f, g1);
    openvr_pose_to_view(g1, G1);
    CHECK(make_grip_delta(H0, G1, H0, G0, 1.0f, d));
    CHECK(std::fabs(at(d,0,2)) > 0.3f);                       // it did rotate
    CHECK(std::fabs(at(d,3,0)) + std::fabs(at(d,3,2)) > 0.01f); // and shifted
    {
        // The grip point in head space: translation of grip * inverse(head).
        Mat4 Hi, gih; CHECK(m4::invert(H0, Hi)); m4::multiply(G0, Hi, gih);
        const float p[4] = { at(gih,3,0), at(gih,3,1), at(gih,3,2), 1.0f };
        float q[3] = {};
        for (int c = 0; c < 3; ++c)
            q[c] = p[0]*at(d,0,c) + p[1]*at(d,1,c) + p[2]*at(d,2,c) + at(d,3,c);
        CHECK_NEAR(q[0], p[0], 1e-4f);
        CHECK_NEAR(q[1], p[1], 1e-4f);
        CHECK_NEAR(q[2], p[2], 1e-4f);
    }

    // 5. Scale and sanity gate.
    pose(0.0f, 0.3f, 1.2f, -0.3f, g1);
    openvr_pose_to_view(g1, G1);
    CHECK(make_grip_delta(H0, G1, H0, G0, 10.0f, d));
    CHECK_NEAR(at(d,3,0), 1.0f, 1e-3f);

    // 6. The gate is on the STEP between samples, not the distance from
    //    calibration: a hand a long way from where it started is normal, a
    //    hand that jumped a long way in one frame is a tracking glitch.
    Mat4 a, b;
    m4::translation(0.9f, 0.0f, 0.0f, a);      // 0.9 m from calibration
    m4::translation(0.93f, 0.0f, 0.0f, b);     // 3 cm later
    CHECK(grip_delta_step_is_sane(a, b, 0.5f));
    m4::translation(1.8f, 0.0f, 0.0f, b);      // 0.9 m in one frame
    CHECK(!grip_delta_step_is_sane(a, b, 0.5f));
}

// --------------------------------------------------------------------------
// Head orientation. These are the tests that pin the tilting-horizon bug: the
// first version of the correction used two Euler scalars and failed both the
// roll case and the yaw+pitch case.

// An OpenVR 3x4 (column-vector rotation, +X right / +Y up / -Z forward) built
// from intrinsic yaw, then pitch, then roll.
static void make_pose(float yaw, float pitch, float roll, const float pos[3], float out[12])
{
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll),  sr = std::sin(roll);
    const float Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
    const float Rx[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
    const float Rz[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };
    float t[3][3] = {}, R[3][3] = {};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { t[i][j] = 0; for (int k = 0; k < 3; ++k) t[i][j] += Ry[i][k] * Rx[k][j]; }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { R[i][j] = 0; for (int k = 0; k < 3; ++k) R[i][j] += t[i][k] * Rz[k][j]; }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = R[r][c];
        out[r * 4 + 3] = pos ? pos[r] : 0.0f;
    }
}

// Identity camera basis: right +x, up +y, forward +z. With this basis the
// camera's world axes after the correction are just the rows of A = B*T, so
// the assertions below read directly.
static const float kIdentityBasis[9] = { 1,0,0, 0,1,0, 0,0,1 };

static void test_head_orientation()
{
    const float origin[3] = { 0, 0, 0 };
    float ref[12], now[12];
    Mat4 rot;

    // 1. Not moved: no correction.
    make_pose(0.0f, 0.0f, 0.0f, origin, ref);
    make_pose(0.0f, 0.0f, 0.0f, origin, now);
    CHECK(head_world_rotation(now, ref, 0.0f, kIdentityBasis, rot));
    Mat4 I; m4::identity(I);
    CHECK(near_matrix(rot, I, 1e-5f));

    // 2. PURE ROLL. Tilting the head must rotate the world about the view
    //    axis: forward is preserved, up is tilted. The Euler construction
    //    produced identity here - the horizon rolled with the head.
    make_pose(0.0f, 0.0f, 0.5f, origin, now);
    CHECK(head_world_rotation(now, ref, 0.0f, kIdentityBasis, rot));
    {
        const float fwd[3] = { 0, 0, 1 };
        float f2[3];
        for (int c = 0; c < 3; ++c)
            f2[c] = fwd[0]*at(rot,0,c) + fwd[1]*at(rot,1,c) + fwd[2]*at(rot,2,c);
        CHECK_NEAR(f2[0], 0.0f, 1e-4f);
        CHECK_NEAR(f2[1], 0.0f, 1e-4f);
        CHECK_NEAR(f2[2], 1.0f, 1e-4f);          // view axis unchanged
        const float up[3] = { 0, 1, 0 };
        float u2[3];
        for (int c = 0; c < 3; ++c)
            u2[c] = up[0]*at(rot,0,c) + up[1]*at(rot,1,c) + up[2]*at(rot,2,c);
        CHECK(std::fabs(u2[0]) > 0.4f);          // world up genuinely tilted
    }

    // 3. YAW + PITCH COMBINED must stay ROLL-FREE. The camera's right axis is
    //    row 0 of A = rot^-1 = rot^T; it must remain horizontal. This is the
    //    reported bug: with the Euler construction, pitching and then turning
    //    canted the horizon.
    make_pose(0.6f, -0.35f, 0.0f, origin, now);
    CHECK(head_world_rotation(now, ref, 0.0f, kIdentityBasis, rot));
    {
        // rot rotates the world; the camera's axes are the rows of its inverse.
        Mat4 inv; CHECK(m4::invert(rot, inv));
        const float right_y = at(inv, 0, 1);     // y component of camera right
        CHECK_NEAR(right_y, 0.0f, 1e-3f);
    }

    // 4. And with roll added, the right axis SHOULD leave horizontal - by
    //    exactly the roll. (Guards against "fixed" by force-levelling.)
    make_pose(0.6f, -0.35f, 0.4f, origin, now);
    CHECK(head_world_rotation(now, ref, 0.0f, kIdentityBasis, rot));
    {
        Mat4 inv; CHECK(m4::invert(rot, inv));
        CHECK(std::fabs(at(inv, 0, 1)) > 0.3f);
    }

    // 5. A deliberate turn with a still head is a pure yaw of the view.
    make_pose(0.0f, 0.0f, 0.0f, origin, now);
    CHECK(head_world_rotation(now, ref, 0.5f, kIdentityBasis, rot));
    {
        Mat4 inv; CHECK(m4::invert(rot, inv));
        CHECK_NEAR(at(inv, 0, 1), 0.0f, 1e-4f);   // no roll
        CHECK(std::fabs(at(inv, 0, 2)) > 0.4f);   // but it did turn
    }
}

static void test_eye_axis_and_lean()
{
    const float origin[3] = { 0, 0, 0 };
    float ref[12], now[12];

    // The eye baseline rides the HEAD's right axis, so a tilted head tilts the
    // stereo separation. With a level head it matches the body's right.
    make_pose(0.0f, 0.0f, 0.0f, origin, ref);
    make_pose(0.0f, 0.0f, 0.0f, origin, now);
    float right[3];
    CHECK(head_right_in_world(now, ref, 0.0f, kIdentityBasis, right));
    CHECK_NEAR(right[0], 1.0f, 1e-4f);
    CHECK_NEAR(right[1], 0.0f, 1e-4f);

    make_pose(0.0f, 0.0f, 0.6f, origin, now);      // ear to shoulder
    CHECK(head_right_in_world(now, ref, 0.0f, kIdentityBasis, right));
    CHECK(std::fabs(right[1]) > 0.3f);             // the baseline tilted with it

    // Leaning is expressed in the frame the recenter defined. Recentre facing
    // 90 degrees away from the tracking origin's forward, then lean along the
    // tracking +x axis: in the game that must come out along the body's
    // FORWARD, not its right. The first version mapped raw tracking axes
    // straight through the camera basis and got this wrong.
    make_pose(1.5707963f, 0.0f, 0.0f, origin, ref);
    const float dpos[3] = { 0.2f, 0.0f, 0.0f };
    float lean[3];
    CHECK(lean_in_world(dpos, ref, 0.0f, kIdentityBasis, lean));
    CHECK_NEAR(std::sqrt(lean[0]*lean[0] + lean[1]*lean[1] + lean[2]*lean[2]), 0.2f, 1e-3f);
    CHECK(std::fabs(lean[2]) > 0.15f);             // along forward
    CHECK_NEAR(lean[0], 0.0f, 1e-3f);              // not along right

    // With no reference yaw it is a plain sideways lean.
    make_pose(0.0f, 0.0f, 0.0f, origin, ref);
    CHECK(lean_in_world(dpos, ref, 0.0f, kIdentityBasis, lean));
    CHECK_NEAR(lean[0], 0.2f, 1e-3f);
}

// The aim error is measured against a CAPTURED reference direction, so the
// grip-versus-aim axis question stops mattering. These pin the properties that
// make that safe.
static void test_aim_deviation()
{
    auto dir = [](float yaw, float pitch, float out[3]) {
        out[0] = std::sin(yaw) * std::cos(pitch);
        out[1] = std::sin(pitch);
        out[2] = std::cos(yaw) * std::cos(pitch);
    };
    float a[3], b[3], y = 0.0f, p = 0.0f, w = 0.0f;

    // Same direction as the reference: no error, whatever that direction is.
    dir(0.7f, -0.3f, a);
    CHECK(aim_deviation(a, a, y, p, w));
    CHECK_NEAR(y, 0.0f, 1e-5f);
    CHECK_NEAR(p, 0.0f, 1e-5f);

    // A known yaw deviation is reported as exactly that yaw - and the absolute
    // direction of the reference does not matter, which is the whole point of
    // measuring against a capture rather than an assumed axis.
    dir(0.7f, -0.3f, a);
    dir(0.7f + 0.25f, -0.3f, b);
    CHECK(aim_deviation(b, a, y, p, w));
    CHECK_NEAR(y, 0.25f, 1e-4f);
    CHECK_NEAR(p, 0.0f, 1e-4f);

    // Pitch likewise.
    dir(0.7f, -0.3f + 0.2f, b);
    CHECK(aim_deviation(b, a, y, p, w));
    CHECK_NEAR(p, 0.2f, 1e-4f);
    CHECK_NEAR(y, 0.0f, 1e-4f);

    // Wrap-around: deviating across +/-180 is a small error, not a full turn.
    dir(3.10f, 0.0f, a);
    dir(-3.10f, 0.0f, b);
    CHECK(aim_deviation(b, a, y, p, w));
    CHECK(std::fabs(y) < 0.15f);

    // A PURE ROLL of the controller about its own pointing axis must produce no
    // deviation - otherwise a wrist twist would steer the player's body. The
    // pointing direction is unchanged by such a roll, so this reduces to the
    // identical-direction case, and it is asserted explicitly because it is the
    // property that would be silently lost if the direction were ever derived
    // from a different row of the pose.
    dir(0.4f, 0.1f, a);
    CHECK(aim_deviation(a, a, y, p, w));
    CHECK_NEAR(y, 0.0f, 1e-5f);

    // Degenerate inputs are refused rather than guessed at.
    const float pole[3] = { 0.0f, 1.0f, 0.0f };
    CHECK(!aim_deviation(pole, a, y, p, w));
    CHECK(!aim_deviation(a, pole, y, p, w));
}

// Aiming vertically spun the player's body. The yaw reading is enormous and
// jittery near the pole while the gun has barely moved horizontally, and the old
// guard only refused within 0.06 degrees of straight up, so essentially every
// steep aim was chased at full gain. These pin the taper that fixes it.
static void test_aim_deviation_near_pole()
{
    auto dir = [](float yaw, float pitch, float out[3]) {
        out[0] = std::sin(yaw) * std::cos(pitch);
        out[1] = std::sin(pitch);
        out[2] = std::cos(yaw) * std::cos(pitch);
    };
    const float deg = 3.14159265358979f / 180.0f;
    float a[3], b[3], y = 0.0f, p = 0.0f, w = 0.0f;

    // Level: full authority, and the error is passed through untouched.
    dir(0.0f, 0.0f, a);
    dir(0.30f, 0.0f, b);
    CHECK(aim_deviation(b, a, y, p, w));
    CHECK_NEAR(w, 1.0f, 1e-4f);
    CHECK_NEAR(y, 0.30f, 1e-4f);

    // Steeply up: refused outright. This is the case that used to sail through
    // the 1e-3 guard and drive a full-gain turn.
    dir(0.0f, 85.0f * deg, a);
    dir(40.0f * deg, 85.0f * deg, b);
    CHECK(!aim_deviation(b, a, y, p, w));

    // A reference captured while pointing at the sky is just as unusable.
    dir(0.0f, 0.0f, b);
    CHECK(!aim_deviation(b, a, y, p, w));

    // Authority falls off monotonically as the aim steepens, with no cliff: the
    // body keeps following, progressively more gently, until it stops.
    float prev = 2.0f;
    for (float pitch_deg = 0.0f; pitch_deg <= 80.0f; pitch_deg += 10.0f) {
        dir(0.0f, pitch_deg * deg, a);
        dir(0.20f, pitch_deg * deg, b);
        const bool ok = aim_deviation(b, a, y, p, w);
        if (!ok) { CHECK(pitch_deg >= 70.0f); break; }   // only the steep end refuses
        CHECK(w <= prev + 1e-5f);
        CHECK(w >= 0.0f);
        prev = w;
    }

    // The property that actually stops the spin: the SAME yaw deviation commands
    // a strictly smaller turn when the gun is pointed steeply than when level.
    dir(0.0f, 0.0f, a);      dir(0.20f, 0.0f, b);
    CHECK(aim_deviation(b, a, y, p, w));
    const float level = std::fabs(y) * w;
    dir(0.0f, 65.0f * deg, a); dir(0.20f, 65.0f * deg, b);
    CHECK(aim_deviation(b, a, y, p, w));
    CHECK(std::fabs(y) * w < level);
}

// A perspective ProjParams with the given total horizontal field, at 4:3.
static ProjParams cam_at(float fov_h_deg)
{
    const float deg = 3.14159265358979f / 180.0f;
    const float tan_h = std::tan(0.5f * fov_h_deg * deg);
    const float tan_v = tan_h / (4.0f / 3.0f);
    ProjParams p;
    p.a = 1.0f / tan_h;
    p.b = 1.0f / tan_v;
    p.q = 1.0001f;
    p.t = -0.1f;
    p.perspective = true;
    return p;
}

static void test_camera_plausibility()
{
    // The exact failure that black-boxed the headset: the world frustum
    // collapsed to 18.5 degrees and was adopted as the player's camera.
    CHECK(!camera_is_plausible(cam_at(18.5f), 0.0f));
    // The game's own field, and a widened one, must both be accepted.
    CHECK(camera_is_plausible(cam_at(58.9f), 0.0f));
    CHECK(camera_is_plausible(cam_at(109.4f), 0.0f));
    // Orthographic / unrecoverable is never a first-person camera.
    ProjParams ortho;              // perspective defaults to false
    CHECK(!camera_is_plausible(ortho, 0.0f));

    // The aspect test, when it is used at all.
    CHECK(camera_is_plausible(cam_at(58.9f), 4.0f / 3.0f));
    CHECK(!camera_is_plausible(cam_at(58.9f), 16.0f / 9.0f));
    // ...and the reason it is passed 0 in the real code: the collapsed frustum
    // has the SAME 4:3 shape as the healthy one, so aspect cannot separate them.
    CHECK(!camera_is_plausible(cam_at(18.5f), 4.0f / 3.0f));
}

static void test_choose_player_camera()
{
    // A plausible camera wins over an implausible one even with less weight.
    {
        const ProjParams c[2] = { cam_at(18.5f), cam_at(58.9f) };
        const unsigned w[2] = { 900, 3 };
        CHECK(choose_player_camera(c, w, 2, 0.0f, nullptr) == 1);
    }
    // Among plausible candidates, the most-used one wins.
    {
        const ProjParams c[2] = { cam_at(58.9f), cam_at(90.0f) };
        const unsigned w[2] = { 10, 400 };
        CHECK(choose_player_camera(c, w, 2, 0.0f, nullptr) == 1);
    }
    // Equal weight: the one nearer last frame's camera breaks the tie, so a
    // steady view does not alternate between two passes.
    {
        const ProjParams c[2] = { cam_at(58.9f), cam_at(120.0f) };
        const unsigned w[2] = { 50, 50 };
        const ProjParams prev = cam_at(119.0f);
        CHECK(choose_player_camera(c, w, 2, 0.0f, &prev) == 1);
        const ProjParams prev2 = cam_at(60.0f);
        CHECK(choose_player_camera(c, w, 2, 0.0f, &prev2) == 0);
    }
    // No plausible candidate at all -> refuse, so the caller holds what it had.
    {
        const ProjParams c[2] = { cam_at(18.5f), cam_at(5.0f) };
        const unsigned w[2] = { 900, 900 };
        CHECK(choose_player_camera(c, w, 2, 0.0f, nullptr) == -1);
    }
    CHECK(choose_player_camera(nullptr, nullptr, 0, 0.0f, nullptr) == -1);
}

int main()
{
    test_camera_plausibility();
    test_choose_player_camera();
    test_aim_deviation();
    test_aim_deviation_near_pole();
    test_head_orientation();
    test_eye_axis_and_lean();
    test_grip_delta();
    test_depth_slice_translation_is_exact();
    test_recover_roundtrip();
    test_rejects_non_perspective();
    test_compare_and_classify();
    test_viewmodel_correction_collapses_to_global();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("draw_policy tests passed\n");
    return 0;
}
