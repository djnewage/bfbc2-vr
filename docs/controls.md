# Controls: playing from the motion controllers

Stage 1 of the input work (2026-08-27). The weapon already follows the right controller
(`docs/viewmodel-census.md` §7); this makes the controllers actually play the game.

## How input reaches the game, and why

BFVR writes gameplay input into BF1942's own normalized input frame and deliberately never
synthesises OS input for it. **We cannot**: no BC2 input structure is publicly known
(`docs/bc2-engine.md`), and the published static offsets are null in this build. So input goes
through `SendInput` (`src/os_input.cpp`), with two rules that are not optional:

1. **Nothing is injected unless the game window is the foreground window.** Checked every frame
   with a pure query that never steals focus. Otherwise keystrokes land in whatever the user is
   really doing.
2. **Everything held is released** on `input off`, on focus loss, and on DLL detach. A key left
   down after the mod stops driving it is the worst failure this module can produce.

Keys are sent as **scancodes**, not virtual keys — raw-input and DirectInput games read
scancodes and ignore VK-only injection.

## Bindings

`input on` to enable (off by default), `input left` for a left-handed mirror.

| Control | Action |
|---|---|
| Left stick | move (W/A/S/D, 0.35 on / 0.25 off hysteresis) |
| Left stick click | sprint |
| Left trigger | aim down sights |
| Left A / B | crouch / use |
| **Right stick ←/→** | **snap turn** (30°, one per flick) |
| Right trigger | fire |
| Right grip or A | reload |
| Right B | jump |
| Right stick click | melee |

Commands: `input on|off|left|right`, `snap <degrees>`, `deadzone <0..1>`. Every controller's
buttons, sticks, triggers and the currently held keys appear in `bfbc2vr_status.txt`, so
bindings can be verified from outside the headset.

## Snap turn moves the BODY (corrected 2026-08-27)

The first version rotated only the presented view, leaving the game's body facing its original
direction. In the headset the world turned — but the engine's aim never did, so **every shot
went to the same place regardless of which way the player faced**. A turn has to be a real turn.

Snap turn now drives the game's own mouse look, and the view follows for free because the view
is built on the game camera. That requires mouse counts per radian, which depends on the
player's in-game sensitivity and is not readable from the config, so it is **measured**: emit a
known delta, watch how far the body actually turned, low-pass the ratio. The direction a
positive `dx` turns is resolved the same way rather than assumed. Until the first measurement
lands a conservative bootstrap is used, so a wrong guess under-turns instead of spinning.

`gain` reports and overrides it; `mouse <dx> <dy>` is the raw diagnostic for the underlying
question — does injected relative motion reach this game at all.

## Superseded: snap turn as a view rotation

`camover::request_turn()` moves the HMD reference yaw, so the world turns under a stationary
body. Two reasons this matters:

- The head delta for the frame is already computed when input runs, so the reference and the
  delta are adjusted **together** (`adjust_view_reference`). Writing the reference alone would
  land the turn a frame late and shimmer.
- Stage 2 (body follows gun) adds a compensation term that removes the body's own yaw changes
  from the view. It is opposite-signed into the same accumulator — literally BFVR's
  `offset += requested − body` — so a deliberate turn can never be cancelled by the compensation
  that follows it. `tests/aim_policy_test.cpp` asserts the two have opposite signs.

## What is NOT done yet

**Bullets still follow the engine's aim, not the barrel.** Stage 2 is the aim convergence loop
that steers the game's own aim toward the controller direction. Design and risks are in the
approved plan; the pure math (`src/aim_policy.cpp`) and the view-reference writer already exist
and are tested.

## Body follows gun (Phase B, 2026-08-28)

The engine aims along its own camera; the weapon only follows the controller visually. This loop
closes that gap by steering the game's own aim onto the barrel, so the shot goes where the gun
points — the honest version of "bullets follow the barrel", achieved without touching
projectiles at all. BFVR does the same for BF1942.

**The error is trivial to compute, which is the nice part.** The game's aim is, by definition,
`(0,0,1)` in camera coordinates — so the error *is* the controller's direction expressed in that
frame, and the body's world heading cancels out entirely. No world handedness enters, which
removes the class of sign bug that has cost this project the most time
(`drawpolicy::controller_dir_in_body`).

Controller: proportional only, no integral. Deadzone ~0.6°, per-emit clamp of 0.12 rad (the
measured mouse response is mildly non-linear above ~500 counts, so small steps stay in the
trustworthy region), a 60° error cap that stops chasing rather than spinning, and a carried
sub-count remainder so small errors converge instead of rounding to zero forever.

Two rules that keep it from oscillating or fighting the player:

- **One correction per fresh controller sample.** Present runs faster than the runtime produces
  poses, so without a sequence check every correction is emitted twice — the single biggest
  source of oscillation in a loop like this.
- **Compensate what was commanded, not what is observed.** After emitting, the presentation
  absorbs exactly the rotation we asked for (the *rounded* count converted back through the
  gain, or the rounding difference accumulates into a drift). Because it is attributable to us,
  a snap turn and the player's own mouse — both of which *should* move the view — are untouched.

`aim on|off`, `aim kp <x>`, `aim deadzone <rad>`; the status block reports the live yaw error,
emit and blocked counts.

**Yaw only, for now.** Steering the body vertically would pitch the player's view unless the
presentation offset carries a pitch term too. That is the next increment and is deliberately not
bundled with this one — one behavioural change per test build.

### Fixing it: calibrate the pointing axis (2026-08-28)

The first version was erratic in the headset, and the counters said why: `error-cap=13511`,
a standing yaw error of **-102 deg**, and the view/body offset drifting to **-40 deg**. The loop
spent most of its time refusing to chase, then lunging.

**The error was wrong, not the controller.** `controller_dir_in_body()` took the controller pose's
forward axis and called it the barrel - but OpenVR's legacy pose is the **grip** pose, whose
forward runs along the handle, not the muzzle. That is exactly the distinction OpenXR draws
between `aim` and `grip`, and why BFVR uses grip for holding the weapon and aim for pointing. Held
naturally, the grip axis sits tens of degrees off where the player believes they are pointing.

Rather than hardcode a grip-to-aim rotation (device-specific, and legacy input gives us no aim
pose), the loop now **captures a reference direction** and measures deviation from it. Whatever
axis the runtime's pose uses, the reference is captured in the same axis, so only *changes* matter
and no device constant is needed - the same shape as the grip calibration that already works.
A unit test asserts that a pure roll of the controller produces no yaw error, because a wrist
twist must never steer the player's body.

Also changed:

- **The offset is bounded and bleeds back.** The view may lead the body by at most 25 deg, and
  drifts back toward alignment whenever the loop is idle. Unbounded, it became a permanent lie
  about where the body faces.
- **The error cap re-baselines** instead of refusing forever, which is what produced 13511 dead
  samples.
- Softer defaults: `kp` 0.35 -> 0.20, deadzone 0.6 -> 1.5 deg so hand tremor does not drag the body.
- `aim mode firing` converges only while the trigger is pulled, if continuous still feels twitchy.
  `aim recal` re-captures the reference; `recenter` does too.
