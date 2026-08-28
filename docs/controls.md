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
