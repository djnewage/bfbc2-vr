#include "vrinput.h"
#include "aim_policy.h"
#include "os_input.h"
#include "input_bus.h"
#include "vr_tracking.h"
#include "camera_override.h"
#include "logger.h"

#include <openvr.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vrinput {
namespace {

using aimpolicy::StickKeys;

// On by default now that the DirectInput path is proven in game. It only ever
// injects while the controllers report valid poses, and 'input off' stops it.
bool  g_enabled = true;

// TRANSPORT. SendInput was the bootstrap: it needs the game focused, Windows
// pointer acceleration distorts relative motion, and a stray moment types into
// whatever the user is really doing. BFBC2Game.exe imports dinput8 and has no
// raw input at all, so the device state DirectInput hands the game is a better
// place to inject - exact counts, focus-independent, and incapable of leaking
// keystrokes elsewhere. 'auto' prefers it when the wrappers report in.
enum class Path { Auto, DInput, SendInput, Off };
Path g_path = Path::Auto;

bool dinput_live() { return inputbus::hook_alive(); }
bool use_dinput()
{
    return g_path == Path::DInput || (g_path == Path::Auto && dinput_live());
}
bool  g_swap_hands = false;       // left-handed mirror
float g_snap_degrees = 30.0f;

// TURNING MOVES THE BODY, not just the view.
//
// The first version rotated only the presented view (the HMD reference yaw)
// and left the game's body facing its original direction. The world turned,
// but the engine's aim never did - so every shot went to the same place no
// matter which way the player faced. A turn has to be a real turn: drive the
// game's own mouse look, and the view follows because the view is built on the
// game camera.
//
// That needs mouse counts per radian, which depends on the player's in-game
// sensitivity and cannot be read from the config (Sensitivity0=0.53 and
// Scheme0Sensitivity=0.35 are not interpretable). So it is MEASURED: emit a
// known number of counts, watch how far the game camera actually turned, and
// low-pass the ratio. Until the first measurement lands, a conservative
// bootstrap is used - a wrong guess then under-turns rather than spinning.
// Measured in game 2026-08-28 through the DirectInput path: 334-406 counts
// per radian across 100-500 count deltas (the spread is the game's own mouse
// smoothing). Bootstrapping near the measured value means the very first turn
// is roughly right, and the live estimator refines from there.
float g_counts_per_rad = 360.0f;
bool  g_gain_measured = false;
unsigned g_gain_samples = 0;
float g_turn_sign = 1.0f;          // +1 if positive dx turns the same way as +yaw
bool  g_turn_sign_known = false;

// One measurement in flight: counts we asked for, the body yaw before, and how
// long we are prepared to wait for the game to respond.
struct PendingTurn {
    bool  active = false;
    float counts = 0.0f;
    float want_radians = 0.0f;
    float yaw_before = 0.0f;
    int   frames = 0;
};
PendingTurn g_pending;
constexpr int kTurnSettleFrames = 8;
float g_last_measured_ratio = 0.0f;

// ---------------------------------------------------------------------------
// BODY FOLLOWS GUN.
//
// The engine aims along its own camera; the weapon merely follows the
// controller visually. This loop closes that gap by steering the game's own
// aim onto the barrel, so the shot goes where the gun points - the honest
// version of "bullets follow the barrel", achieved without touching
// projectiles at all (BFVR does the same for BF1942).
//
// The error is simple because the game's aim is (0,0,1) in camera coordinates:
// the error IS the controller's direction expressed in that frame, and the
// body's world heading cancels out. See drawpolicy::controller_dir_in_body.
//
// Yaw only for now. Pitch needs the presentation offset to carry a pitch term
// as well, or steering the body vertically would tilt the player's view; that
// is the next increment, deliberately not bundled with this one.
bool  g_aim_on = false;
float g_aim_kp = 0.35f;              // proportional, no integral
float g_aim_deadzone = 0.010f;       // rad (~0.6 deg) - inside this, leave it alone
float g_aim_max_step = 0.12f;        // rad per emit; the response measured
                                     // non-linear above ~500 counts, so small
                                     // steps stay in the trustworthy region
float g_aim_max_error = 1.05f;       // rad (~60 deg): beyond this something is
                                     // out of sync - stop chasing, do not spin
float g_aim_residue = 0.0f;          // sub-count carry, or small errors would
                                     // round to zero and never converge
// One counter per reason the loop can decline. "Nothing happened and I cannot
// tell why" was costing a full relaunch per diagnosis, so every early return is
// named and counted, and the non-zero ones are logged periodically.
enum AimWhy {
    kAimDisabled = 0, kAimStaleSample, kAimTurnSettling, kAimNoHmd, kAimNoReference,
    kAimNoController, kAimPole, kAimErrorCap, kAimDeadzone, kAimZeroCounts,
    kAimNotForeground, kAimGatedOff, kAimWhyCount
};
const char* const kAimWhyName[kAimWhyCount] = {
    "disabled", "stale-sample", "turn-settling", "no-hmd", "no-reference",
    "no-controller", "pole", "error-cap", "deadzone", "zero-counts",
    "not-foreground", "gated-off"
};
unsigned g_aim_why[kAimWhyCount] = {};
unsigned g_aim_emits = 0;
// Why on_present() itself declined, which also suppresses aim.
unsigned g_gate_disabled = 0, g_gate_not_foreground = 0, g_gate_no_controllers = 0;
// Silent paths in the turn machinery. `no-response` means the injection never
// reached the game - the single most diagnostic failure in this subsystem.
unsigned g_turn_no_camera = 0, g_turn_busy = 0, g_turn_no_response = 0, g_turn_measured = 0;
float g_aim_last_error = 0.0f;
unsigned g_aim_last_seq = 0;         // one correction per FRESH controller sample
float g_stick_on = 0.35f, g_stick_off = 0.25f;
float g_trigger_threshold = 0.6f;

// Held state, so every press is edge-triggered and every hold is releasable.
struct Held {
    StickKeys keys = {};
    bool sprint = false, crouch = false, use = false, reload = false, jump = false, melee = false;
    bool fire = false, ads = false;
};
Held g_held;
aimpolicy::SnapState g_snap;
unsigned g_snaps = 0, g_frames_injected = 0;
bool g_foreground = false;
bool g_legacy_input_ok = false;

vrtrack::ControllerInput g_in[2];

// Index controllers report A as k_EButton_Grip(2) and B as
// k_EButton_ApplicationMenu(1) through legacy input; Touch-style controllers
// use k_EButton_A(7). Accept either so both work without a per-device table.
constexpr unsigned long long kMaskA = (1ull << 2) | (1ull << 7);
constexpr unsigned long long kMaskB = (1ull << 1);
constexpr unsigned long long kMaskStickClick = (1ull << 32);   // k_EButton_Axis0

bool pressed(int hand, unsigned long long mask)
{
    return g_in[hand].valid && (g_in[hand].buttons & mask) != 0;
}

// Edge-triggered key: only sends on a transition.
void hold_key(bool& state, bool want, WORD vk)
{
    if (state == want) return;
    state = want;
    osinput::send_key(vk, want);
}

void hold_mouse(bool& state, bool want, bool right)
{
    if (state == want) return;
    state = want;
    osinput::send_mouse_button(right, want);
}

void update_aim(int hand);   // defined below, used by on_present
bool turn_body(float radians);

int weapon_hand() { return g_swap_hands ? 0 : 1; }
int move_hand()   { return g_swap_hands ? 1 : 0; }

// Turn the body by `radians` through the game's own mouse look, and use the
// result to refine the counts-per-radian estimate.
bool turn_body(float radians)
{
    if (!std::isfinite(radians) || std::fabs(radians) < 1e-4f) return false;
    if (g_pending.active) { ++g_turn_busy; return false; }   // one measurement at a time

    float yaw = 0.0f, pitch = 0.0f;
    if (!camover::game_camera_angles(yaw, pitch)) { ++g_turn_no_camera; return false; }

    const float counts = radians * g_counts_per_rad * g_turn_sign;
    const int dx = static_cast<int>(counts >= 0.0f ? counts + 0.5f : counts - 0.5f);
    if (dx == 0) return false;

    g_pending.active = true;
    g_pending.counts = static_cast<float>(dx);
    g_pending.want_radians = radians;
    g_pending.yaw_before = yaw;
    g_pending.frames = 0;
    if (use_dinput()) inputbus::add_impulse(dx, 0);
    else              osinput::send_mouse_move(dx, 0);
    return true;
}

// Watch for the body to respond, then update the estimate.
void settle_turn()
{
    if (!g_pending.active) return;
    float yaw = 0.0f, pitch = 0.0f;
    if (!camover::game_camera_angles(yaw, pitch)) { g_pending.active = false; ++g_turn_no_camera; return; }

    ++g_pending.frames;
    const float moved = aimpolicy::body_delta(yaw, g_pending.yaw_before);
    const bool enough = std::fabs(moved) > 0.5f * std::fabs(g_pending.want_radians);
    if (!enough && g_pending.frames < kTurnSettleFrames) return;

    g_pending.active = false;
    if (std::fabs(moved) < 1e-3f) {
        // The camera did not move at all. Either the injected motion never
        // reached the game, or the player is in a menu/dead. Do not poison the
        // estimate with it - and count it, because "we asked and nothing
        // happened" is the most diagnostic failure this subsystem has.
        ++g_turn_no_response;
        return;
    }
    // Sign first: which way does a positive dx actually turn the body?
    if (!g_turn_sign_known) {
        const bool same = (moved > 0.0f) == (g_pending.want_radians > 0.0f);
        if (!same) g_turn_sign = -g_turn_sign;
        g_turn_sign_known = true;
        VRLOG("[turn] mouse dx sign resolved: %+.0f (asked %+.1f deg, body moved %+.1f deg)",
              g_turn_sign, g_pending.want_radians * 180.0f / aimpolicy::kPi,
              moved * 180.0f / aimpolicy::kPi);
    }
    const float ratio = std::fabs(g_pending.counts) / std::fabs(moved);
    if (!std::isfinite(ratio) || ratio < 50.0f || ratio > 100000.0f) return;
    g_last_measured_ratio = ratio;
    // Median-free EMA: this is a slowly varying quantity and the outliers are
    // already rejected above.
    g_counts_per_rad = g_gain_measured ? (g_counts_per_rad * 0.75f + ratio * 0.25f) : ratio;
    ++g_gain_samples;
    ++g_turn_measured;
    if (!g_gain_measured) {
        g_gain_measured = true;
        VRLOG("[turn] mouse gain measured: %.0f counts per radian (%.1f per degree)",
              g_counts_per_rad, g_counts_per_rad * aimpolicy::kPi / 180.0f);
    }
}

// The keys/buttons we want held this frame, as bus flags.
std::uint32_t g_want_keys = 0, g_want_buttons = 0;
// 'dik' test hold - proves the level path with no controller involved.
std::uint32_t g_test_key = 0;
DWORD g_test_until = 0;

void publish()
{
    inputbus::publish_levels(g_want_keys, g_want_buttons);
}

// Steer the game's aim toward where the controller points.
void update_aim(int hand)
{
    if (!g_aim_on) { ++g_aim_why[kAimDisabled]; return; }

    // One correction per fresh controller sample. Present runs faster than the
    // runtime produces poses, and without this every correction is emitted
    // twice - the single biggest source of oscillation in a loop like this.
    const unsigned seq = vrtrack::controller_sequence(hand);
    if (seq == g_aim_last_seq) { ++g_aim_why[kAimStaleSample]; return; }
    g_aim_last_seq = seq;

    // Never fight a turn that is still settling, or the measurement and the
    // correction end up chasing each other.
    if (g_pending.active) { ++g_aim_why[kAimTurnSettling]; return; }

    float err_yaw = 0.0f, err_pitch = 0.0f;
    int reason = 0;
    if (!camover::aim_error(err_yaw, err_pitch, reason)) {
        static const AimWhy map[5] = { kAimNoController, kAimNoHmd, kAimNoReference, kAimNoController, kAimPole };
        ++g_aim_why[map[(reason >= 0 && reason < 5) ? reason : 0]];
        return;
    }
    g_aim_last_error = err_yaw;

    if (std::fabs(err_yaw) > g_aim_max_error) { ++g_aim_why[kAimErrorCap]; return; }
    if (std::fabs(err_yaw) < g_aim_deadzone) { g_aim_residue = 0.0f; ++g_aim_why[kAimDeadzone]; return; }

    float step = err_yaw * g_aim_kp;
    step = (std::min)((std::max)(step, -g_aim_max_step), g_aim_max_step);

    // Counts, carrying the sub-count remainder so small errors still converge
    // instead of rounding to zero forever. Truncation toward zero is correct
    // here precisely because the remainder is carried.
    const float want = step * g_counts_per_rad * g_turn_sign + g_aim_residue;
    const int counts = static_cast<int>(want);
    g_aim_residue = want - static_cast<float>(counts);
    if (counts == 0) { ++g_aim_why[kAimZeroCounts]; return; }

    if (use_dinput()) inputbus::add_impulse(counts, 0);
    else if (osinput::game_is_foreground()) osinput::send_mouse_move(counts, 0);
    else { ++g_aim_why[kAimNotForeground]; return; }
    ++g_aim_emits;

    // Hold the view still. Compensate what was ACTUALLY EMITTED - the rounded
    // count converted back through the gain - not the pre-rounding step, or
    // the difference accumulates into a slow drift. And compensate what we
    // COMMANDED rather than what we observe, so it is cleanly attributable to
    // us: a snap turn or the player's own mouse, which must move the view, are
    // left alone.
    const float emitted = (g_counts_per_rad > 1.0f)
        ? static_cast<float>(counts) / (g_counts_per_rad * g_turn_sign) : 0.0f;
    camover::compensate_aim_turn(emitted);
}

} // namespace

void release_all()
{
    g_want_keys = 0; g_want_buttons = 0;
    publish();
    // Order does not matter, but nothing may be skipped: a key left down after
    // the mod stops driving it is the worst failure mode this module has.
    hold_key(g_held.keys.forward, false, 'W');
    hold_key(g_held.keys.back,    false, 'S');
    hold_key(g_held.keys.left,    false, 'A');
    hold_key(g_held.keys.right,   false, 'D');
    hold_key(g_held.sprint, false, VK_LSHIFT);
    hold_key(g_held.crouch, false, VK_LCONTROL);
    hold_key(g_held.use,    false, 'E');
    hold_key(g_held.reload, false, 'R');
    hold_key(g_held.jump,   false, VK_SPACE);
    hold_key(g_held.melee,  false, 'F');
    hold_mouse(g_held.fire, false, false);
    hold_mouse(g_held.ads,  false, true);
    g_snap.armed = true;
}

void on_present()
{
    // Read the controllers regardless of whether we are injecting, so 'status'
    // can show button and stick values while input is off - that is how the
    // bindings get verified without a headset.
    bool any_valid = false;
    for (int h = 0; h < 2; ++h) {
        any_valid |= vrtrack::controller_input(h, g_in[h]);
    }
    g_legacy_input_ok = any_valid;

    // The foreground rule: injecting into another window would type into
    // whatever the user is really doing. Never steal focus from here either -
    // this runs every frame.
    g_foreground = osinput::game_is_foreground();
    // The foreground rule exists because SendInput sprays into whatever window
    // is active. Injecting through DirectInput cannot reach another process, so
    // that path does not need it.
    const bool may_inject = use_dinput() ? true : g_foreground;

    // Diagnostics FIRST, and unconditionally: the summary used to live past
    // this gate, so it only ever logged when the loop was already running -
    // useless precisely when something is wrong. A whole play session produced
    // no [aim] lines because of that.
    static unsigned s_frames = 0;
    if ((++s_frames % 600) == 0) {
        char why[256] = {}; size_t off = 0;
        for (int i = 0; i < kAimWhyCount; ++i) {
            if (!g_aim_why[i]) continue;
            off += _snprintf_s(why + off, sizeof(why) - off, _TRUNCATE, " %s=%u", kAimWhyName[i], g_aim_why[i]);
        }
        VRLOG("[aim] enabled=%d emits=%u err=%+.1f deg | buttons(l=%d r=%d) fg=%d dinput=%d hook=%d |%s",
              g_aim_on ? 1 : 0, g_aim_emits, g_aim_last_error * 180.0f / aimpolicy::kPi,
              g_in[0].valid ? 1 : 0, g_in[1].valid ? 1 : 0, g_foreground ? 1 : 0,
              use_dinput() ? 1 : 0, dinput_live() ? 1 : 0, why[0] ? why : " (nothing declined)");
    }

    // The aim loop needs only the controller POSE, which is a different source
    // from the legacy button/stick read below. Gating it on that read meant a
    // controller whose buttons we cannot see silently disabled aiming even
    // though its pose was tracked perfectly - which is exactly what happened.
    if (g_enabled && may_inject) update_aim(weapon_hand());

    if (!g_enabled || !may_inject || !any_valid) {
        if (!g_enabled) ++g_gate_disabled;
        else if (!any_valid) ++g_gate_no_controllers;
        else ++g_gate_not_foreground;
        release_all();
        return;
    }
    ++g_frames_injected;

    const int mh = move_hand(), wh = weapon_hand();

    // What we want held this frame.
    const StickKeys want = aimpolicy::stick_to_keys(g_in[mh].stick[0], g_in[mh].stick[1],
                                                    g_held.keys, g_stick_on, g_stick_off);
    const bool w_sprint = pressed(mh, kMaskStickClick);
    const bool w_fire   = g_in[wh].trigger >= g_trigger_threshold;
    const bool w_ads    = g_in[mh].trigger >= g_trigger_threshold;
    const bool w_reload = g_in[wh].grip > 0.6f || pressed(wh, kMaskA);
    const bool w_jump   = pressed(wh, kMaskB);
    const bool w_crouch = pressed(mh, kMaskA);
    const bool w_use    = pressed(mh, kMaskB);
    const bool w_melee  = pressed(wh, kMaskStickClick);

    g_want_keys = 0;
    if (want.forward) g_want_keys |= inputbus::kW;
    if (want.back)    g_want_keys |= inputbus::kS;
    if (want.left)    g_want_keys |= inputbus::kA;
    if (want.right)   g_want_keys |= inputbus::kD;
    if (w_sprint)     g_want_keys |= inputbus::kShift;
    if (w_reload)     g_want_keys |= inputbus::kR;
    if (w_jump)       g_want_keys |= inputbus::kSpace;
    if (w_crouch)     g_want_keys |= inputbus::kCtrl;
    if (w_use)        g_want_keys |= inputbus::kE;
    if (w_melee)      g_want_keys |= inputbus::kF;
    g_want_buttons = (w_fire ? inputbus::kLeft : 0u) | (w_ads ? inputbus::kRight : 0u);
    if (g_test_key && GetTickCount() < g_test_until) g_want_keys |= g_test_key;
    else g_test_key = 0;
    publish();

    // The DirectInput path takes the levels straight off the bus, so the
    // SendInput edges must NOT also be sent - the game would see both.
    if (!use_dinput()) {
        hold_key(g_held.keys.forward, want.forward, 'W');
        hold_key(g_held.keys.back,    want.back,    'S');
        hold_key(g_held.keys.left,    want.left,    'A');
        hold_key(g_held.keys.right,   want.right,   'D');
        hold_key(g_held.sprint, w_sprint, VK_LSHIFT);
        hold_mouse(g_held.fire, w_fire, false);
        hold_mouse(g_held.ads,  w_ads,  true);
        hold_key(g_held.reload, w_reload, 'R');
        hold_key(g_held.jump,   w_jump,   VK_SPACE);
        hold_key(g_held.crouch, w_crouch, VK_LCONTROL);
        hold_key(g_held.use,    w_use,    'E');
        hold_key(g_held.melee,  w_melee,  'F');
    } else {
        g_held.keys = want;   // keep the hysteresis state coherent across a switch
    }

    // Snap turn: a real turn of the BODY through the game's own mouse look, so
    // the aim turns with it. The view follows for free, because the view is
    // built on the game camera.
    settle_turn();
    const int step = aimpolicy::snap_turn_step(g_in[wh].stick[0], g_snap);
    if (step != 0) {
        // Count only turns that were actually emitted: the counter used to rise
        // even when turn_body bailed, which made it lie about what happened.
        if (turn_body(static_cast<float>(step) * g_snap_degrees * aimpolicy::kPi / 180.0f)) ++g_snaps;
    }
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    char a1[32] = {};
    if (args) sscanf_s(args, "%31s", a1, static_cast<unsigned>(sizeof(a1)));
    const bool has1 = a1[0] != 0;

    if (!strcmp(cmd, "input")) {
        if (has1) {
            if (!_stricmp(a1, "on")) g_enabled = true;
            else if (!_stricmp(a1, "off")) { g_enabled = false; release_all(); }
            else if (!_stricmp(a1, "left")) g_swap_hands = true;
            else if (!_stricmp(a1, "right")) g_swap_hands = false;
        }
        _snprintf_s(reply, n, _TRUNCATE,
                    "input %s (%s-handed) foreground=%d legacy-input=%d snaps=%u",
                    g_enabled ? "on" : "off", g_swap_hands ? "left" : "right",
                    g_foreground ? 1 : 0, g_legacy_input_ok ? 1 : 0, g_snaps);
        return true;
    }
    if (!strcmp(cmd, "snap")) {
        if (has1) g_snap_degrees = (std::min)((std::max)(static_cast<float>(atof(a1)), 5.0f), 90.0f);
        _snprintf_s(reply, n, _TRUNCATE, "snap turn %.0f degrees", g_snap_degrees);
        return true;
    }
    if (!strcmp(cmd, "aim")) {
        if (has1) {
            if (!_stricmp(a1, "on")) { g_aim_on = true; g_aim_residue = 0.0f; }
            else if (!_stricmp(a1, "off")) { g_aim_on = false; g_aim_residue = 0.0f; }
            else if (!_stricmp(a1, "kp")) {
                char b2[16] = {}; if (args) sscanf_s(args, "%*s %15s", b2, static_cast<unsigned>(sizeof(b2)));
                if (b2[0]) g_aim_kp = (std::min)((std::max)(static_cast<float>(atof(b2)), 0.02f), 1.0f);
            } else if (!_stricmp(a1, "deadzone")) {
                char b2[16] = {}; if (args) sscanf_s(args, "%*s %15s", b2, static_cast<unsigned>(sizeof(b2)));
                if (b2[0]) g_aim_deadzone = (std::min)((std::max)(static_cast<float>(atof(b2)), 0.0f), 0.2f);
            }
        }
        // Report the top reason it declined, not a single opaque count.
        int top = -1; unsigned topn = 0;
        for (int i = 0; i < kAimWhyCount; ++i) if (g_aim_why[i] > topn) { topn = g_aim_why[i]; top = i; }
        _snprintf_s(reply, n, _TRUNCATE,
                    "aim %s kp=%.2f deadzone=%.1f deg error=%.1f deg emits=%u gain=%.0f top-block=%s(%u)",
                    g_aim_on ? "on" : "off", g_aim_kp, g_aim_deadzone * 180.0f / aimpolicy::kPi,
                    g_aim_last_error * 180.0f / aimpolicy::kPi, g_aim_emits, g_counts_per_rad,
                    top >= 0 ? kAimWhyName[top] : "none", topn);
        return true;
    }
    if (!strcmp(cmd, "path")) {
        if (has1) {
            if (!_stricmp(a1, "auto")) g_path = Path::Auto;
            else if (!_stricmp(a1, "dinput")) g_path = Path::DInput;
            else if (!_stricmp(a1, "sendinput")) g_path = Path::SendInput;
            else if (!_stricmp(a1, "off")) { g_path = Path::Off; release_all(); }
        }
        const auto* bus = inputbus::get();
        const char* names[] = { "auto", "dinput", "sendinput", "off" };
        _snprintf_s(reply, n, _TRUNCATE,
                    "path=%s using=%s hook=%s devices=0x%X polls=%u consumed=(%d,%d)",
                    names[static_cast<int>(g_path)], use_dinput() ? "dinput" : "sendinput",
                    dinput_live() ? "LIVE" : "absent",
                    bus ? bus->devices.load() : 0u, bus ? bus->polls.load() : 0u,
                    bus ? bus->consumed_dx.load() : 0, bus ? bus->consumed_dy.load() : 0);
        return true;
    }
    if (!strcmp(cmd, "dik")) {
        // Hold one key for N milliseconds with no VR in the loop - the level
        // path proven in isolation.
        char b1[16] = {}, b2[16] = {};
        if (args) sscanf_s(args, "%15s %15s", b1, static_cast<unsigned>(sizeof(b1)), b2, static_cast<unsigned>(sizeof(b2)));
        if (!b1[0]) { _snprintf_s(reply, n, _TRUNCATE, "usage: dik <w|a|s|d|space|r|e|f> <ms>"); return true; }
        struct { const char* nm; std::uint32_t f; } tbl[] = {
            {"w",inputbus::kW},{"a",inputbus::kA},{"s",inputbus::kS},{"d",inputbus::kD},
            {"space",inputbus::kSpace},{"r",inputbus::kR},{"e",inputbus::kE},{"f",inputbus::kF},
            {"shift",inputbus::kShift},{"ctrl",inputbus::kCtrl},
        };
        std::uint32_t flag = 0;
        for (const auto& t : tbl) if (!_stricmp(b1, t.nm)) flag = t.f;
        if (!flag) { _snprintf_s(reply, n, _TRUNCATE, "dik: unknown key '%s'", b1); return true; }
        g_test_key = flag;
        g_test_until = GetTickCount() + static_cast<DWORD>(b2[0] ? atoi(b2) : 600);
        _snprintf_s(reply, n, _TRUNCATE, "holding %s for %d ms via %s", b1, b2[0] ? atoi(b2) : 600,
                    use_dinput() ? "dinput" : "sendinput");
        return true;
    }
    if (!strcmp(cmd, "mouse")) {
        // Diagnostic: does injected relative mouse motion reach the game at
        // all? Emits a raw delta and reports how far the body actually turned.
        char b1[16] = {}, b2[16] = {};
        if (args) sscanf_s(args, "%15s %15s", b1, static_cast<unsigned>(sizeof(b1)), b2, static_cast<unsigned>(sizeof(b2)));
        float before = 0.0f, p = 0.0f;
        const bool had = camover::game_camera_angles(before, p);
        const int dx = b1[0] ? atoi(b1) : 200;
        const int dy = b2[0] ? atoi(b2) : 0;
        // An explicit one-shot diagnostic may bring the game forward itself -
        // that is the difference between focus_game() and the per-frame
        // game_is_foreground() gate. Without this the test can never run,
        // because whoever asks for it is by definition in another window.
        if (use_dinput()) {
            inputbus::add_impulse(dx, dy);
        } else {
            // An explicit one-shot diagnostic may bring the game forward
            // itself; the per-frame path never does.
            if (!osinput::focus_game()) { _snprintf_s(reply, n, _TRUNCATE, "mouse: could not focus the game - refusing to inject"); return true; }
            osinput::send_mouse_move(dx, dy);
        }
        _snprintf_s(reply, n, _TRUNCATE, "mouse: sent dx=%d dy=%d (body yaw before %.2f deg%s) - re-run 'status' to see the result",
                    dx, dy, before * 180.0f / aimpolicy::kPi, had ? "" : ", NO CAMERA");
        return true;
    }
    if (!strcmp(cmd, "gain")) {
        if (has1) {
            g_counts_per_rad = (std::min)((std::max)(static_cast<float>(atof(a1)), 50.0f), 100000.0f);
            g_gain_measured = true;
        }
        _snprintf_s(reply, n, _TRUNCATE, "mouse gain %.0f counts/rad (%.1f/deg) measured=%d samples=%u sign=%+.0f",
                    g_counts_per_rad, g_counts_per_rad * aimpolicy::kPi / 180.0f,
                    g_gain_measured ? 1 : 0, g_gain_samples, g_turn_sign);
        return true;
    }
    if (!strcmp(cmd, "deadzone")) {
        if (has1) {
            g_stick_on = (std::min)((std::max)(static_cast<float>(atof(a1)), 0.05f), 0.9f);
            g_stick_off = g_stick_on * 0.7f;
        }
        _snprintf_s(reply, n, _TRUNCATE, "stick deadzone on=%.2f off=%.2f", g_stick_on, g_stick_off);
        return true;
    }
    return false;
}

void status(FILE* f)
{
    {
        const auto* bus = inputbus::get();
        fprintf(f, "input path: %s (using %s) hook=%s devices=0x%X polls=%u consumed=(%d,%d) events=%u\n",
                g_path == Path::Auto ? "auto" : g_path == Path::DInput ? "dinput" : g_path == Path::SendInput ? "sendinput" : "off",
                use_dinput() ? "dinput" : "sendinput", dinput_live() ? "LIVE" : "absent",
                bus ? bus->devices.load() : 0u, bus ? bus->polls.load() : 0u,
                bus ? bus->consumed_dx.load() : 0, bus ? bus->consumed_dy.load() : 0,
                bus ? bus->injected_events.load() : 0u);
        if (bus) {
            fprintf(f, "  dinput: drains=%u injected=%u capped=%u levels-seen=0x%X published=0x%X\n",
                    bus->drain_calls.load(), bus->injected_events.load(), bus->capped.load(),
                    bus->levels_seen.load(), g_want_keys);
        }
    }
    fprintf(f, "input: %s %s-handed foreground=%d legacy=%d snaps=%u frames=%u\n",
            g_enabled ? "on" : "off", g_swap_hands ? "left" : "right",
            g_foreground ? 1 : 0, g_legacy_input_ok ? 1 : 0, g_snaps, g_frames_injected);
    for (int h = 0; h < 2; ++h) {
        fprintf(f, "  %s hand: valid=%d buttons=%08llX stick=(%+.2f %+.2f) trigger=%.2f grip=%.2f\n",
                h ? "right" : "left", g_in[h].valid ? 1 : 0,
                static_cast<unsigned long long>(g_in[h].buttons),
                g_in[h].stick[0], g_in[h].stick[1], g_in[h].trigger, g_in[h].grip);
    }
    {
        float yaw = 0.0f, pitch = 0.0f;
        const bool had = camover::game_camera_angles(yaw, pitch);
        fprintf(f, "  turn: gain=%.0f counts/rad (measured=%d, %u samples, last ratio %.0f) sign=%+.0f pending=%d  body yaw=%.1f pitch=%.1f deg%s\n",
                g_counts_per_rad, g_gain_measured ? 1 : 0, g_gain_samples, g_last_measured_ratio,
                g_turn_sign, g_pending.active ? 1 : 0,
                yaw * 180.0f / aimpolicy::kPi, pitch * 180.0f / aimpolicy::kPi, had ? "" : " (none)");
    }
    fprintf(f, "  aim: %s kp=%.2f deadzone=%.2f deg yaw-error=%+.1f deg emits=%u\n",
            g_aim_on ? "ON" : "off", g_aim_kp, g_aim_deadzone * 180.0f / aimpolicy::kPi,
            g_aim_last_error * 180.0f / aimpolicy::kPi, g_aim_emits);
    fprintf(f, "  aim why:");
    for (int i = 0; i < kAimWhyCount; ++i) if (g_aim_why[i]) fprintf(f, " %s=%u", kAimWhyName[i], g_aim_why[i]);
    fprintf(f, "\n");
    fprintf(f, "  gate: disabled=%u not-foreground=%u no-controllers=%u | turn: busy=%u no-camera=%u no-response=%u measured=%u\n",
            g_gate_disabled, g_gate_not_foreground, g_gate_no_controllers,
            g_turn_busy, g_turn_no_camera, g_turn_no_response, g_turn_measured);
    fprintf(f, "  held: w=%d a=%d s=%d d=%d sprint=%d crouch=%d fire=%d ads=%d reload=%d jump=%d\n",
            g_held.keys.forward, g_held.keys.left, g_held.keys.back, g_held.keys.right,
            g_held.sprint, g_held.crouch, g_held.fire, g_held.ads, g_held.reload, g_held.jump);
}

} // namespace vrinput
