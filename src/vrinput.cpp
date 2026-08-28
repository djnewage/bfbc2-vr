#include "vrinput.h"
#include "aim_policy.h"
#include "os_input.h"
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

bool  g_enabled = false;          // opt-in: 'input on'
bool  g_swap_hands = false;       // left-handed mirror
float g_snap_degrees = 30.0f;
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

} // namespace

void release_all()
{
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
    if (!g_enabled || !g_foreground || !any_valid) {
        release_all();
        return;
    }
    ++g_frames_injected;

    const int mh = move_hand(), wh = weapon_hand();

    // Movement.
    const StickKeys want = aimpolicy::stick_to_keys(g_in[mh].stick[0], g_in[mh].stick[1],
                                                    g_held.keys, g_stick_on, g_stick_off);
    hold_key(g_held.keys.forward, want.forward, 'W');
    hold_key(g_held.keys.back,    want.back,    'S');
    hold_key(g_held.keys.left,    want.left,    'A');
    hold_key(g_held.keys.right,   want.right,   'D');
    hold_key(g_held.sprint, pressed(mh, kMaskStickClick), VK_LSHIFT);

    // Actions.
    hold_mouse(g_held.fire, g_in[wh].trigger >= g_trigger_threshold, false);
    hold_mouse(g_held.ads,  g_in[mh].trigger >= g_trigger_threshold, true);
    hold_key(g_held.reload, g_in[wh].grip > 0.6f || pressed(wh, kMaskA), 'R');
    hold_key(g_held.jump,   pressed(wh, kMaskB), VK_SPACE);
    hold_key(g_held.crouch, pressed(mh, kMaskA), VK_LCONTROL);
    hold_key(g_held.use,    pressed(mh, kMaskB), 'E');
    hold_key(g_held.melee,  pressed(wh, kMaskStickClick), 'F');

    // Snap turn. This is a VIEW rotation, not a mouse turn: the reference yaw
    // moves, so the world turns under a stationary body. When the aim loop
    // lands (stage 2) it will walk the body around to follow the gun, and its
    // compensation term is opposite-signed into this same accumulator, so the
    // two can never cancel each other.
    const int step = aimpolicy::snap_turn_step(g_in[wh].stick[0], g_snap);
    if (step != 0) {
        const float requested = static_cast<float>(step) * g_snap_degrees * aimpolicy::kPi / 180.0f;
        camover::request_turn(requested);
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
    fprintf(f, "input: %s %s-handed foreground=%d legacy=%d snaps=%u frames=%u\n",
            g_enabled ? "on" : "off", g_swap_hands ? "left" : "right",
            g_foreground ? 1 : 0, g_legacy_input_ok ? 1 : 0, g_snaps, g_frames_injected);
    for (int h = 0; h < 2; ++h) {
        fprintf(f, "  %s hand: valid=%d buttons=%08llX stick=(%+.2f %+.2f) trigger=%.2f grip=%.2f\n",
                h ? "right" : "left", g_in[h].valid ? 1 : 0,
                static_cast<unsigned long long>(g_in[h].buttons),
                g_in[h].stick[0], g_in[h].stick[1], g_in[h].trigger, g_in[h].grip);
    }
    fprintf(f, "  held: w=%d a=%d s=%d d=%d sprint=%d crouch=%d fire=%d ads=%d reload=%d jump=%d\n",
            g_held.keys.forward, g_held.keys.left, g_held.keys.back, g_held.keys.right,
            g_held.sprint, g_held.crouch, g_held.fire, g_held.ads, g_held.reload, g_held.jump);
}

} // namespace vrinput
