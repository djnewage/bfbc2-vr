# Recon — verified facts

Everything here was measured on this machine, not recalled. Re-run `tools/recon.ps1` to refresh.

Date of capture: 2026-08-13

## Install

```
C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2
```

Steam libraries on this machine: `C:\Program Files (x86)\Steam`, `C:\Games`.

## Target binary

| Property | Value |
|---|---|
| Executable | `BFBC2Game.exe` |
| Size | 18.79 MB |
| **Architecture** | **x86 (32-bit)** |
| PE sections | 10 |
| PE timestamp | 2011-08-24 18:42:55 UTC |
| File version | 1.0.1.0 |
| SHA-256 | `22E96001382EB847D3AB0A02B59EC7BB79BA47E04146E74C6C49E38465F38C96` |

32-bit matters: our injected DLL must be 32-bit, and we share the game's ~2–4 GB address
space. Stereo render targets come out of that budget.

## Graphics / input surface

DLL references found in the binary:

```
d3d9.dll   d3d10.dll   d3d10_1.dll   d3d11.dll   dxgi.dll
d3dx9_42.dll   d3dx10_42.dll   d3dx11_42.dll   d3dcompiler_42.dll
dinput8.dll   xinput1_3.dll   nvapi.dll   binkw32.dll
pbag.dll  pbcl.dll  pbclnew.dll  pbclold.dll  pbsv.dll  pbsvnew.dll  pbsvold.dll
```

Redistributables shipped alongside the exe: `D3DX9_42.dll`, `d3dx10_42.dll`, `d3dx11_42.dll`,
`D3DCompiler_42.dll`, `binkw32.dll`, `msvcr71.dll`, `GDFBinary.dll`.

**All four D3D runtimes are referenced**, and the renderer is chosen at startup — so DX9 is a
real, shipped, supported path, not a fallback stub.

### Why this makes `d3d9.dll` proxying viable

`d3d9.dll` is **not** on the Windows KnownDLLs list. For a 32-bit process, the application
directory is searched before `System32`, so a `d3d9.dll` placed next to `BFBC2Game.exe` is
loaded instead of the system one. This is the cheapest possible injection point — no
`CreateRemoteThread`, no manual mapping, no separate injector process.

### Input

`dinput8.dll` and `xinput1_3.dll` are both referenced. Either is proxyable by the same
mechanism, which is how we will eventually feed synthetic look/aim input from motion controllers.

## Subdirectories

```
dist/  install/  Output/  Package/  pb/  Support/
```

`pb/` is PunkBuster. `Package/` holds the Frostbite `.fbrb` archives (FBOneTools territory).

## Configuration

`%USERPROFILE%\Documents\BFBC2\settings.ini` — **does not exist yet**; the game has not been
launched. It is generated on first run.

The renderer is selected by the `DxVersion` key in that file. Forcing DX9 is step one of
Phase 0. The Helix Mod fix for this game also requires forcing DX9, which is independent
confirmation that the DX9 path is complete and playable.

FOV is adjustable via `settings.ini` in multiplayer but **not** in singleplayer — the vorpX
profile for this game works around it by editing `Fov=74` directly. Since we are singleplayer-only,
expect to set FOV through the ini or by overriding the projection matrix ourselves (we will be
replacing the projection matrix anyway, so the game's FOV largely stops mattering after Phase 4).

## Anti-cheat

PunkBuster client DLLs ship in the install. **Singleplayer only.** Official servers were
retired 2023-12-08; community multiplayer is Venice Unleashed: Project Rome. Do not inject
into any online session.

## Open questions for Phase 0

- [ ] Does the DX9 path actually run cleanly on Windows 11 / current GPU drivers?
- [ ] What does a frame look like in RenderDoc under DX9 — draw call count, render target
      layout, where the deferred lighting resolve happens?
- [ ] Is the camera matrix delivered via `SetVertexShaderConstantF` (expected) or baked
      per-object into a combined MVP?
- [ ] Does DXVK's D3D9 implementation run BFBC2 at all, before we add any VR code?
