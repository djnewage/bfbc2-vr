# Architecture & roadmap

## The three problems

Turning a flat FPS into a 6DOF VR game is not one problem, it's three, and they get harder
in order:

1. **Stereo** — render the scene twice with per-eye projection, get both images to the headset.
   Well-trodden. Prior art exists for this exact game.
2. **6DOF head tracking** — replace the game's camera transform with the HMD pose, including
   *positional* offset, decoupled from the body's movement. Harder, but mechanical.
3. **Motion-controlled gun handling** — decouple weapon aim from view direction and drive it
   from a tracked controller. This is the real project. Everything else is prep.

Most "VR mods" for old games stop at 1 and half of 2 (vorpX, and its BFBC2 profile, do exactly
that: geometry 3D plus head-look, gun still welded to the crosshair). Getting to 3 is what
makes this worth doing.

## Chosen approach

```
BFBC2Game.exe  (x86, DX9 forced via settings.ini)
      │
      │  LoadLibrary("d3d9.dll")  →  resolves to our DLL in the game directory
      ▼
bfbc2vr d3d9.dll
      │   wraps and forwards to ──▶  DXVK (D3D9 → Vulkan)
      │
      │   intercepts:
      │     SetVertexShaderConstantF  → locate & override view / projection matrices, per eye
      │     SetTransform              → fixed-function matrices used by 2D HUD elements
      │     BeginScene / EndScene     → drive the scene once per eye
      │     Present                   → submit to the VR runtime instead of the desktop swapchain
      ▼
OpenXR runtime (SteamVR → Valve Index)
```

This is openRBRVR's architecture, and it is the right one. The reasoning:

- **Why DX9 and not DX11.** DX9's `SetVertexShaderConstantF` is a single, narrow chokepoint
  where every matrix the GPU sees must pass through. DX11 hides the same data inside opaque
  constant buffers that we would have to identify by size and update pattern. DX9 also has the
  Helix Mod fix as a map of which shaders need stereo correction, and DX11 has no equivalent
  for a Frostbite 1.5 title.
- **Why DXVK rather than a raw D3D9 proxy.** D3D9 has no clean path to hand a texture to
  OpenXR — you end up in shared-surface hacks and driver-specific interop. DXVK translates
  to Vulkan, and Vulkan images go straight into an OpenXR swapchain. It also gets us a modern,
  maintained D3D9 implementation on Windows 11 drivers for free. openRBRVR and the
  `TheIronWolfModding/dxvk vr-dx9-rel` fork both prove this works on 32-bit D3D9 games.
- **Why not vorpX.** It's closed-source, and it structurally cannot do problem 3.

### The main risk with this approach

Frostbite 1.5 is a **deferred renderer**. Deferred lighting, SSAO, shadow maps, and other
screen-space effects are computed in a space that assumes one camera. Rendering twice with
different eye matrices means every screen-space pass needs per-eye correction or it will
visibly separate between the eyes. The Helix Mod fix's known-issues list (shadow and HUD
problems, sniper crosshair) is a preview of exactly which passes will fight us.

Fallback if the double-render cost is unaffordable on a 2010 32-bit single-threaded-ish
renderer: **alternate eye rendering** (Luke Ross's R.E.A.L. approach) — render one eye per
frame at double framerate. Cheaper, but introduces ghosting and demands a rock-steady
framerate. Keep it in the back pocket; don't design for it up front.

## Phases

Each phase ends in something runnable. No phase depends on speculative work in a later one.

### Phase 0 — Recon *(current)*
- Launch once to generate `settings.ini`, force `DxVersion=9`, confirm the DX9 path runs
  clean on this machine.
- Capture a singleplayer frame in RenderDoc. Count draw calls, map render targets, find the
  deferred resolve, find the HUD pass.
- Try stock DXVK `d3d9.dll` with no VR code at all. If BFBC2 won't run on DXVK, the whole
  architecture changes and we need to know now.

**Exit:** we know the frame structure and DXVK compatibility.

### Phase 1 — Passthrough proxy
A `d3d9.dll` that forwards every call and does nothing else. Confirms the injection point,
the calling convention, and that we can ship a build the game tolerates.

**Exit:** game runs identically with our DLL in the loop.

### Phase 2 — Matrix discovery
Log every `SetVertexShaderConstantF` write. Correlate register ranges against known camera
movement to identify view and projection. Cross-reference the Helix Mod fix's shader list —
it already encodes which shaders consume the camera transform.

**Exit:** we can print the camera's position and orientation in real time from inside the hook.

### Phase 3 — Mono VR
One eye, HMD orientation only, submitted to OpenXR. Ugly and nauseating, but it proves the
entire pipeline end to end: hook → matrix override → Vulkan image → runtime → headset.

**Exit:** head turning moves the in-game camera in a headset.

### Phase 4 — Stereo
Render the scene twice, per-eye projection from the OpenXR runtime, correct IPD. Begin
fixing screen-space passes that break under stereo.

**Exit:** correct, comfortable stereo depth.

### Phase 5 — 6DOF
Apply the HMD's positional offset to the camera, decoupled from the player body. Leaning,
crouching, peeking around corners. Requires deciding what happens when your head leaves the
player collision volume.

**Exit:** you can physically lean out from behind cover.

### Phase 6 — HUD
Reproject 2D elements onto a floating panel at a comfortable depth instead of pasting them
on the near plane. `SetTransform` interception is the lever. The Helix fix already ships a
HUD-suppression toggle we can learn from.

### Phase 7 — Motion-controlled gun handling *(the actual goal)*
Two paths, and we ship the cheap one first:

- **7a — synthetic input (fallback).** Proxy `dinput8.dll`/`xinput1_3.dll` and translate
  controller pointing direction into mouse deltas. Universal, works without any reverse
  engineering, and gets a playable result fast. But aim and view stay welded — pointing the
  gun turns your whole view. Not real VR gun handling.
- **7b — decoupled aim (the prize).** Reverse engineer the player/weapon structures: find the
  first-person weapon transform and, separately, the aim ray the game traces for hitscan.
  Override both from the controller pose. Tooling: Cheat Engine to find the structures live,
  Ghidra/IDA on `BFBC2Game.exe` to understand them. This is the biggest unknown in the project
  and the phase most likely to need several attempts.

**Exit:** hold the gun independently of where you're looking.

### Phase 8 — Comfort & polish
Snap turn, vignette, weapon model scale and grip alignment, recoil handling, ADS via
physically bringing sights to your eye.

## Notes on tooling

- **RenderDoc** — frame capture and analysis. Phase 0 onward.
- **DXVK** (`vr-dx9-rel` fork lineage) — D3D9→Vulkan backend.
- **OpenXR SDK** — runtime interface; SteamVR for the Index.
- **Ghidra or IDA** — static analysis of the 32-bit exe, Phase 7b.
- **Cheat Engine** — live structure discovery, Phase 7b.
- **FBOneTools** — only if we need to touch game assets (weapon models, HUD layout).

Language: C++ for the DLL. Build must target **x86**, not x64.

## Ground rules

- Singleplayer only. PunkBuster ships with the game — see `recon.md`.
- Never modify files in the Steam install directory in place without a backup; prefer
  dropping our DLL alongside and keeping the install verifiable.
- Every phase gets committed working before the next one starts. This project has enough
  unknowns that a bisectable history is worth the discipline.
