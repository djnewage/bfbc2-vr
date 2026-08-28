// Pure input/aim math: no Windows, no OpenVR, no D3D, no globals.
//
// Everything here is a function of its arguments so it can be unit-tested on a
// desk (tests/aim_policy_test.cpp). The sign conventions below are the part
// most likely to be wrong on the first try, so each one is asserted by a test
// rather than trusted.
//
// CONVENTIONS (matching camera_override.cpp)
//   Yaw follows hmd_yaw_pitch(): forward = R*(0,0,-1), yaw = atan2(fx, -fz),
//   so yaw 0 faces -Z and positive yaw turns left. Pitch is positive up.
//   The view correction rotates the WORLD by yaw_sign*(yaw_head - ref_yaw)
//   about world up, with yaw_sign = -1 by default.
#pragma once

namespace aimpolicy {

constexpr float kPi = 3.14159265358979f;

// Wrap to (-pi, pi]. Every angular difference must go through this: without it
// a pass through +/-180 degrees injects a 2*pi kick.
float wrap_pi(float radians);

// Yaw/pitch of a direction in the convention above. Near the pole a direction
// has no meaningful yaw, so `fallback_yaw` is returned unchanged rather than
// injecting arbitrary horizontal motion (BFVR's rule).
float yaw_from_forward(const float f[3], float fallback_yaw);
float pitch_from_forward(const float f[3]);

// The change in the game's body yaw between two frames, wrapped.
inline float body_delta(float now, float prev) { return wrap_pi(now - prev); }

// ---------------------------------------------------------------------------
// View compensation and deliberate turns.
//
// The presented view direction is  view_yaw = body_yaw - yaw_sign*(head - ref).
//
//   To hold the view still while the body turns by db:  d_ref = -yaw_sign * db
//   To deliberately turn the view by t:                 d_ref = +yaw_sign * t
//
// They are opposite-signed contributions to one accumulator - exactly BFVR's
// `offset += requested - body` - so a deliberate turn can never be cancelled
// by the compensation that follows it.
inline float compensation_ref_delta(float body_delta_rad, float yaw_sign)
{
    return -yaw_sign * body_delta_rad;
}
inline float turn_ref_delta(float requested_rad, float yaw_sign)
{
    return +yaw_sign * requested_rad;
}

// ---------------------------------------------------------------------------
// Movement stick -> digital keys. Hysteresis, or a stick resting on the
// threshold chatters the key up and down every frame.
struct StickKeys { bool forward, back, left, right; };
StickKeys stick_to_keys(float x, float y, StickKeys previous,
                        float on_threshold = 0.35f, float off_threshold = 0.25f);

// A stick flick as a discrete event: fires once past `fire`, re-arms only
// below `rearm`. Returns -1, 0 or +1.
//
// `y` and the cooldown exist because raising the controller to aim upward drags
// the thumb across the stick, and the player was getting bursts of snap turns
// they never asked for. A deliberate turn is a sideways flick, so a gesture with
// more vertical than horizontal travel is ignored; and `now_seconds` enforces a
// minimum gap so that even if every other gate is fooled, a burst is impossible.
//
// `armed` starts FALSE: the latch must see a genuine at-rest reading before it
// will fire. Starting armed meant any code path that reset the state - or any
// dropped tracking sample reading as zero - could fire a turn with the stick
// still held over.
struct SnapState {
    bool  armed = false;
    float last_fire_seconds = -1e9f;
};
int snap_turn_step(float x, float y, float now_seconds, SnapState& state,
                   float fire = 0.80f, float rearm = 0.25f, float cooldown = 0.25f);

} // namespace aimpolicy
