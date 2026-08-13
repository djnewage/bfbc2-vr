# Phase 0 results

Captured 2026-08-13. All verified by measurement, not assumption.

## 1. The DX9 path is real and it initializes

With `DxVersion=9` in `settings.ini`, `BFBC2Game.exe` loads:

```
d3d9.dll      (system)
nvd3dum.dll   NVIDIA D3D9 user-mode driver
```

and **does not** load `d3d11.dll`. The DX9 path is a genuine shipped renderer, not a stub.

`dinput8.dll` and `xinput1_3.dll` are both loaded — the Phase 7 input proxy targets exist.

## 2. BFBC2 runs on DXVK — the architecture holds

This was the one Phase 0 answer that could have forced a redesign. It didn't.

Dropping DXVK 3.0.2's 32-bit `d3d9.dll` into the game directory produces a working Vulkan device:

```
info:  Game: BFBC2Game.exe
info:  DXVK: v3.0.2
info:  Build: x86 gcc 16.1.0
info:  Found device: NVIDIA GeForce RTX 3060 (NVIDIA 591.86.0)
info:  Creating device:
info:    Graphics : (0, 0)
info:    Transfer : (1, 0)
```

Loaded module evidence, contrasted against the native run:

| Module | Native D3D9 | On DXVK |
|---|---|---|
| `d3d9.dll` | System32 | **game directory** |
| `nvd3dum.dll` (D3D9 UMD) | loaded | **gone** |
| `nvoglv32.dll` (Vulkan ICD) | — | **loaded** |
| `vulkan-1.dll` | — | **loaded** |
| `steamoverlayvulkanlayer.dll` | — | **loaded** |

The D3D9 user-mode driver disappearing while the Vulkan ICD appears is the proof the
translation is real. Steam's *Vulkan* overlay layer injecting itself is independent
corroboration — Steam only loads that layer into genuine Vulkan applications.

Process health: alive 45s past launch, 354 MB working set, no crash on device creation.

> **Not yet verified:** that the image on screen is *correct*. Module inspection proves a
> Vulkan device was created and the process is healthy; it cannot prove the game isn't
> rendering black or corrupt. Needs a human look or a frame capture.

## 3. Bonus: upstream DXVK already has VR extension providers

The single most useful line in the log:

```
info:  Extension providers:
info:    Platform WSI
info:    OpenVR
info:  OpenVR: could not open registry key, status 2
info:  OpenVR: Failed to locate module
info:    OpenXR
```

**Upstream DXVK 3.0.2 ships OpenVR *and* OpenXR extension providers.** It probes for a VR
runtime at startup and, when it finds one, enables the Vulkan instance and device extensions
that runtime requires.

This matters. openRBRVR was built against `TheIronWolfModding/dxvk vr-dx9-rel`, a *fork* that
added exactly this plumbing. Upstream has since absorbed the capability, so we can likely
build on stock DXVK instead of maintaining a fork — a real reduction in long-term maintenance
burden and a much easier upgrade path.

The OpenVR probe failing here is expected and harmless: SteamVR was not running. Re-check this
log with SteamVR up — the providers should resolve, and that becomes our Phase 3 smoke test.

## 4. Noted for later — depth format substitution

```
info:  D3D9: VK_FORMAT_D16_UNORM_S8_UINT -> VK_FORMAT_D24_UNORM_S8_UINT
```

DXVK substitutes a deeper depth format than the game asked for. Harmless now, but flag it
against openRBRVR's dual Z-range trick: it fought z-fighting across a 0.01–10000 near/far
split by giving cockpit and world geometry different depth ranges. BFBC2 has the same
first-person-weapon-vs-distant-terrain problem, so depth precision will come back in Phase 4.

## Environment

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 3060, driver 32.0.15.9186 (Vulkan 591.86.0) |
| Vulkan runtime | present in System32 (`vulkan-1.dll`, `vulkaninfo.exe`) |
| Documents | OneDrive-redirected → `C:\Users\djnew\OneDrive\Documents` |
| Config | `OneDrive\Documents\BFBC2\settings.ini`, original at `settings.ini.bak` |
| Applied | `DxVersion=9`, `Fullscreen=false`, `1280x720` |
| DXVK | 3.0.2 x32, installed via `tools/dxvk.ps1 -Enable` |

## Remaining Phase 0 work

- [ ] Confirm the rendered image is actually correct on DXVK (human eye).
- [ ] RenderDoc captures per `docs/renderdoc-capture.md` — the three-capture set.
- [ ] Record draw call structure, render target layout, and the deferred resolve point.
- [ ] Determine whether view/projection arrive as separate matrices via
      `SetVertexShaderConstantF` or pre-multiplied into a per-object MVP.
- [ ] Re-read the DXVK log with SteamVR running; confirm the OpenXR provider resolves.
