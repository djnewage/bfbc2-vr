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
| Right stick click (stick centred) | melee — key configurable, see below |

Commands: `input on|off|left|right`, `snap <degrees>`, `deadzone <0..1>`,
`key <action> <char>` (melee, reload, use, jump, crouch, sprint; bare `key` lists them). Every controller's
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

### The pole: aiming vertically spun the body

With the reference capture in, pointing the gun up or down made the body spin uncontrollably.

Yaw is not equally meaningful in every direction. Turning the body by a yaw error `d` displaces a
pointing direction horizontally by roughly `h * d`, where `h` is the direction's horizontal
component. Near vertical `h` collapses, `atan2(x, z)` becomes hypersensitive - at 80 deg of pitch a
centimetre of hand wobble swings the reading by tens of degrees - and the loop faithfully turned
the body that far.

The guard that was supposed to catch this refused only below `h < 1e-3`, which is **within 0.06
degrees of straight up**. Every realistic steep aim sailed past it and was chased at full gain.

`aim_deviation` now also returns an **authority**: 1 when pointing level, ramping linearly to 0
between ~50 and ~81 degrees of pitch, and refusing beyond. The yaw error is scaled by it. That
scaling is not a fudge factor - it *is* the horizontal projection above - and the ramp means the
body keeps following a steepening aim progressively more gently rather than cutting out mid-gesture.
The reference direction is checked the same way, since a reference captured while pointing at the
sky has no usable yaw either.

Tests pin the property that matters: the same yaw deviation commands a strictly smaller turn at 65
deg of pitch than level, authority falls off monotonically with no cliff, and a steep aim is
refused outright.

`authority=` now appears in the status block, because "the loop is idle" and "the loop is declining
because you are pointing at the sky" were indistinguishable from the counters alone.

### One variable doing three jobs: why left/right was inconsistent

Aiming left and right worked, then didn't, with no obvious pattern. `g_turn_yaw` was carrying
three different quantities:

1. `request_turn` (snap turn) did `g_turn_yaw += radians` - a deliberate body turn, and the view is
   **meant** to follow it.
2. `compensate_aim_turn` did `g_turn_yaw -= emitted` - the aim loop turned the body, and the view
   must **not** follow.
3. `controller_dir_in_body` read it as "how far the body has rotated since the reference", to map
   the controller into the body frame and compute the aim error.

Jobs 1 and 2 move the number in **opposite directions for the same physical event** (the body
rotating), because they differ in whether the view should follow. So as a measure of body rotation
- job 3 - it was simply wrong: snap turn left, then aim left, and the mapping was off by twice the
snap. The clamp and the idle bleed added in the previous two commits then corrupted it further,
each nudging a value the error measurement depended on.

Split into two:

- `g_turn_yaw` - deliberate turns only. Never clamped, never bled.
- `g_aim_view_offset` - the aim loop's view-hold offset. Clamped to 25 deg and bled when idle.
- `view_turn_yaw()` = their sum, which is what every view-side call site uses.

(The clamp was also actively dangerous while shared: it bounded the *total*, so after a few snap
turns the first aim correction would have yanked the view back to 25 deg.)

And job 3 no longer accumulates anything. The body's rotation since the reference is now read from
the **game camera**, which is ground truth and already reflects snap turns, the aim loop's own
turns, the player's mouse, and anything the game does to the heading. Accumulating meant every turn
the loop did not personally emit went missing from the mapping.

## The right stick was firing input nobody asked for (2026-08-28)

Two reports: *"when I use the right thumbstick it throws a grenade"* — on pushing it **sideways**,
without clicking — and *"when I aim up I snap turn left and right and the screen shakes"*, as
**discrete ~30° jumps**, i.e. real snap turns firing repeatedly.

### Deflecting the stick asserts the stick-click bit

SteamVR's own legacy binding for the Index — `drivers/indexcontroller/resources/input/`
`legacy_bindings_index_controller.json` — maps the thumbstick **twice**. Once as a joystick:

```json
{ "mode": "joystick", "path": "/user/hand/right/input/thumbstick",
  "inputs": { "position": { "output": ".../right_axis0_value" },
              "click":    { "output": ".../right_axis0_press" } } }
```

and again as a threshold button on the *same* output:

```json
{ "mode": "button", "type": "boolean_threshold",
  "path": "/user/hand/right/input/thumbstick",
  "inputs": { "click": { "output": ".../right_axis0_press" } },
  "parameters": { "force_input": "position",
                  "click_activate_threshold": 0.8, "click_deactivate_threshold": 0.7 } }
```

So **pushing the stick past 0.8 in any direction sets bit 32** — the same bit read as "stick
click". That is the grenade: sideways, no click. The trackpad shares that bit too, but it is the
threshold rule that matches the report.

The fix is to require the stick near centre (`|stick| < 0.4`) before a click counts. The 0.8/0.7
hysteresis above is what makes `0.4` a derived threshold rather than a guessed one.

#### A wrong turn taken first (2026-09-01)

This was initially diagnosed from `thirdparty/openvr/openvr.h`, which defines
`k_EButton_IndexController_JoyStick = k_EButton_Axis3` (35) against
`k_EButton_SteamVR_Touchpad = k_EButton_Axis0` (32), and the reader was switched to `rAxis[3]` /
bit 35 for Index. **That was wrong and it broke the stick completely.** Those constants describe
the *modern* input system's device layout; legacy emulation deliberately collapses the thumbstick
back onto axis 0, which is the entire purpose of a legacy binding. SteamVR never populates
`rAxis[3]` in this mode, so the stick read a flat zero, no snap turn ever fired, and — because the
aim loop was at that point gated on a snap turn having resolved the mouse sign — **turning stopped
working altogether**.

The evidence that settled it: with the game in the foreground and the loop being serviced at 120
frames/second, all five axes on both hands read `+0.00` across a 45-second capture, including
`axis1` (trigger) and `axis2` (grip) while they were being pressed.

Legacy input puts the primary 2D control on **axis 0, click bit 32, for every controller type**;
the per-device branch was the wrong shape as well as the wrong value, and is gone. The device type
is still read and logged, and **every raw axis and the raw button mask are printed in the status
file**, which is what made the mistake visible.

### Two paths re-armed the snap latch with the stick untouched

- `release_all()` set `g_snap.armed = true`, and it runs on **every frame the input gate fails**.
  One flickering controller read therefore re-armed the latch while the stick was still held over,
  and the next good frame fired another turn.
- `controller_input()` cleared its output struct *before* the validity check, so a refused read
  wrote `stick[0] = 0` — which reads as "returned to centre" and re-arms too.

Raising the controller is exactly when tracking flickers, so a thumb resting past the fire
threshold produced a burst of turns in whichever direction it sat. Both are fixed: `release_all`
no longer re-arms, a failed read leaves the caller's values alone, and the latch now **starts
disarmed** — it must see a genuine at-rest reading before it will fire at all.

### The rest of the gates

- A turn must be a **sideways** flick (`|x| > |y|`): a thumb sliding as the arm rises is not one.
- Fire threshold `0.65 → 0.80`, re-arm `0.35 → 0.25` — a wider dead band cannot chatter.
- A **250 ms cooldown**, so even a fooled gate cannot produce a burst.
- A stick **click** only counts when the stick is near centre (`< 0.4`), and melee is now a **tap**
  rather than a hold, so a stuck false press cannot repeat the action. Refused clicks are counted
  in the status block — a large number there is direct evidence the click bit is not a click.

Snap turn deliberately does **not** dedup on the controller pose sequence the way the aim loop
does: axis and button state comes from a separate `GetControllerState` poll, so pose freshness says
nothing about it, and gating on it would make a deliberate flick do nothing while the player held
the controller still.

### The keys are configurable now

The mod's entire key set is `W A S D Space Ctrl Shift R E F`, and the scancode path is correct, so
the "melee" binding really was sending **F** — meaning F is the grenade key in this install. BC2
keeps its bindings inside `Documents/BFBC2/GameSettings.bin` as an opaque blob (`settings.ini` is
video and audio only), so the mod cannot read them. Rather than swap one guess for another,
`key <action> <char>` sets them and the choice persists in `bfbc2vr.cfg`.

## Why the aim loop shook on its own

The stick fixes above account for the discrete 30° jumps, but the *shake* had a separate cause: the
loop's gain was around 1.

`compensate_aim_turn` moves the view in the **same instruction stream**, and `rebuild_correction`
consumes it three lines later — but the injected mouse counts reach the game on its next input
tick and only appear in the camera **two or more frames later**, smeared by the game's own mouse
smoothing (the measured 334-406 counts/radian spread is that smoothing). Nothing recorded what had
already been asked for, so the loop re-emitted `kp * err` every frame against an error that had not
responded yet. Total commanded rotation was `N * kp * err`, not `kp * err`. With `kp = 0.20` and
2-4 frames of latency that is a loop gain at or above 1 — overshoot, then reverse, forever.

The "one correction per fresh sample" gate did not throttle it: the pose sequence increments on
exact float inequality, and tracking noise makes that true every frame.

**Fix**: track the yaw commanded but not yet observed, and act on `err − in_flight`. It retires as
the camera's own heading moves — which correctly counts the player's own mouse too, since the error
is measured from the camera. The retire can only shrink the debt (a player turning against us would
otherwise inflate it without bound), and it decays anyway so a menu or a dead player cannot wedge
the loop shut. `in-flight=` is in the status block, and a new `waiting-on-in-flight` counter
separates "the gun is on target" from "we already asked and are waiting".

### Three more things that fed it

- **The gain estimator accepted a measurement at HALF the requested rotation**, so
  `ratio = counts / moved` could be **twice** the true gain. An over-estimated gain emits twice the
  counts intended while compensating the view for only what was intended — the view swings while
  the code believes it is holding still. It now waits for 90% of the rotation, and folds the first
  sample in rather than replacing the 360 bootstrap wholesale.
- **`g_turn_sign` was unverified until a snap turn happened.** If it is wrong the loop drives the
  body *away* from the target: positive feedback, not a wobble, until the 60° cap trips and the
  reference re-baselines — then again. The aim loop now waits for the sign to be a measured fact,
  and says so in the status block.
- **The error cap re-baselined on the first sample over it**, silently redefining "aligned"
  mid-gesture. Between 50° and 81° of elevation `atan2` has `1/h` sensitivity and flips sign as the
  muzzle crosses the vertical plane, so noise alone tripped it repeatedly. It now requires the cap
  to persist, and refuses to re-baseline on a reading whose yaw authority is already low.

### And one that could inject a spike from nowhere

`g_cam_rows` holds whichever camera wrote c189 **last** in a frame — shadow, reflection and scope
passes all write it. A frame where one of those went last reports a heading unrelated to the
player, which the loop would turn the body to chase. A player cannot rotate 90° between two
Presents, so a jump that large is now refused and counted as `cam-yaw-rejected`.

Separately, the view offset now bleeds toward zero on **every** idle path. It previously bled only
on the deadzone and not-firing paths, so it froze wherever it happened to be whenever the muzzle
crossed 81°, the cap tripped, or the sample was stale.

### A cross-wire found on the way

`g_grip_hand` was hardcoded to the right hand and settable only by the separate `grip left|right`
command, while `input left` flipped `g_swap_hands`. Since `update_aim`'s hand parameter never
reaches the error computation — `controller_dir_now` always reads `g_grip_hand` — the swapped
configuration dedup'd and gated on one controller while measuring the other. `input left|right`
now moves the weapon hand with it, and invalidates both calibrations when it does.

## The aim loop deadlocked waiting for a snap turn (2026-09-01)

The mouse-dx sign — which way a positive `dx` actually turns the body — has to be a measured fact,
because a wrong guess drives the body *away* from the target: positive feedback, not a wobble. The
loop was therefore gated on `g_turn_sign_known`, which only `settle_turn()` sets, and which only
runs when a turn is pending, and the only thing that requested a turn was a **snap turn**.

Snap turn needs the stick. While the stick was misread (above), the sign was never resolved, so the
aim loop declined *every frame for an entire session*:

```
[aim] enabled=1 emits=0 err=+0.0 deg | buttons(l=1 r=1) fg=1 dinput=1 hook=1 |
      disabled=1 stale-sample=1782 turn-sign-unknown=12240
```

`turn-sign-unknown` climbing by 600 per log line, `emits=0`. Two independent bugs, but the gate is
what turned a broken stick into *no turning at all* — a dependency on a user action that could
itself be broken is a deadlock, however well-founded the caution behind it.

The sign is now resolved by the aim loop's own motion: when it is unknown and there is a real error
to act on, the loop emits one bounded 3° probe through the same `turn_body()` path a snap turn uses,
and `settle_turn()` reads the answer off the camera — no second observer. A wrong guess costs one
3° nudge the wrong way, once. The probe is deliberately not view-compensated: it is a real body
turn, so the view moves with it. One small jolt at startup is the honest price of not guessing.

The status line reads `turn-sign=UNKNOWN (probing)` rather than `(aim held off)`.

## The auto FOV hunt collapsed the world frustum (2026-09-01)

> **Correction, same day.** The headline of this section is **wrong**. The hunt did not cause the
> collapse. A later launch with auto-location removed - `bfbc2vr.log` line 582 confirms the hunt
> never started - showed the tangents fall `0.5536 -> 0.3608 -> 0.1630` anyway. The real cause is
> below, under *"Whoever wrote c185 last"*. Everything this section says about the **consequences**
> of a collapsed frustum still holds, and the guardrails it added are what turned the next failure
> from an unusable black box into a merely wrong-scaled image. The hunt is still opt-in, which is
> right on its own merits.

Reported in-headset: **no gun visible at all**, the view **"a small black box instead of 3d"**, and
shooting not following the barrel. The desktop window looked fine. All of it was one fault.

`bfbc2vr.log`, the bad run - four hits, each measured against a baseline that had already shrunk:

```
HIT 10152BC4  orig=45.0994:  tangents 0.5536/0.4152 -> 0.4349/0.3262
HIT 1027CD48  orig=45.0814:  tangents 0.4158/0.3118 -> 0.3405/0.2554
HIT 1027E164  orig=45.0813:  tangents 0.3238/0.2429 -> 0.2513/0.1885
HIT 102DAAD8  orig=45.1000:  tangents 0.2332/0.1749 -> 0.1630/0.1223
```

The launch before found only two and they happened to cancel (`0.5536 -> 0.7487 -> 0.5536`), which
is why this worked earlier the same session. It is nondeterministic.

```
healthy:  world tan=1.8842/1.4131  (deg 124.1 x 109.4)   VIEWMODEL writes=11  hidden=3/1
broken:   world tan=0.1630/0.1223  (deg  18.5 x  13.9)   VIEWMODEL writes=0   hidden=0/1
```

### One wrong tangent, four symptoms

- **The black box.** `VRTextureBounds_t` maps frustum tangents onto UVs as `u = (l + tg) / (2*tg)`,
  with `l,r,t,b` from `GetProjectionRaw`. The span is the headset's field measured *in units of the
  game's*, so a game frustum far narrower than the headset's makes it explode: at `tg = 0.163` the
  span reached about 8, putting roughly an eighth of the panel width on screen and black everywhere
  else. At the healthy `1.8842` it is a sane `u = [0.13, 0.87]`.
- **No gun.** At that crop the weapon, which sits low in frame, is outside the visible box.
- **The gun ignored the controller.** `classify()` keys on the weapon's projection *differing* from
  the world's; the near planes match, so `DepthDiffers` is false and `own_fov` is the whole test.
  World 18.5 deg against weapon 18.6 deg compares `Same`, so it fell back to the +/-0.6 m distance
  test that almost nothing passes - hence `VIEWMODEL writes=0`, no grip transform, and no arms-hide
  either, since that requires `cls == Viewmodel`.
- **"Instead of 3d."** At an eighth of the panel the stereo disparity is a tiny fraction of the
  image, so it reads flat.

### Why it kept happening, and what changed

The hunt started itself, roughly ten seconds after the view stabilised, retrying up to three times:

```cpp
g_fov_stable_frames > 600 && g_fovfind_attempts < 3 && (g_fov_stable_frames % 1800) == 601
```

It pokes thousands of heap floats and writes each back afterwards. When a restore does not take, the
projection is left wrong and nothing says so. **Auto-location is now removed** - `fovfind` and
`fovhunt` are the only entry points, and a one-line log at startup says so.

Three guardrails, each useful on its own:

- **A candidate that narrows the view is rejected.** Every encoding in `kEnc` is built so a correct
  address *widens*: `deg/rad/tan` are poked x1.3, and `proj_a`/`proj_b` hold `1/tan` so they are
  poked x(1/1.3). The tangent must go **up**. All four hits above went down, and accepting them is
  precisely how the world collapsed.
- **Every restore is verified** by reading back, with failures counted and named in the log and a
  loud warning at the end of a hunt. `fov restore` puts back every address a hunt has touched -
  hits, candidates, and the selected address - not just the one `fov off` handles.
- **The compositor refuses insane bounds.** Beyond a UV span of 2.0 (half the panel as border) it
  submits the full frame instead and logs why. A wrong FOV should cost the wrong scale, never the
  whole image. `bounds-refused=` is in the status block.

The recovered world projection is also gated now, the same way camera yaw already was: a
single-frame collapse of more than 2x is refused and the last believable value held. The test is on
the *rate*, not the value, so a genuine scope still comes through - and it gives up after 10
consecutive refusals, because latching onto whichever projection was seen first is a worse failure
than the one being guarded. `world-proj-rejected=` is in the status block.

## "Whoever wrote c185 last" was never a camera (2026-09-01)

Reported: **unable to snap or smooth turn**, and the view collapsing to a tiny letterboxed image
while just walking around - confirmed **not** scoped. Two symptoms, one fault.

`c185..c192` is one contiguous constant block holding **both** the view-projection (`c185..c188`)
and the camera-to-world transform (`c189..c192`), and every pass that renders geometry writes it -
shadow cascades, reflections, the main scene. The code latched both, unconditionally, at write time.
When a foreign pass wrote last it took **both at once**:

- `g_world_proj` collapsed to 18.5 degrees - the visible FOV bug, the black box, and the reason the
  weapon stopped being classifiable (the classifier keys on the weapon field *differing* from the
  world's, and 18.5 against 18.6 compares the same).
- `g_cam_rows` took that camera's heading in the same instant.

### The gate that could not let go

`game_camera_angles()` refuses a heading that jumps more than 90 degrees between Presents, because a
player cannot turn that fast and a jump that large is someone else's camera. But `s_last_yaw` only
advanced on the **accept** path. Once a foreign camera took the slot, its heading was compared
against a stale player yaw forever and could never be accepted again.

It rejected **15472 of 15960 frames - 97 percent**. And both turning mechanisms bail when it returns
false:

- `controller_dir_now()` returns false -> `aim_error` reason 3 -> `no-controller=10601`, `emits=0`.
  **Smooth turn dead.**
- `turn_body()` returns false at the same call -> `no-camera=11`, `snaps=0`. **Snap turn dead.**

The counters reconcile exactly: `6630 + 2319 + 7011 = 15960` frames, and
`1 + 3037 + 10601 + 2 = 13641` aim-why events.

**The input layer was never at fault.** `snap_turn_step` fired 11 times - the stick, the `armed`
latch, the 0.80 threshold, the `|x| > |y|` test and the cooldown all behaved correctly, and every
one of those turns was thrown away downstream. `melee clicks refused = 314` independently proves the
stick delivers real deflection. Two rounds of input fixes were chasing a symptom.

### What changed

- **Every gate now has an escape.** A baseline that only advances on the accept path is the bug
  pattern; `game_camera_angles` gets the same persist-and-accept hatch `g_world_proj` already had.
- **The camera is chosen, not inherited.** Writes to `c185..c192` are collected as candidates for
  the frame, each weighted by how many writes used it, and `promote_player_camera()` picks one at
  end of frame - before anything reads it. Scoring is a pure function in `draw_policy.cpp`
  (`camera_is_plausible`, `choose_player_camera`) so it is unit-tested like the rest of that file:
  perspective only, field between 30 and 150 degrees, most-used candidate wins, and last frame's
  camera breaks ties so a steady view does not alternate between passes. If nothing is plausible the
  previous camera is held rather than a known-bad one adopted.
- **An aspect test exists in the API but is passed 0.** It cannot separate this failure: the
  collapsed frustum is `0.1630/0.1223 = 1.333`, the same 4:3 as the healthy `0.5536/0.4152`. The
  field-of-view range is what does the work. A unit test pins that fact down so nobody re-adds it
  believing it helps.

`cam-candidates=` and `promote-failed=` are in the status block. `cam-yaw-rejected` near zero is the
signal that this is healthy; ~97 percent of frames is what the failure looked like.

## Firing mode: absorb the swing, then bring the body back (2026-09-02)

`aim mode firing` is the traditional-VR-shooter layout Tristan asked for: the stick turns the
body, the gun aims freely, and only while the trigger is held does the body swing to line the
camera up with the barrel (rounds still leave the camera until shots spawn from the barrel).

Two reports followed: *"my head snaps to a different position"* when firing, and a slow drift
afterwards. One cause. The view-hold that keeps the world still while the body swings was capped at
25°, tuned for continuous mode where the error is never more than a degree or two. In firing mode
the trigger is pulled with the gun 40–70° off the body; the cap absorbed 25° and the rest appeared
as a view jump, then bled back at 0.02°/frame as the drift.

- The cap is now **per mode**: 25° continuous, 120° firing, so the whole swing is absorbed.
- On trigger release the loop enters a **return** phase: it commands the body back by exactly the
  held offset, compensated, so the offset walks to zero while the presented view does not move. The
  body ends up under the head. No bleed of the view, no drift. A new trigger pull cancels the
  return and the aim error takes over from wherever the body is.
- The offset is decremented at *command* time, so it already excludes what is in flight; the return
  uses it directly and cannot reintroduce the loop-gain oscillation the in-flight accounting fixed.

`returning=` and `returns=` are in the status block.
