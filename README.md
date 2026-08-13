# bfbc2-vr

A 6DOF VR mod for **Battlefield: Bad Company 2** (Frostbite 1.5, 2010).

Goal: real roomscale VR — stereo rendering, 6DOF head tracking, and motion-controlled
gun handling — injected into a game that has no VR support, no mod SDK, and no source.

**Status:** Phase 0 (recon). Nothing built yet.

---

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
docs/recon.md          verified facts about the target binary + environment
docs/architecture.md   technical design and phased roadmap
tools/recon.ps1        re-runnable environment probe
```

## Prior art / reference index

| Project | Relevance |
|---|---|
| [openRBRVR](https://github.com/Detegr/openRBRVR) | Primary architectural template — D3D9 + DXVK + OpenXR, 6DOF |
| [TheIronWolfModding/dxvk `vr-dx9-rel`](https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel) | The DXVK fork that adds D3D9 VR support |
| [Helix Mod BFBC2 DX9 fix](https://helixmod.blogspot.com/2013/04/battlefield-bad-company-2dx9.html) | Stereo shader constants for our exact target |
| [DarkStarSword/3d-fixes](https://github.com/DarkStarSword/3d-fixes) | Helix/3DMigoto fix corpus + tooling |
| [Perception](https://github.com/Innovative-Ideas/Perception) | Older d3d9 proxy that fakes a 3D camera via view-projection edits |
| [DrBeef/HL2-VR-Proxy](https://github.com/DrBeef/HL2-VR-Proxy) | Minimal D3D9 proxy → OpenVR direct mode reference |
| [FBOneTools](https://github.com/AnirohDev/FBOneTools) | Frostbite 1 asset editing |
| [FBOneMapEditor](https://github.com/AnirohDev/FBOneMapEditor) | WIP BC2 map editor |
