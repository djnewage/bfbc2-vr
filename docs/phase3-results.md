# Phase 3 results — head tracking is live

2026-08-13, one day from "the game is downloading" to an Index steering the camera.

## Confirmed working

```
[vr] OpenVR initialized (Background). HMD: Index (LHR-F8C14B95)
[correct] ON  (convention=transposed, vp=1 eye=1 correction=1)
[vr] head tracking engaged (ref yaw=-6.1 pitch=30.4 deg)
```

Turning the head turns the in-game world. The full chain, every link measured
rather than assumed:

```
Index pose (OpenVR Background mode)
  -> yaw/pitch delta vs reference
  -> R about the eye (yaw: world Y; pitch: camera right, Rodrigues)
  -> CORRECTION = VP^-1 * R * VP        rebuilt once per frame
  -> applied per draw to CTAB-named WVPs (S' = C^T * S, transposed convention)
     and to fingerprinted WVPs inside anonymous "constants" blobs
  -> DXVK -> Vulkan -> screen
```

~900 corrected transform writes per frame, steady.

## What it took - the discovery chain

1. `c185-c188` view-projection and `c189-c192` camera-to-world found by
   stability-split diffing (STABLE within frame + changed across frames).
2. Overriding those moved only fog/billboards: main geometry uses per-object
   pre-multiplied WVPs. (First recording: vertex explosion + rolled world.)
3. No fixed register - each shader has its own layout. Answered by parsing the
   D3D9 CTAB per shader: DICE's own names (`worldViewProj`, `viewProjMatrix`,
   `worldViewProjMatrix`), all 65 shaders intact.
4. Convention mismatch: compiled shader constants are column-major/transposed;
   the global c185 block is row-major. Engine mixes conventions.
5. Terrain megashaders declare only an anonymous `constants` blob - solved by
   fingerprinting: a 4-register window is a WVP iff `S^T * VP^-1` is affine.
   (Second recording: world coherent, terrain holes. Third: holes closed.)

## Known artifacts, deferred

- **First-person weapon distorted** (black stretched arc): skinned via
  `boneMatrices`/`boneVectors` (180 registers), bones disagree with the
  corrected WVP. Likely bones bake a transform we are not correcting.
  Revisit with stereo, where the weapon needs special handling anyway.
- **Skydome coverage gap** at extreme angles (gray void patch).
- HUD stays screen-fixed (expected; Phase 6 owns it).
- FOV is the game's flat ~55deg; real per-eye projection arrives in Phase 4.

## Notes for Phase 4 (stereo)

- OpenVR Background mode was the right call for tracking - zero graphics
  binding. Stereo needs the real thing: per-eye render + submission, via
  OpenXR/Vulkan (DXVK 3.0.2 ships both extension providers upstream) or
  OpenVR's IVRCompositor with Vulkan textures.
- The correction mechanism IS the per-eye mechanism: substitute per-eye R
  (+ eventually per-eye projection) instead of head yaw/pitch. The open
  problem is not the matrices, it is rendering the scene twice and handing
  both images to the compositor from inside a 32-bit process.
- Theater mode: Steam auto-wraps flat games when SteamVR runs. Harmless for
  testing; disable per-game via Properties if it costs perf.
