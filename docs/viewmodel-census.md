# Viewmodel: census, classifier, and the corrected-projection path

2026-08-20. Follows the BFVR study (`prior-art-bfvr.md`). Three things changed in
one build, in dependency order:

## 0. FOV widening was a no-op (bug, fixed)

Since `0afea3b` the widen matrix `K = V·S·M_cw` was multiplied into `rot` *after* `r`
had already been built from `rot`; `g_correction` is built from `r`, so `K` never
reached it. The game kept rendering its native ~69°×45° field while
`game_proj_tangents()` told the compositor the image was 2.3×/3.4× wider, and the
per-eye texture bounds simply **magnified** the native image to fill the eye. That
is a large part of "the gun is in my face", and it also means every in-headset
judgement since 0afea3b was made on a 2–3× zoomed picture.

Now `K` is applied last, in the projecting camera's frame (after head rotation,
6DOF, and eye shift). **Expect the first run to look different**: a genuinely
wider field, softer (same backbuffer, more degrees), more edge pop-in (the engine
still culls at its native frustum), the gun visibly smaller. PgUp must now reveal
*more* world rather than zoom. If the world looks warped at 90° yaw, the ordering
is wrong and needs revisiting — that is the one in-headset check this fix needs.

## 1. Draw census (`]` key)

`draw_diag.cpp`. Press **`]`** in game (standing still, weapon visible, ideally
an enemy or teammate a few metres away for contrast). Four frames of every draw call
are folded into signatures keyed by *(vertex shader, game call-site RVA, z-enable,
z-write, alpha-blend, render target is backbuffer + size, projection class, has
bone palette)*. The table is written to `bfbc2vr_draws_NN.txt` next to the log,
sorted by draws/frame, and the bone / own-projection rows are echoed to the log.

Per signature: draws and prims per frame; the draw's own projection recovered from
its WVP (`a`, `b` → `tan = 1/a`, near plane); the view-space origin distance band
`[min..max]` and its z range; the residue `M·VP⁻¹` entries `w33`/`w23` (1/0 when the
draw uses the world P); a `bone0` sample (translation and 3×3 shape of the first
bone) for bone shaders; up to four game-image RVAs of the call stack.

**What to look for:**

- A small group of **bone** signatures with `view=[0.0x..0.5]`, `CLASSIFIED`,
  distinct from remote-soldier bone draws metres away. That is the weapon + arms.
- Whether those rows say `fov-differs` / `depth-differs` / `fov+depth-differ`. If
  they do, the 1P weapon is rendered with its own projection — which is *why* it
  rendered as a stretched arc under the global correction in Phase 3 — and the
  classifier can rely on projection alone (`accept_on_projection`).
- `bone0`: `diag≈1, offdiag≈0, |t|` small ⇒ bones are object/bind-space and the
  WVP-only correction is complete. A rotation resembling the camera ⇒ bones carry
  view; the WVP correction is *still* right (it acts after view space), just note it.
- `ret=` RVAs: one shared engine draw site is expected; if a deeper `stack=` frame
  is unique to the weapon rows it becomes an optional extra leg, never the only one.

## 2. Classifier (`draw_policy.cpp`, unit-tested)

Per constant write, fail-closed: `has_wvp && perspective && has_bones &&
(own projection || view-origin within 0.6 m and |z| ≤ 1 m)` ⇒ `Viewmodel`.
Anything else keeps the global correction. **Shift+DELETE** toggles the bone
requirement for experiments. `ctest --test-dir build -C Release` runs the
deterministic tests (projection recovery round-trip, correction collapses to the
global one when `P_vm == P`, classifier edge cases).

## 3. Viewmodel correction (DELETE / `[` cycles, `-`/`=` push)

For classified writes: `M' = M · P_vm⁻¹ · Δ · C_view · P_sel`, with `C_view =
V⁻¹·r·V` the frame's head/6DOF/eye/FOV correction in view space and `Δ` the
weapon's own offset in the *body* camera frame (push along +z now; a controller
grip delta later, same slot — BFVR's `World·sourceView·gripDelta·residualEye·P`).

| mode (DELETE / `[`) | `P_sel` | Expect |
|---|---|---|
| 0 | — (global correction for everything) | today's behaviour |
| 1 | the weapon's own P | the Phase-3 distortion gone; if `P_vm == P` this is identical to mode 0 |
| **2 (default)** | world field, weapon's near plane | VR-correct angular size, no near clipping |
| 3 | world P outright | as 2, but may clip at the world near plane |

Push (`=`) moves the weapon out along the body's forward, both eyes together; `-`
brings it back. Default 0. Start at mode 1, confirm the gun fuses and no longer
stretches when you turn your head, then try 2 and a 0.3–0.5 m push.

The `[viewmodel]` log line every ~5 s reports mode, push, classified writes per
frame and how many were classified by projection evidence; `[vm-hist]` is the
view-space distance histogram of all corrected writes.

## Keys (new)

| Key | Action |
|---|---|
| `]` | 4-frame draw census → `bfbc2vr_draws_NN.txt` |
| Shift+`]` | toggle per-draw own-projection correction (off = old global path, warps) |
| DELETE or `[` | cycle viewmodel projection mode 0→1→2→3 |
| Shift + (DELETE or `[`) | toggle classifier bone requirement |
| `-` / `=` | viewmodel push −/+ 0.05 m |
| PgUp / PgDn | manual FOV widen ±0.05 (default 1.0) |
| Shift+PgUp | toggle automatic headset FOV match (engine-cull void beyond ~58°×45°) |

## 4. Census result (2026-08-20, same day): the depth-slice finding

`bfbc2vr_draws_08.txt` (outdoor scene, 2048×1536): **670 of ~716 scene draws per frame are
drawn with a near plane of 7.48 m or 21.34 m**, 46 with 0.1 m. Frostbite renders the view
in depth slices for Z precision; VP (c185) carries only the 0.1 m projection.

For a draw with its own `P'`, the global correction `VP⁻¹·r·VP` leaves `P'·P⁻¹` in front of
`r`. That residue is `[[1,0,0,0],[0,1,0,0],[0,0,1,(q'−q)/t],[0,0,0,t'/t]]`: rotation about the
eye passes through exactly, but **every translation in `r` — eye offset, 6DOF lean, push — is
multiplied by `t'/t` = 75 (7.48 m slice) or 213 (21.34 m slice)**. A 3.3 cm eye shift became
2.5 m; a 10 cm lean moved the far world 7–21 m. That is the "world warps around me, but I can
still look around" report, and it has been present since stereo first lit up. The unit test
`test_depth_slice_translation_is_exact` pins it.

Consequence: the per-draw correction `P_d⁻¹ · C_view · P_d` (built around each draw's own
recovered projection, cached per distinct `P_d`, ~4 per frame) is now the **default for every
recoverable write**; the global form is its `P_d == P` special case and remains only as the
fallback for writes whose projection cannot be recovered. `a/b` are snapped to the world's when
within 2 % so a non-rigid World cannot leak into the field; `q/t` are always the draw's own.
**Shift+`]`** toggles the old global-only path for A/B — expect the warp to come back.

The weapon (bone shaders, own FOV tan 0.357/0.268) is the only class that also gets an offset;
default mode is now **2 (hybrid)**: world field of view, weapon's own depth range.

## 5. Engine culling: the widen trick hits its wall

User screenshot (mirror, auto widen 2.6×/3.4× live): gray void over most of the image — the
engine culls objects against its own ~58°×45° frustum, so everything the widened render
would show beyond that cone (sky segments, terrain patches, trees) is never drawn.
`settings.ini Fov=90` has **no effect** (tangents identical at 55 and 90). Before today the
dead `K` hid this by magnifying a native image.

Decision: auto widen **off by default** (correct geometry in a theater window); PgUp/PgDn
manual, Shift+PgUp toggles auto. The real fix is engine-level: make the game itself render
*and cull* a headset-sized FOV. `BFBC2Game.exe` contains the reflection names `Fov`,
`DefaultFOV`, `RenderFov`, `ZoomRenderFov`, `InfantryFOVMultiplier`, `ForceFov`,
`FovModifier`, `CameraFov` — the starting points for the Ghidra/memory-patch track
(BFVR's `RenderView::getFrustum` ×1.5 inflation is the pattern).

## 6. Engine FOV + floating gun (2026-08-20, end of day)

With the engine's own camera FOV held at the headset's vertical field (`docs/console.md`), the
first-person **arms** reach behind the near plane and render as blades — the viewmodel is
authored for a 30° field and the shoulders sit at/behind the eye. Tested: holding the weapon's
own FOV equal to the camera's does not help. Fix by elimination with eye shots: the arms +
gloves are one vertex shader, bytecode FNV `76fbfb1ef6146ed9`; hiding its draws while the
write is classified Viewmodel leaves a clean floating gun (`hide`/`unhide`/`hidden` commands;
hidden by default). Push default 0.15 m. This is also the shape the controller-held gun needs.

## 7. The weapon follows a motion controller (Phase 7a, 2026-08-27)

No engine reverse engineering was needed for this: the arm's-length push already occupied the
offset slot in `M' = M · P_vm⁻¹ · Δ · C_view · P_sel`, which is the same slot BFVR feeds its
grip delta into (`World · sourceView · gripDelta · residualEye · P`). Δ simply became the
controller's motion instead of a constant.

**The math** (`draw_policy.cpp`, unit-tested):

- `openvr_pose_to_view` converts an OpenVR 3×4 (column-vector, right-handed, −Z forward) into
  our row-vector left-handed view convention by transposing the rotation and conjugating with
  `C = diag(1,1,−1)`.
- `make_grip_delta` = `inverse(gripAtCalibration ⊘ head) · (grip ⊘ head)`, i.e. head-relative,
  so shared head-and-hand motion (walking, leaning) leaves the weapon alone.
- It is a **full rigid delta**. Rotating the controller in place yields a non-zero translation —
  the compensating term that keeps the *grip point* fixed. A rotation-only delta would swing
  the weapon about the eye instead (BFVR's recorded dead-end). The unit test asserts the grip
  point maps to itself rather than asserting the translation is zero; the first version of that
  test asserted the wrong thing and the code was right.
- Fail-closed: no actively-tracked pose (VALID alone is refused — OpenVR keeps reporting an
  inferred pose after tracking loss), a non-invertible pose, or a jump over 0.5 m between
  accepted samples drops back to the native weapon pose and re-calibrates.

**Scope, stated plainly:** this is *presentation only*. The engine still owns aim, firing,
recoil, reload and projectiles, so the bullet does not follow the barrel. Making it do so needs
either the engine's own fire basis (`ClientWeaponFiringEffects::m_shootSpace`,
`docs/bc2-engine.md`) or synthesised look input that walks the body's aim onto the controller
direction (BFVR's `InfantryAuthoritativeAim`).

Commands: `grip on|off|left|right|recal`, `grip <units-per-metre>`, `gripsmooth <0..1>`.
Calibration happens on the first tracked sample after `grip on` or `recenter`: hold the
controller where the on-screen weapon already is, then move.
