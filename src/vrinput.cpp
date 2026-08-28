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

int weapon_hand() { return g_swap_hands ? 0 : 1; }
int move_hand()   { return g_swap_hands ? 1 : 0; }

// Turn the body by `radians` through the game's own mouse look, and use the
// result to refine the counts-per-radian estimate.
void turn_body(float radians)
{
    if (!std::isfinite(radians) || std::fabs(radians) < 1e-4f) return;
    if (g_pending.active) return;               // one measurement at a time

    float yaw = 0.0f, pitch = 0.0f;
    if (!camover::game_camera_angles(yaw, pitch)) return;

    const float counts = radians * g_counts_per_rad * g_turn_sign;
    const int dx = static_cast<int>(counts >= 0.0f ? counts + 0.5f : counts - 0.5f);
    if (dx == 0) return;

    g_pending.active = true;
    g_pending.counts = static_cast<float>(dx);
    g_pending.want_radians = radians;
    g_pending.yaw_before = yaw;
    g_pending.frames = 0;
    if (use_dinput()) inputbus::add_impulse(dx, 0);
    else              osinput::send_mouse_move(dx, 0);
}

// Watch for the body to respond, then update the estimate.
void settle_turn()
{
    if (!g_pending.active) return;
    float yaw = 0.0f, pitch = 0.0f;
    if (!camover::game_camera_angles(yaw, pitch)) { g_pending.active = false; return; }

    ++g_pending.frames;
    const float moved = aimpolicy::body_delta(yaw, g_pending.yaw_before);
    const bool enough = std::fabs(moved) > 0.5f * std::fabs(g_pending.want_radians);
    if (!enough && g_pending.frames < kTurnSettleFrames) return;

    g_pending.active = false;
    if (std::fabs(moved) < 1e-3f) {
        // The camera did not move at all. Either the injected motion never
        // reached the game, or the player is in a menu/dead. Do not poison the
        // estimate with it.
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
    if (!g_enabled || !may_inject || !any_valid) {
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
        turn_body(static_cast<float>(step) * g_snap_degrees * aimpolicy::kPi / 180.0f);
        ++g_snaps;
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
    fprintf(f, "  held: w=%d a=%d s=%d d=%d sprint=%d crouch=%d fire=%d ads=%d reload=%d jump=%d\n",
            g_held.keys.forward, g_held.keys.left, g_held.keys.back, g_held.keys.right,
            g_held.sprint, g_held.crouch, g_held.fire, g_held.ads, g_held.reload, g_held.jump);
}

} // namespace vrinput
