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

### The stick was never on the axis we were reading

`thirdparty/openvr/openvr.h` defines `k_EButton_SteamVR_Touchpad = k_EButton_Axis0` (32) but
`k_EButton_IndexController_JoyStick = k_EButton_Axis3` (35). The code read `rAxis[0]` as the stick
and tested bit 32 as its click; **`rAxis[3]` and bit 35 were never read anywhere**. On Index that
means bit 32 is the **trackpad**, which a thumb resting on or sliding across asserts with no
intent — so nudging the stick sideways sent the melee key every time.

`vr_tracking.h` had stated the assumption as fact ("stick is axis 0 ... on both Index and
Touch-style controllers"); the bundled header contradicts it. The axis and click bit are now chosen
per device from `Prop_ControllerType_String`, and **every raw axis and the raw button mask are
printed in the status file** so the choice is checkable rather than assumed. Wiggle one control at
a time and read off which numbers move.

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
