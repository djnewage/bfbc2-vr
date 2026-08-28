#include "aim_policy.h"

#include <cmath>

namespace aimpolicy {

float wrap_pi(float radians)
{
    if (!std::isfinite(radians)) return 0.0f;
    radians = std::remainder(radians, 2.0f * kPi);
    if (radians <= -kPi) radians += 2.0f * kPi;
    else if (radians > kPi) radians -= 2.0f * kPi;
    return radians;
}

float yaw_from_forward(const float f[3], float fallback_yaw)
{
    const float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (!(h >= 1e-3f)) return fallback_yaw;   // pole (or NaN): keep what we had
    return wrap_pi(std::atan2(f[0], -f[2]));
}

float pitch_from_forward(const float f[3])
{
    const float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    return std::atan2(f[1], h > 1e-6f ? h : 1e-6f);
}

StickKeys stick_to_keys(float x, float y, StickKeys previous, float on_threshold, float off_threshold)
{
    if (!std::isfinite(x)) x = 0.0f;
    if (!std::isfinite(y)) y = 0.0f;
    auto latch = [&](bool was, float v) {
        return was ? (v > off_threshold) : (v > on_threshold);
    };
    StickKeys out;
    out.forward = latch(previous.forward,  y);
    out.back    = latch(previous.back,    -y);
    out.right   = latch(previous.right,    x);
    out.left    = latch(previous.left,    -x);
    // A stick cannot mean both directions of one axis at once; if hysteresis
    // ever leaves both latched (a jump straight across zero), drop both.
    if (out.forward && out.back) { out.forward = out.back = false; }
    if (out.left && out.right)   { out.left = out.right = false; }
    return out;
}

int snap_turn_step(float x, SnapState& state, float fire, float rearm)
{
    if (!std::isfinite(x)) { state.armed = true; return 0; }
    if (std::fabs(x) < rearm) state.armed = true;
    if (!state.armed || std::fabs(x) < fire) return 0;
    state.armed = false;
    return (x > 0.0f) ? 1 : -1;
}

} // namespace aimpolicy
