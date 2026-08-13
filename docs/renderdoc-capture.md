# RenderDoc capture procedure — BFBC2 (DX9, 32-bit)

RenderDoc 1.45, installed at `C:\Program Files\RenderDoc`.
32-bit support files present at `C:\Program Files\RenderDoc\x86\` — **required**, because
`BFBC2Game.exe` is x86. RenderDoc's UI is x64 but it injects the x86 `renderdoc.dll` into
32-bit targets automatically.

## Why we capture before writing any code

Phase 0 needs four answers, and a single good frame capture gives all four:

1. **Draw call structure** — how many draws per frame, and how they're grouped into passes.
2. **Render target layout** — Frostbite 1.5 is deferred, so we need to see the G-buffer, the
   lighting resolve, the shadow map passes, and the post chain. Every screen-space pass here
   is a thing that will break under stereo.
3. **Where the camera matrix lives** — whether view/projection arrive via
   `SetVertexShaderConstantF` as separate matrices (what we want) or pre-multiplied into a
   combined MVP per object (much more painful).
4. **Where the HUD is drawn** — which pass, and whether it uses the fixed-function pipeline
   (`SetTransform`) or shaders. Determines the Phase 6 approach.

## Launching a capture

Steam complicates direct launch. Two options:

### Option A — inject into the running process (preferred)
1. Launch BFBC2 normally through Steam, get into a singleplayer level.
2. RenderDoc → **File → Inject into Process**.
3. Find `BFBC2Game.exe` in the list, select it, **Inject**.
4. Alt-Tab back to the game, press **F12** (or PrtScn) to capture a frame.

Injection after startup can miss device creation. If the capture looks incomplete, use B.

### Option B — launch under RenderDoc
1. RenderDoc → **Launch Application**.
2. Executable: `C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2\BFBC2Game.exe`
3. Working directory: the same folder.
4. Enable **Capture Callstacks** (helps identify which module sets each constant) and
   **Ref All Resources**.
5. Launch. If Steam DRM refuses a direct exe launch, fall back to A.

## Settings that must be right before capturing

In `%USERPROFILE%\Documents\BFBC2\settings.ini`:

```ini
DxVersion=9
```

Capturing the DX11 path tells us nothing useful — the whole architecture targets DX9.
Verify in RenderDoc's capture header that the API reads **D3D9**, not D3D11.

Also prefer **windowed mode** for capture work. Fullscreen exclusive on DX9 makes Alt-Tab and
the RenderDoc overlay unreliable.

## What to capture

Get three separate captures, they answer different questions:

| Capture | Scene | Answers |
|---|---|---|
| 1 | Standing still, looking at terrain | Baseline pass structure, G-buffer layout |
| 2 | Same spot, camera rotated ~90° | Diff the constant buffers against #1 to isolate the **view** matrix |
| 3 | Aiming down sights (ADS) | Isolates the **projection** matrix (FOV change) and weapon-specific draws |

Captures 1 and 2 as a pair are the actual matrix-discovery trick: whatever floats changed
between two frames that differ only by camera rotation is the view matrix. Capture 3 changes
FOV, which moves the projection matrix instead.

## What to record in `docs/frame-analysis.md` afterwards

- Total draw call count and the pass breakdown (shadow / G-buffer / lighting / transparent /
  post / HUD).
- The register indices (`c0`–`cN`) where the camera matrices land.
- Which shaders consume them — cross-reference against the Helix Mod fix's shader list.
- Render target formats and resolutions, especially anything full-screen (those get doubled
  in stereo and drive our VRAM and perf budget).

## Storage

Captures land in `%TEMP%\RenderDoc\` by default and are large. `captures/` and `*.rdc` are
gitignored — keep them local, commit the written analysis instead.
