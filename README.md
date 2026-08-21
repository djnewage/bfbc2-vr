# bfbc2-vr

A 6DOF VR mod for **Battlefield: Bad Company 2** (Frostbite 1.5, 2010) — stereo rendering,
head tracking and roomscale leaning injected into a game with no VR support, no mod SDK and
no source, through a `d3d9.dll` proxy on DXVK submitting to SteamVR.

**Status — working today:**

- **Stereo + 6DOF head tracking** in the headset (alternate-eye rendering, IPD read from the
  headset, lean/duck/peek at 1:1 world scale)
- **Full headset field of view, rendered *and culled* by the engine.** The camera's own FOV
  value is located in memory at runtime and held at the headset's vertical field, so there is
  no black border, no theater window, and no missing geometry at the edges
- **Per-draw projection correction.** Frostbite renders the scene in depth slices (near planes
  at 0.1 / 7.48 / 21.34 m); each draw is corrected around its *own* recovered projection
- **First-person weapon** identified by shader identity + its own field of view, corrected
  separately, with the arms hidden (they intersect the eye at headset FOV)
- **HUD on a floating panel** — HUD draws are redirected into a transparent render target and
  shown as an OpenVR overlay in front of the player's aim direction, out of the world image
- **A command channel** (`bfbc2vr_cmd.txt`) + status file + in-process memory scanner, so the
  mod can be driven, measured and screenshotted without touching the keyboard

**Not done yet:** motion-controlled gun handling (the real goal), ADS/scope handling, comfort
options. See `docs/architecture.md` for the roadmap.

Everything here was built by measurement rather than guesswork; the `docs/` folder is the
evidence trail, including three bugs that survived weeks because nothing was verified against
the game's own numbers (`docs/viewmodel-census.md` §4).

---

## Requirements

- Battlefield: Bad Company 2 (Steam), **singleplayer** — see the constraint below
- A PC VR headset with SteamVR running **before** the game
- [DXVK](https://github.com/doitsujin/dxvk) — drop its 32-bit `d3d9.dll` into the game folder
  renamed to `dxvk_d3d9.dll`
- Visual Studio 2022 build tools with CMake, and the
  [OpenVR SDK](https://github.com/ValveSoftware/openvr) (see *Building*)

## Building

```powershell
# 1. OpenVR: copy from the SDK into the repo (not vendored - Valve's binaries stay Valve's)
#      thirdparty/openvr/lib/win32/openvr_api.lib
#      thirdparty/openvr/bin/win32/openvr_api.dll
# 2. Build (x86 - BFBC2Game.exe is 32-bit) and install into the game folder
pwsh -File tools\build.ps1 -Install

# Restore the game to stock at any time
pwsh -File tools\build.ps1 -Uninstall
```

`tools\build.ps1 -GameDir "<path>"` if your install is not in the default Steam location.
`tools\configure-dx9.ps1` forces the game's DX9 path and sets a VR-friendly resolution.
Unit tests for the pure math: `ctest --test-dir build -C Release`.

## Using it

Launch SteamVR, then the game, then load a singleplayer mission. The mod engages by itself:
it finds the engine's FOV a few seconds after the world appears and matches it to the headset.
**F5** recenters (face the way your body is aiming). `docs/console.md` lists every key and
every command; `docs/viewmodel-census.md` explains the rendering decisions.

## Why this is possible

BFBC2 ships a **DirectX 9 render path** that can be forced via `settings.ini`. That single
fact makes the whole project tractable, because it puts us on the same ground as the best
open-source prior art:

- **[openRBRVR](https://github.com/Detegr/openRBRVR)** — a working, actively maintained 6DOF
  OpenXR implementation for *Richard Burns Rally*, a 2004 D3D9 game with no source. Same
  problem shape as ours. Its approach (hook `SetVertexShaderConstantF` / `SetTransform` /
  `Present`, run on a DXVK D3D9→Vulkan backend, hand Vulkan images to OpenXR) is our template.
- **[Helix Mod: BFBC2 \[DX9\]](https://helixmod.blogspot.com/2013/04/battlefield-bad-company-2dx9.html)** —
  a 2013 3D Vision fix for *this exact game* on DX9. Someone already located the stereo-relevant
  shader constants and catalogued which shaders break. That is a large chunk of Phase 2 done
  and documented.
- **[FBOneTools](https://github.com/AnirohDev/FBOneTools)** — Frostbite 1 asset toolchain
  (`.fbrb`, `.dbx`, textures). Not needed for rendering, but essential if we ever touch
  weapon models, FOV assets, or HUD layout.

## Non-negotiable constraint

**Singleplayer / offline only.** The game ships PunkBuster (`pb/`, `pbcl.dll`, `pbsv.dll`).
Injecting a DLL into a multiplayer session will get you kicked or banned. Official servers
shut down 2023-12-08; community multiplayer now runs on
[Venice Unleashed: Project Rome](https://veniceunleashed.net/). Do not point this mod at it.

## Layout

```
src/                  the proxy DLL (x86)
  camera_override     the correction: per-draw projection, head pose, eyes, viewmodel
  draw_policy         pure classification + correction math (unit-tested)
  draw_diag           the draw-signature census behind every decision here
  vr_compositor       DXVK->Vulkan interop, eye submission, the HUD overlay
  memscan             in-process scanner + the autonomous engine-FOV hunt
  console             file-driven command channel and status dump
docs/architecture.md  design and phased roadmap
docs/console.md       commands, status file, and how the FOV was found
docs/viewmodel-census.md   what the draw census proved, and the bugs it caught
docs/prior-art-bfvr.md     what transfers from the shipped BF1942 VR mod
docs/bc2-engine.md         public Frostbite 1.5 class layouts + the reflection system
tools/                build/install, DXVK, settings, and recon scripts
```

## Licence

MIT — see `LICENSE`. Third-party headers under `thirdparty/` keep their own licences
(OpenVR: BSD-3-Clause, Vulkan: Apache-2.0, DXVK interop header: zlib).

## Prior art / reference index

| Project | Relevance |
|---|---|
| [openRBRVR](https://github.com/Detegr/openRBRVR) | Primary architectural template — D3D9 + DXVK + OpenXR, 6DOF |
| [BFVR (Battlefield 1942 VR)](https://github.com/JayBiggsGMG/BFVR-Battlefield-1942-VR-Mod) | Shipped sibling mod: viewmodel classification, grip math, cull-FOV hook, HUD layer, engine input hooks — see `docs/prior-art-bfvr.md` |
| [TheIronWolfModding/dxvk `vr-dx9-rel`](https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel) | The DXVK fork that adds D3D9 VR support |
| [Helix Mod BFBC2 DX9 fix](https://helixmod.blogspot.com/2013/04/battlefield-bad-company-2dx9.html) | Stereo shader constants for our exact target |
| [DarkStarSword/3d-fixes](https://github.com/DarkStarSword/3d-fixes) | Helix/3DMigoto fix corpus + tooling |
| [Perception](https://github.com/Innovative-Ideas/Perception) | Older d3d9 proxy that fakes a 3D camera via view-projection edits |
| [DrBeef/HL2-VR-Proxy](https://github.com/DrBeef/HL2-VR-Proxy) | Minimal D3D9 proxy → OpenVR direct mode reference |
| [FBOneTools](https://github.com/AnirohDev/FBOneTools) | Frostbite 1 asset editing |
| [FBOneMapEditor](https://github.com/AnirohDev/FBOneMapEditor) | WIP BC2 map editor |
