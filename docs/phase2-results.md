# Phase 2 results — the camera is found

Captured 2026-08-13 from `bfbc2vr_diff_00.txt`, frame 14299.
Method: F10 baseline while standing still, rotate in place, F11 diff.

## Headline

| Registers | Contents |
|---|---|
| **c189–c192** | **Camera-to-world matrix** (inverse view) — row-major, translation in `c192` |
| **c185–c188** | **View-projection matrix** (combined) |
| c201, c202 | Scalar params, only `.w` moved — fog/exposure, not camera |

Everything else that changed was `VARYING` per-object churn (c19 upward: world
matrices and bone palettes), correctly filtered out by the stability split.

## The camera matrix

Baseline, standing still:

```
c189    0.98467   -0.00000    0.17442    0.00000
c190    0.00000    1.00000    0.00000    0.00000
c191   -0.17442   -0.00000    0.98467   -0.00000
c192 -617.85333    5.56187  177.67635    1.00000
```

After rotating in place:

```
c189   -0.88389   -0.00000    0.46770    0.00000
c190   -0.10825    0.97285   -0.20457    0.00000
c191   -0.45500   -0.23145   -0.85989    0.00000
c192 -617.85345    5.55908  177.67639    1.00000
```

### Verification

- Upper-left 3×3 is **orthonormal to six decimal places** in both samples
  (row lengths 1.000000, pairwise dots < 5e-6). Pure rotation, no scale.
- `c192.w == 1.0` and the last column is `(0,0,0,1)` → affine, **row-major**,
  translation in the last register.
- Yaw went **10.04° → 152.11°**, a 142° turn.
- Position moved `|Δ| ≈ 0.003` units — idle sway, effectively stationary.

### It is camera-to-world, not view

This distinction matters for Phase 5 and it is worth stating why we are sure.

A true view matrix has translation `-(R · eye)`, which **changes when you
rotate even if you do not move**, because `R` changed. Here the translation
row was identical to five decimals across a 142° turn. Therefore `c192` holds
the raw camera position and `c189–c191` the camera's world orientation — this
is the **inverse view / camera-to-world** matrix.

So the camera is at `(-617.85, 5.56, 177.68)` in world space, Y-up
(5.56 is a plausible eye height above terrain).

## Why this is the good branch

`docs/architecture.md` flagged the risk that the engine pre-multiplies the
camera into per-object MVP matrices, which would have forced us to recover the
view transform by factoring. It does not. BFBC2 hands the GPU a clean,
standalone camera matrix **and** a separate view-projection matrix.

That means Phases 3–5 are direct edits rather than reconstruction:

- **6DOF head tracking** — write the HMD pose into `c189–c192`.
- **Stereo** — write per-eye view-projection into `c185–c188`.

Both are the openRBRVR pattern: cancel the game's transform, substitute the
VR runtime's.

## Two tooling bugs this exposed

**Matrix blocks are not 4-aligned.** `classify()` only ran on registers where
`i % 4 == 0`. Both camera matrices start at c185 and c189 — both ≡ 1 (mod 4) —
so the auto-annotation silently skipped the exact matrices it existed to find.
The analysis above was done by hand. Now scans every 4-register window.

**Register usage is scene-dependent.** Early logs reported `highest c18` at the
menu; in gameplay it reaches c202. Any conclusion drawn from menu-time data
about which registers matter would have been wrong.

## Next

- [ ] Confirm `c185–c188` is view-projection by isolating FOV: baseline, toggle
      ADS, diff. Projection changes, camera-to-world should not.
- [ ] Confirm the registers are stable across levels and vehicles — c189 may
      only hold for the infantry camera.
- [ ] Phase 3: override `c189–c192` with a synthetic rotation and confirm the
      rendered view moves. That proves write-side control before OpenXR is
      involved at all.
