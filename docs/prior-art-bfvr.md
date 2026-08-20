# Prior art: BFVR (Battlefield 1942 VR) — what transfers to us

Reviewed 2026-08-20 against BFVR at commit `dd48c87` (v1.0.1, 2026-08-12).
Source: https://github.com/JayBiggsGMG/BFVR-Battlefield-1942-VR-Mod — local clone at
`C:\Users\djnew\bfvr-reference` (read-only reference; not vendored). Their own
continuation guide is `docs/AI_DEVELOPER_HANDOFF.md`; the evidence record is
`devREADME.md` and `docs/first-person-arms-research.md`.

BFVR is a *shipped* mod (~98k lines). Its architecture is different from ours —
D3D8 fixed-function game → patched d3d8to9 → x86 client DLL injected by a launcher →
shared textures → **x64 OpenXR presenter process** — and it leans heavily on
**engine-level hooks** (MinHook on functions found with Ghidra). So its code does not
port. Its *decisions* do, and several answer exactly what our last commits were stuck on.

## Side by side

| Concern | BFVR (BF1942, D3D8) | Us (BFBC2, D3D9 on DXVK) |
|---|---|---|
| Injection | Launcher + remote-thread IAT patch of `Direct3DCreate8` (full proxy interface crashed their game) | `d3d9.dll` proxy in game dir — works, keep |
| Runtime | OpenXR 1.0 in a separate x64 presenter (x86 Oculus runtime crashed in `xrCreateSession`) | OpenVR in-process via DXVK Vulkan interop — works, keep; their split was a bitness bug, not a preference |
| Stereo | **Per-draw double replay** into owned per-eye RTs, same frame; original flat draw skipped; per-draw fail-closed to native draw | **Alternate-eye** (one eye per game frame). Per-draw replay on a deferred renderer would mean mirroring every G-buffer RT per eye — later upgrade, not now |
| Head pose into camera | Engine hook on `RenderView::setTransformation` (full camera-to-world matrix) gated on exact caller | `VP⁻¹·R·VP` correction applied per draw to CTAB-named WVP spans |
| Per-eye projection | Rebuilt from runtime FOV tangents; **near/far recovered from the game's own P** (`near=−M32/M22`); game FOV ignored, zoom re-added as `M00/M11` scale | Game's P kept; FOV widened by camera-space scale + per-eye texture bounds |
| Culling / pop-in | Hook `RenderView::getFrustum`: advance camera to HMD *before* cull, **inflate cull FOV ×1.5 (×2 mounted)**, restore before projection query | Engine culls at native frustum → edge pop-in; mitigated by `Fov=90` in settings |
| Viewmodel identity | **Call-site return address + VS bytecode hash + z/alpha state + narrow viewmodel-FOV projection** (`m00≥2.0 && m11≥3.5`). Distance-to-camera tried and retired | Distance-to-camera (matched ~700 draws/frame → wrong). No Draw* hooks yet |
| Gun ↔ controller | Full rigid delta `inv(refGrip)·curGrip` in head-local, inserted **before** per-eye offset: `World·V·gripDelta·eyeResidual·P`; grip pose = position, aim pose = direction; fail-closed reset on tracking loss/weapon swap | Not started (Phase 7) |
| Arms | Drive the game's **native two-bone IK** (hook `Skeleton::transform`/`applyIk`, hijack a private IK handle) | Not portable — Frostbite bones are VS constants; we'd solve/rewrite bones ourselves |
| Fire direction | Hook weapon-fire core, swap the fire basis for the controller gun pose (caller allowlist); plus **body follows gun** via bounded synthetic look deltas written into the engine's input frame | Not started |
| Input | **Never SendInput for gameplay.** MinHook the engine input-frame builder, add to the normalized `float[55]` before the engine consumes it | Not started (Phase 7a planned dinput proxy — reconsider) |
| HUD | Non-perspective / pre-transformed draws → **separate UI RT** → quad/cylinder layer at 1.5 m; head-locked for HUD, yaw-only world-locked for menus; per-screen **clear vs retain** policy | HUD baked into eye frames (at infinity); menus auto-mono'd |
| Crosshair | Native crosshair suppressed; **world-space sprite at fixed 50 m, angular size, projected per eye** — no raycast | Native (screen-fixed) |
| Scope | `scale = tan(normalFov/2)/tan(zoomFov/2)` applied to `M00/M11`; reticle as eye-filling per-eye quads at 1 m | Game zoom currently locked out by per-frame FOV lock |
| Comfort | Vignette from **translation speed only** (hysteresis 0.08/0.02 m/s); snap/smooth turn = presentation yaw offset that subtracts native body-yaw changes; tracking anchor captured **yaw-only, once per context** (3 stable frames) | F5 recenter; no vignette/turn |
| Discovery method | Ghidra on WinPC + symbolized Mac build to name functions; fixed RVAs validated at runtime by SEH-guarded **byte-prefix compare**; refuse to arm on mismatch; one real signature scan (frame limiter) requiring exactly one match | None yet |
| Process | Pure math modules in `stereo/` with deterministic `ctest`; diagnostics `off/normal/deep` (player default = zero logging); bounded self-terminating probes; "no headset claim without a headset test" | Hotkeys + VRLOG; no unit tests |

## Lessons that change our plan

1. **Viewmodel**: stop testing distance. Add Draw* hooks and build a draw signature
   (call-site return address inside `BFBC2Game.exe`, VS bytecode hash, z/alpha state,
   *and whether the draw's projection differs from the global VP*). If the 1P weapon
   uses its own P — very likely, given that it rendered distorted in Phase 3 under a
   correction that assumes the global VP — that is both the discriminator and the bug.
2. **Grip math**: full rigid delta, head-local, before the eye offset, both eyes share
   one adjusted World. Rotation-only delta pivots about the eye (their dead-end #3).
3. **Per-eye P**: replace the game's P per eye from `GetProjectionRaw` with the game's
   own near/far, instead of widen-factor + bounds. Game FOV animation then cannot reach
   the eyes at all; intentional zoom returns as an `M00/M11` scale.
4. **Pop-in**: only an engine hook on the camera/frustum path fixes it. This is our first
   engine-RE item (Ghidra + MinHook + prefix validation), and it unlocks Phase 7.
5. **HUD**: separate RT + OpenVR overlay (quad, HMD-relative transform). Crosshair as a
   per-eye world sprite at fixed range — no depth or raycast needed.
6. **Input / aim**: write into the engine's input frame, not `SendInput`/dinput. Body
   follows gun via bounded look deltas. Fire basis swapped at the fire core.
7. **Discipline**: a pure math module with unit tests (their non-commuting yaw/pitch
   tests caught multiply-order bugs that cost headset runs — we have the same
   row/column-major exposure), diagnostics levels, bounded probes.

## Dead-ends they recorded (don't repeat)

- Distance/proximity as viewmodel discriminator; `center1pHands`-style template shortcut.
- Rotation-only grip delta; grip *orientation* for gun direction (use aim); full rigid
  hand attachment (lever arm below the hand — use orientation-only + position).
- Capturing the tracking anchor every frame → image pins to the headset.
- Shader-constant mismatch treated as a frame failure → killed the session; per-draw skip instead.
- Naive per-eye views on view-dependent texgen (water): bands; fold the eye residual into P.
- Synthesizing an elbow via the solver's elbow-point arg; per-weapon wrist relation cached globally.
- SSGI shipped disabled; editing user config files for the frame limiter.

## Their architecture notes worth keeping in mind

- `XR_REFERENCE_SPACE_TYPE_VIEW` at the predicted display time as the head reference for
  hands (OpenVR equivalent: HMD pose from the same `WaitGetPoses`).
- Cull/pose ordering: the pose must be acquired **before** the engine's cull pass; acquiring it
  in a draw callback is too late for that frame (`D3D8StereoPairProbe.cpp:2102`).
- Draw eligibility for stereo: colour target exactly backbuffer-sized, non-MSAA, matching
  depth — excludes shadow/reflection/aux passes automatically.
- Skinned shaders: read back c0–c3, verify they equal `transpose(W·V·P)` from observed
  matrices, only then write per-eye variants; never touch c4+.
