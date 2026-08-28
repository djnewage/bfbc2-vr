# The tilting horizon: using the full head orientation

2026-08-27. In-headset report: *"head movement isn't stabilised like in normal VR games"*,
and on being asked, the horizon tilts **when turning and looking around** — not only when
tilting the head. That distinction was the useful part: it meant two separate defects.

## What was wrong

The correction was built from two Euler scalars (`camera_override.cpp`, before this change):

```cpp
rotation_axis(wy /*world up*/, g_yaw_sign * g_hmd_yaw, ry);
rotation_axis(g_cam_right,     g_pitch_sign * g_hmd_pitch, rp);
multiply(rp, ry, rot);   // pitch about camera right, then yaw about world up
```

1. **Head roll was never extracted.** `hmd_yaw_pitch()` derives yaw and pitch from the head's
   forward vector alone — a forward vector cannot carry roll. Tilt your head and the image did
   not counter-rotate, so the virtual horizon rolled with you.
2. **The composition injected roll while merely turning.** Pitch was applied about the *body
   camera's* right axis, then yaw about world up. Once pitched, turning rotates about an axis
   that is no longer the head's right, and the result is not a roll-free orientation. Measured
   in a unit test against the old code: at yaw 0.6 rad, pitch −0.35 rad, the camera's right axis
   came out **11° off horizontal**. That is the tilt the player saw.
3. **Leaning used the wrong basis** (found while reading, not reported): the positional delta
   was mapped from raw OpenVR tracking axes straight through the game camera basis, which is
   only correct if the recenter happened to face the tracking origin's forward. At any other
   heading, leaning sideways slid the player partly forwards or backwards.

All three are the same mistake — decomposing a 3-DOF orientation into scalars — and BFVR does
not make it: `MakeD3D8ViewFromOpenXRPose` is quaternion → 3×3 with no Euler anywhere, and roll
is stripped only where a consumer wants gravity alignment (`MakeYawOnlyUiAnchor` for panels).

## What it is now

Row-vector, left-handed (`src/draw_policy.cpp`, unit-tested):

```
B    = R_now · R_ref⁻¹        head rotation since recenter, in the reference's frame
T    = yaw(turn)              deliberate turn of the virtual body (snap turn)
A    = B · T                  camera rotation relative to the body
rot  = V · A⁻¹ · M_cw         the same rotation in world space, inverted
                              (M_cw = camera-to-world basis, V = M_cwᵀ)
```

The eye baseline now rides the **head's** right axis (`(1,0,0)·A·M_cw`) so a tilted head tilts
the stereo separation, matching BFVR's `headPosition + headOrientation·(±IPD/2,0,0)`. The lean
delta is rotated into the reference's frame before the camera basis.

`M_cw` is orthonormal to six decimals (`docs/phase2-results.md`), so conjugating by it is safe
and the inverses are transposes.

Deliberately still yaw-only: the HUD panel anchor (a tilted head must not cant a gravity-aligned
panel) and the snap-turn / aim accumulator, which reason about a scalar heading.

## How it was verified

The tests were written first and run against a stub reproducing the **actual old composition**,
to prove they bite: six assertions failed, including the 11° measurement above. Then the real
implementation turned them green. `headroll off` restores the old yaw+pitch path for A/B in the
headset, and the reference falls back to it automatically if a head pose is unavailable.
