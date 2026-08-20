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
| **1 (default)** | the weapon's own P | the Phase-3 distortion gone; if `P_vm == P` this is identical to mode 0 |
| 2 | world field, weapon's near plane | VR-correct angular size, no near clipping |
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
| DELETE or `[` | cycle viewmodel projection mode 0→1→2→3 |
| Shift + (DELETE or `[`) | toggle classifier bone requirement |
| `-` / `=` | viewmodel push −/+ 0.05 m |
