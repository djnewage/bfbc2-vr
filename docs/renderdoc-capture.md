# RenderDoc capture procedure — BFBC2

RenderDoc 1.45, installed at `C:\Program Files\RenderDoc`.
32-bit support files present at `C:\Program Files\RenderDoc\x86\`.

## Read this first: RenderDoc cannot capture D3D9

RenderDoc supports **Vulkan, D3D11, D3D12, OpenGL, and OpenGL ES**. D3D9 is not supported
and never has been. You cannot point RenderDoc at stock BFBC2 on the DX9 path and get a
capture.

**DXVK is what makes this game capturable.** It translates D3D9 to Vulkan, and Vulkan is a
first-class RenderDoc target. So the requirement is the opposite of what you'd assume:

> **DXVK must be ENABLED to capture.** `tools\dxvk.ps1 -Enable`

Verified working in Phase 0 — see `phase0-results.md`.

This also means captures show the **Vulkan** side of the translation, not raw D3D9 calls.
That is fine for the questions RenderDoc is good at (pass structure, render targets, formats),
but it is *not* where we hook. For D3D9-level questions — which is where our mod actually
sits — use the logging proxy instead. See "Division of labour" below.

## What RenderDoc answers

1. **Pass structure** — how many draws, grouped into which passes.
2. **Render target layout** — Frostbite 1.5 is deferred, so we need the G-buffer, the lighting
   resolve, shadow map passes, and the post chain. Every screen-space pass here is something
   that will break under stereo.
3. **Formats and resolutions** — anything full-screen gets doubled in stereo and drives our
   VRAM and bandwidth budget.
4. **Where the HUD is drawn** — which pass, and whether it goes through the fixed-function
   path. Determines the Phase 6 approach.

## Capture procedure

1. Ensure DXVK is enabled: `pwsh -File tools\dxvk.ps1 -Status`
2. Launch BFBC2 through Steam and get into a singleplayer level.
3. RenderDoc → **File → Inject into Process** → `BFBC2Game.exe` → **Inject**.
   - RenderDoc's UI is x64 but injects the x86 `renderdoc.dll` into 32-bit targets.
   - Injection after startup can miss instance/device creation. If the capture looks
     incomplete, use **Launch Application** instead, pointing at
     `...\Battlefield Bad Company 2\BFBC2Game.exe`. Steam DRM may refuse a direct exe launch;
     fall back to injection if so.
4. Alt-Tab to the game and press **F12** (or PrtScn) to capture.

Windowed mode is already configured (`Fullscreen=false`, 1280x720) — exclusive fullscreen
makes Alt-Tab and the RenderDoc overlay unreliable.

### Known issue

RenderDoc + DXVK has a history of capture crashes
([baldurk/renderdoc#2157](https://github.com/baldurk/renderdoc/issues/2157)). If captures crash
the game, try a `dxvk.conf` next to the exe with `dxvk.enableGraphicsPipelineLibrary = False`
and disable the Steam overlay, which also injects a Vulkan layer and can conflict.

## What to capture

Three captures, answering different questions:

| Capture | Scene | Answers |
|---|---|---|
| 1 | Standing still, looking at terrain | Baseline pass structure, G-buffer layout |
| 2 | Same spot, camera rotated ~90° | Diff constants against #1 to isolate the **view** matrix |
| 3 | Aiming down sights | Isolates the **projection** matrix (FOV change) + weapon draws |

The 1-vs-2 pair is the matrix-discovery trick: whatever floats changed between two frames that
differ *only* by camera rotation is the view matrix. Capture 3 changes FOV instead, which moves
the projection matrix. `Fov=55` in `settings.ini` is a second lever for the same purpose.

### Reading D3D9 constants through the DXVK layer

Our hook target, `SetVertexShaderConstantF`, sets D3D9 float constant registers `c0`–`cN`.
DXVK packs these into a Vulkan uniform buffer, so in a RenderDoc capture they appear as
constant buffer contents rather than named D3D9 registers.

Expected mapping — **verify before relying on it**, this has not been confirmed against
DXVK 3.0.2's actual layout: register `cN` lands at byte offset `N * 16` (four floats per
register). Confirm by finding a recognizable value in the buffer, such as a view matrix's
bottom row holding camera position, and working backwards to its register index.

## Division of labour

RenderDoc is not the only tool, and for matrix work it is not the best one.

| Question | Tool |
|---|---|
| Render target layout, pass structure, formats | **RenderDoc** (Vulkan capture) |
| Which D3D9 registers hold view/projection | **Logging d3d9 proxy** (Phase 1/2) |
| Live per-frame camera values | **Logging d3d9 proxy** |

The proxy chains in front of DXVK — game → our `d3d9.dll` → renamed DXVK DLL → Vulkan —
and logs `SetVertexShaderConstantF` directly. That reads the API we actually hook, with no
translation layer in between, and it doubles as the Phase 1 deliverable. Build it rather than
trying to reverse the register mapping through RenderDoc.

## What to record in `docs/frame-analysis.md`

- Draw call count and pass breakdown (shadow / G-buffer / lighting / transparent / post / HUD).
- Render target formats and resolutions, flagging every full-screen target.
- Which passes are screen-space and will therefore need per-eye correction.
- Where the HUD is drawn and how.

## Storage

Captures default to `%TEMP%\RenderDoc\` and are large. `captures/` and `*.rdc` are gitignored —
keep them local, commit the written analysis instead.
