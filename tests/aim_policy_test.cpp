// Deterministic tests for the pure input/aim math. Plain asserts; exit 0 = pass.
#include "aim_policy.h"

#include <cmath>
#include <cstdio>

using namespace aimpolicy;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_NEAR(x, y, eps) do { const float _a = (x), _b = (y); if (std::fabs(_a - _b) > (eps)) { std::printf("FAIL %s:%d  %s=%g vs %s=%g\n", __FILE__, __LINE__, #x, _a, #y, _b); ++g_failures; } } while (0)

static float deg(float d) { return d * kPi / 180.0f; }

static void test_wrap()
{
    CHECK_NEAR(wrap_pi(0.0f), 0.0f, 1e-6f);
    CHECK_NEAR(wrap_pi(kPi), kPi, 1e-5f);
    CHECK_NEAR(wrap_pi(-kPi), kPi, 1e-5f);            // -pi folds to +pi
    CHECK_NEAR(wrap_pi(1.5f * kPi), -0.5f * kPi, 1e-5f);
    CHECK_NEAR(wrap_pi(-3.2f * kPi), 0.8f * kPi, 1e-4f);
    CHECK_NEAR(wrap_pi(std::nanf("")), 0.0f, 0.0f);

    // THE ONE THAT MATTERS: stepping across the +/-180 boundary must be a small
    // delta, not a full turn. 179 deg -> -179 deg is +2 deg.
    CHECK_NEAR(body_delta(deg(-179.0f), deg(179.0f)), deg(2.0f), 1e-4f);
    CHECK_NEAR(body_delta(deg(179.0f), deg(-179.0f)), deg(-2.0f), 1e-4f);
}

static void test_forward_angles()
{
    const float fwd[3] = { 0.0f, 0.0f, -1.0f };       // -Z is yaw 0
    CHECK_NEAR(yaw_from_forward(fwd, 123.0f), 0.0f, 1e-5f);
    const float left[3] = { -1.0f, 0.0f, 0.0f };
    CHECK_NEAR(yaw_from_forward(left, 0.0f), deg(-90.0f), 1e-4f);
    const float right[3] = { 1.0f, 0.0f, 0.0f };
    CHECK_NEAR(yaw_from_forward(right, 0.0f), deg(90.0f), 1e-4f);

    // At the pole there is no meaningful yaw: keep the caller's, do not spin.
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    CHECK_NEAR(yaw_from_forward(up, 1.234f), 1.234f, 0.0f);
    CHECK_NEAR(pitch_from_forward(up), deg(90.0f), 1e-3f);
    CHECK_NEAR(pitch_from_forward(fwd), 0.0f, 1e-5f);
    const float down45[3] = { 0.0f, -1.0f, -1.0f };
    CHECK_NEAR(pitch_from_forward(down45), deg(-45.0f), 1e-4f);
}

static void test_compensation_and_turn_signs()
{
    const float ys = -1.0f;   // the shipped default

    // Hold the view still while the body turns: at yaw_sign -1 the reference
    // moves WITH the body.
    CHECK_NEAR(compensation_ref_delta(deg(10.0f), ys), deg(10.0f), 1e-6f);
    // A deliberate turn moves the reference the other way, so the view turns.
    CHECK_NEAR(turn_ref_delta(deg(10.0f), ys), deg(-10.0f), 1e-6f);

    // They must be OPPOSITE contributions to one accumulator, or a snap turn
    // would be silently undone by the compensation that follows it.
    CHECK(compensation_ref_delta(deg(10.0f), ys) * turn_ref_delta(deg(10.0f), ys) < 0.0f);
    CHECK_NEAR(compensation_ref_delta(deg(10.0f), ys) + turn_ref_delta(deg(10.0f), ys), 0.0f, 1e-6f);

    // Both flip with the convention.
    CHECK_NEAR(compensation_ref_delta(deg(10.0f), +1.0f), deg(-10.0f), 1e-6f);
    CHECK_NEAR(turn_ref_delta(deg(10.0f), +1.0f), deg(10.0f), 1e-6f);
}

static void test_stick_keys_hysteresis()
{
    StickKeys s = {};
    s = stick_to_keys(0.0f, 0.30f, s);      // below the on threshold
    CHECK(!s.forward);
    s = stick_to_keys(0.0f, 0.40f, s);      // past it
    CHECK(s.forward);
    s = stick_to_keys(0.0f, 0.30f, s);      // between the thresholds: HOLDS
    CHECK(s.forward);
    s = stick_to_keys(0.0f, 0.20f, s);      // below the off threshold
    CHECK(!s.forward);

    // Sweeping across the boundary must not chatter.
    StickKeys h = {};
    int changes = 0;
    bool prev = false;
    for (int i = 0; i < 40; ++i) {
        const float y = 0.30f + 0.02f * std::sin(static_cast<float>(i));
        h = stick_to_keys(0.0f, y, h);
        if (h.forward != prev) { ++changes; prev = h.forward; }
    }
    CHECK(changes <= 1);

    // Directions and exclusivity.
    StickKeys r = stick_to_keys(0.9f, 0.0f, StickKeys{});
    CHECK(r.right && !r.left && !r.forward && !r.back);
    StickKeys l = stick_to_keys(-0.9f, -0.9f, StickKeys{});
    CHECK(l.left && l.back);
}

static void test_snap_edge()
{
    SnapState st;
    CHECK(snap_turn_step(0.5f, st) == 0);        // below fire
    CHECK(snap_turn_step(0.9f, st) == 1);        // fires once
    CHECK(snap_turn_step(0.9f, st) == 0);        // held: no repeat
    CHECK(snap_turn_step(0.5f, st) == 0);        // above rearm: still armed off
    CHECK(snap_turn_step(0.1f, st) == 0);        // re-arms
    CHECK(snap_turn_step(-0.9f, st) == -1);      // fires the other way
    CHECK(snap_turn_step(-0.9f, st) == 0);

    // A stick held hard over must produce exactly one turn.
    SnapState st2;
    int fires = 0;
    for (int i = 0; i < 100; ++i) fires += (snap_turn_step(1.0f, st2) != 0) ? 1 : 0;
    CHECK(fires == 1);
}

int main()
{
    test_wrap();
    test_forward_angles();
    test_compensation_and_turn_signs();
    test_stick_keys_hysteresis();
    test_snap_edge();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("aim_policy tests passed\n");
    return 0;
}
