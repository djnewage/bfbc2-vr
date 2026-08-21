# BC2 / Frostbite 1.5 engine structures — what is publicly known

Compiled 2026-08-20 from a survey of public BC2 reverse-engineering work. **None of this is
verified against our build yet**; every offset below is a hypothesis to be confirmed at runtime
with `dump` / `scanptr` (see `console.md`) before any code depends on it. Frostbite classes
are versioned, and most of these come from the 2010 retail era.

**Provenance and the honest caveat.** The public source of BC2 class layouts is the
UnknownCheats BC2 subforum, because the 2010 leaked *dedicated server* package shipped with
full MSVC **PDBs** (`Frost.Game.Main_Win32_Final.pdb`) and people dumped them. The surrounding
tooling there is multiplayer cheating, which this project has nothing to do with — this mod is
singleplayer-only by design (PunkBuster). The class layouts themselves are neutral engine
facts and are the only public record of them. Take the structs, ignore the rest.

**Safety note:** one page encountered during the survey (`odd.blog`, on the server leak)
contains hidden text attempting to instruct an AI agent to exfiltrate `~/.ssh`. Do not feed
that page to an agent.

## The reflection system — the reason offsets should be a last resort

BC2 entities derive from `dice::ITypedObject`, whose vtable slot 3 is `getType()`, and the
type carries a field table:

```cpp
class ITypedObject : public IRefCount { virtual TypeInfo* getType(); };   // vslot 3
struct FieldInfo::FieldInfoData {
    const char*        name;
    MemberInfoFlags    flags;
    unsigned short     arraySize;
    const TypeInfo*    fieldTypePtr;
    const TypeInfo*    secondaryTypePtr;
    unsigned int       fieldOffset;
    const AttributeSpec* attributes;
};
```

So a live object can be asked what it is, and its fields enumerated **by name with exact
offsets, at runtime**. That matches the reflection strings already found in the exe (`Fov`,
`DefaultFOV`, `RenderFov`, `ZoomRenderFov`, `InfantryFOVMultiplier`, `ForceFov`, `CameraFov`,
`SoldierCameraPosition`). Walking this beats hard-coded offsets and survives patches — the
approach to try first. Sibling implementations to copy the walk from:
[CallumCVM/FrostbiteGen](https://github.com/CallumCVM/FrostbiteGen),
[txt231/FBTools](https://github.com/txt231/FBTools) (BF3, 32-bit).

There is also a global named-variable registry (`GetGlobalVariable("Game.AutoAimEnabled", …)`
seen at `.text:00567B49`) worth probing for camera/FOV keys.

## Claimed layouts (retail era — verify before use)

Statics: `GameContext 0x1570C40`, `WorldRender 0x156B7F4`, `Client 0x180AD880`.

| Class | Field | Offset |
|---|---|---|
| `PlayerManager` | `m_clientPlayers` (vector) / `m_localPlayer` | `+0x94` / `+0xB4` |
| `ClientPlayer` | `m_soldier` / `m_controlledControllable` / `m_inputSensitivity` | `+0xC54` / `+0xC68` / `+0xCC8` |
| `ClientControllableEntity` | `m_worldTransform` / `m_health` / `m_weapons` / `m_currentAnimatedWeaponIndex` | `+0x28` / `+0x34` / `+0x260` / `+0x278` |
| | **`m_cameraLocalSpace` (D3DXMATRIX)** / `m_localPersonView` / `m_lastZoomLevel` | `+0x2A0` / `+0x3B8` / `+0x460` |
| `ClientSoldierWeapon` | `m_data` / `m_firingEffects` | `+0x04` / `+0x38` |
| `SoldierWeaponData` | **`m_zoomRenderFov` / `m_renderFov`** | `+0x30` / `+0x3C` |
| `ClientWeaponFiringEffects` | **`m_shootSpace` (D3DXMATRIX)** / `m_shootSpaceDelta` | `+0x30` / `+0x70` |
| | `m_cameraFov` / `m_weaponFov` / `m_fovScaleFactor` / **`m_zoomLevel`** | `+0xF8` / `+0xFC` / `+0x100` / `+0x108` |
| `ShotConfigData` | `m_initialSpeed` / `m_initialDirection` / `m_initialPosition` | `+0x00` / `+0x10` / `+0x20` |
| `AnimatedSoldier` | `m_eyePosition` / `m_soldierTransform` / `m_soldierYaw` / `m_weaponBoneIndex` | `+0x2C0` / `+0x1D0` / `+0x8D8` / `+0x938` |
| `dice::RenderView` | `m_dirtyFlags` / `m_fovX` / `m_fovY` / `m_defaultFov` / `m_nearPlane` / `m_farPlane` / `m_aspect` | `+0x10` / `+0x20` / `+0x24` / `+0x28` / `+0x2C` / `+0x30` / `+0x34` |
| | `m_transform` / `m_viewMatrix` / `m_viewProjectionMatrix` (size `0x470`) | `+0x60` / `+0x230` / `+0x3B0` |

`RenderView::DirtyFlags { ViewMatrix=1, ProjectionMatrix=2, FovConst=4, Frustum=8, All=0xF }`.

## What this changes for us

1. **Our FOV object is not `RenderView`.** Ours is vtable `0x014755C8` with the live vertical
   FOV at `+0x50` (`+0x60`/`+0x70` hold the settings 55, `+0x7C` the 45.1 infantry-multiplied
   value). `RenderView` puts `m_fovY` at `+0x24`. So we are holding a camera/view object one
   level up, and `RenderView` is a separate thing worth locating — its `m_dirtyFlags` is
   exactly the "rebuild projection *and* frustum" lever we currently get for free by writing
   the source FOV every frame.
2. **ADS has a proper signal.** `m_zoomLevel` / `m_lastZoomLevel` and the per-weapon
   `m_zoomRenderFov` / `m_renderFov` would replace our read-back inference (which works, but
   is a deduction from a field we ourselves are stomping).
3. **Phase 7 has a target.** `ClientWeaponFiringEffects::m_shootSpace` is the first-person
   weapon transform *and* the fire basis — the single matrix a controller-held gun needs to
   drive, exactly like BFVR's fire-core basis swap. `ShotConfigData::m_initialDirection` /
   `m_initialPosition` are the projectile terms.
4. **Input remains unknown.** No public BC2 input structure exists. FB2 uses an `EntryInput`
   float array indexed by a concept enum and BC2 likely has the ancestor, but nothing public
   names it. The PDB would settle it (`Dia2Dump -type "dice::EntryInput"`).

## Other resources

- **[FBOneTools](https://github.com/AnirohDev/FBOneTools)** + **[FBOneScripts](https://github.com/AnirohDev/FBOneScripts)** —
  `.dbx` is Frostbite 1 EBX; extracting BC2's weapon `.dbx` and grepping `Fov` gives the
  ground-truth FB1.5 field vocabulary, and per-weapon `ZoomRenderFov` values, offline.
- **[GrzybDev/BFBC2_Hook](https://github.com/GrzybDev/BFBC2_Hook)** — the only current public
  injection prior art for this exe: `dinput8` proxy + Detours + a clean `FindPattern`
  + inline-patch template. Being `dinput8` it coexists with our `d3d9` proxy.
- BC2 shipped **NVIDIA 3D Vision** support (`StereoCrosshairMaxHitDepth` in the FB field
  dumps) — sibling `Stereo*` fields may exist in the engine and be worth hunting.
- `settings.ini` `Fov=55` is vertical and **multiplayer-only**; the campaign ignores it, which
  is exactly what we measured (identical tangents at 55 and 90).

**Dead ends:** Venice Unleashed / Project Rome (closed source, network emulation only, no
client API), the public Cheat Engine table (zero recoil only — no FOV, no pointer paths),
Widescreen Fixer (binary only), and the BC2 map-editor / XML projects (offline data only).
