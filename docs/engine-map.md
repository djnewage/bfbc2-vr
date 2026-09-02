# Engine map — Frostbite 1.5 as shipped in BFBC2Game.exe

**Rule of this document:** every row in a *Verified* table cites the dump, log line or Ghidra
address that proves it, and was measured on **this** build. Anything else lives under *Open
questions*. `docs/bc2-engine.md` is the hypothesis list this map is checked against; it is not
evidence.

Singleplayer only. Every tool referenced reads memory the way `memscan` always has, and nothing
here is ever used against multiplayer or Venice Unleashed.

## Binary

| Fact | Value | Proof |
|---|---|---|
| Executable | `BFBC2Game.exe`, 19,703,744 bytes, x86 | `tools/recon.ps1`, `docs/recon.md` |
| Image base / size (runtime) | `0x00400000` / `0x1970000` (ends `0x1D70000`) | header of every `bfbc2vr_draws_*.txt` |
| Reflection type names retained | **890 distinct `*Data` identifiers**, incl. `ShotConfigData`, `CameraData`, `SoldierEntity`, `EntityData` ×182 | `grep -a` over the exe, 2026-09-02 |
| `bc2-engine.md` static `Client 0x180AD880` | **not an image address** on this build (past `0x1D70000`) | image geometry above |
| `.text` is **SteamStub-encrypted on disk** | entropy 8.00; entry point `0x019142EE` inside a section named `.bind` (entropy 7.99). Static analysis of engine *code* needs a runtime image dump | `tools/ghidra/explore_typeinfo.py` section table, `build/ghidra/out/type_registration.txt` (the constructor decompiles as junk) |
| Static initialisers live in an unencrypted section named **`ctr`** | RVA `0xF87000`, entropy 6.25; every type registration is a `push <TypeInfoData>; mov ecx,<TypeInfo>; call` sequence there — 2,081 sites, 941 to ctor `0x00500880`, 273 to `0x0043C390` | `type_registration.txt` |
| Reflection data has its own sections: **`typeinfo`** (RVA `0x17F1000`, 0x22D18 B) and **`fieldinf`** (RVA `0x1814000`, 0x34E60 B) | unencrypted; **readable from the file with no runtime** | `tools/ghidra/dump_reflection.py` |

## Rendering — constant registers

| Fact | Value | Proof |
|---|---|---|
| `c185..c188` holds a view-projection | rows orthonormal to 6 dp across a 142° turn; translation unchanged | `docs/phase2-results.md`, `bfbc2vr_diff_00.txt` |
| `c189..c192` holds camera-to-world | translation constant under rotation (a view matrix's would move) | same |
| `c185..c192` is written by **every geometry pass** | shadow, reflection and main scene all write it; "last writer" collapsed the world field 58.9° → 18.5° | `bfbc2vr.log` 2026-09-01 (`cam-yaw-rejected=15472/15960`), `docs/controls.md` |
| Overriding `c185..c192` does not move the world | BC2 pre-multiplies WVP per object on the CPU (~456 draws/frame rewrite `c6..c18`) | `src/camera_override.h:5-11` recording |
| Global camera block is row-major; compiled shader constants are column-major | | `docs/phase3-results.md:37` |
| Vertex shaders with CTAB | 65 parsed, 0 stripped (2026-08) — superseded by the `shaders` dump | `src/shader_registry.h:36`; `bfbc2vr_shaders_<stamp>_NN.txt` |

## Rendering — passes and projections

| Fact | Value | Proof |
|---|---|---|
| Depth slices | ~670 of ~716 scene draws/frame use near = 7.48 m or 21.34 m; 46 use 0.1 m; `c185` carries only the 0.1 m projection | `docs/viewmodel-census.md:96-118`, `bfbc2vr_draws_08.txt` |
| Native singleplayer field | tan 0.5536 × 0.4152 (58.9° × 45.1°, 4:3); `settings.ini Fov` has no effect in the campaign | `[comp] game tangents` log line; `docs/viewmodel-census.md:129` |
| Culling wall | beyond ~58°×45° the widen trick shows void; the cull frustum follows the engine FOV field | `docs/console.md:51-84` |
| Weapon draws | bone-skinned, own FOV tan 0.357 × 0.268; arms+gloves shader hash `76fbfb1ef6146ed9` | `docs/viewmodel-census.md:136-142` |

## Reflection system — measured layout

Read off the file by `tools/ghidra/dump_reflection.py`; full dump in `docs/recon/reflection-bfbc2.txt`
(**1,900 types, 1,381 with value tables, 9,039 typed fields; every consistency check passes**).

| Fact | Value | Proof |
|---|---|---|
| `TypeInfoData` (class) | 24 B: `{name*, size<<16\|flags, module*, 0x0001\|count<<8\|align, 0, 0}`; flags `0x29` = class with embedded field-table pointer at +24 (28 B) | 281 of 281 flags-`0x29` records carry it; 0 of 960 flags-`0x35` do |
| `TypeInfoData` (scalar) | flags `0xC0xx`/`0x41xx`: `+12` = alignment, `+16` = size (plain), `+20` = 0 | `Boolean..Float64, String, FileRef` region `0x01BF144C..` |
| `TypeInfoData` (enum) | flags `0x179`, size 4; pointer at +24 to a table whose `offset` column is the enumerator value | `CoreLogLevel` → `CllNone=0 … CllDebug=9` |
| `FieldInfoData` | 24 B: `{name*, flags\|arraySize<<16, fieldType*, secondaryType*, offset, attributes*}` | tables **tile** (1,377 of 1,380 adjacent pairs end exactly where the next begins) |
| Field-table linkage | 520 records embed the pointer at +24; **861** get it from the `ctr` initialiser (`push <table>` beside `push <record>`); none ambiguous | `dump_reflection.py` counters |
| `fieldType` points at the **runtime `TypeInfo` object**, not the record | objects are zero on disk (typeinfo padding / `.data` BSS); 1,468 mapped via `mov ecx` in `ctr` | same |
| Scalar objects (registered from encrypted `.text`, no static ref) | laid out in record order: `0x01BF1354 + 0x10·k` = `Boolean, Uint8, Int8, Uint16, Int16, Uint32, Int32, Uint64, Int64, Float32, Float64, String, FileRef, Guid`; `0x01562D60 + 0x14·k` = `Vec4, Quat, Plane, LinearTransform, Mat4, AxisAlignedBox`; `Vec2 @0x0155E0AC`, `Vec3 @0x01562ADC` | every mapping's **measured field-packing size equals the record size** (20 of 20 with uses) |
| Array fields | field flags `0x48`; type = `ArrayBase`, element type in `secondaryType` | e.g. `SoldierWeaponData.WeaponStates: ArrayBase<WeaponStateData>` |
| Classes the engine map needs | `ShotConfigData` (0x50): `Vec3 InitialSpeed @0, InitialDirection @0x10, InitialPosition @0x20`, `Boolean ForceSpawnToCamera @0x49`, `ProjectileEntityData* @0x44` | `reflection-bfbc2.txt` |
| | `SoldierWeaponData` (0x130): `Float32 ZoomRenderFov @0x30`, `RenderFov @0x3C`, `WeaponFiringData* WeaponFiring @0x98`, `FirstPersonCameraData* @0x80` | same |
| | `GameRenderSettings` (0xA4): **`Float32 ForceFov @0x18`**, `NearPlane @0x10`, `ViewDistance @0x0C`, `Boolean LockView @0x81` | same |
| | `LevelData` (0x3C0): `Float32 DefaultFOV @0x1F4`, `InfantryFOVMultiplier @0x1F0` (the poke-found object's `45.1 = 55 × 0.82` is this) | same |
| | `CameraData` (0x40): occlusion/fade only — **no FOV field**; `GameSettings.AutoAimEnabled @0x9C` | same |
| `bc2-engine.md` claims now **verified**: `ShotConfigData` 0x00/0x10/0x20; `SoldierWeaponData.m_zoomRenderFov +0x30`, `m_renderFov +0x3C` | | above |
| Not reflected at all (runtime classes): `SoldierEntity`, `ClientPlayer`, `RenderView`, `ClientSoldierWeapon`, `AnimatedSoldier`, `PlayerManager` — no such type-name strings | so their offsets in `bc2-engine.md` can only be checked at runtime | `frostbite_types.txt` controls |

## Engine objects

| Fact | Value | Proof |
|---|---|---|
| Camera object vtable | `0x014755C8`; live vertical FOV at `+0x50`; `+0x60`/`+0x70` = settings 55; `+0x7C` = 45.1 (×0.82 infantry) | `docs/console.md:51-84`, poke-verified |
| Its object base | `0x1BF90648` on three launches, but **heap** — no pointer path yet | same |
| It is **not** `dice::RenderView` | `RenderView` keeps `m_fovY` at `+0x24`; ours is at `+0x50` | `docs/bc2-engine.md` self-check |

## Input

| Fact | Value | Proof |
|---|---|---|
| Index thumbstick under legacy input | `rAxis[0]`, click bit 32 — **not** axis 3 / bit 35 | `legacy_bindings_index_controller.json`; 45 s capture 2026-09-01 |
| Stick deflection > 0.8 asserts bit 32 | `boolean_threshold` binding onto `axis0_press`, hysteresis 0.8/0.7 | same file |
| Mouse gain | 334–406 counts/radian (spread = the game's own smoothing) | `[turn]` log lines |

## Open questions (not yet verified on this build)

- The registry's linked list (`TypeInfo::m_next`) and its static head: the constructor `0x00500880` is in encrypted `.text`. Needs a **runtime image dump** (the mod is already in-process) and a second Ghidra pass over the decrypted image.
- `ITypedObject::getType` at vslot 3 — runtime only.
- Statics `GameContext 0x1570C40`, `WorldRender 0x156B7F4` — inside the image, unverified.
- Offsets of the *runtime* classes in `bc2-engine.md` (`ClientControllableEntity.m_cameraLocalSpace`, `ClientWeaponFiringEffects.m_shootSpace`, `RenderView.*`) — not reflected; runtime only.
- Whether writing `GameRenderSettings.ForceFov` is the sanctioned lever the FOV hunt has been emulating, and whether the poke-found object (vtable `0x014755C8`) *is* the render-settings instance. Test: find the `GameRenderSettings` instance at runtime and compare `+0x18` against the value `fovfind` locates.
- Which shader names `c185`/`c189` — `shaders` dump pending (first in-game run on this branch).
- The render-pass order and which pass the camera election picks — `passes` dump pending.
- World scale (`units_per_metre`, assumed 1.0 at `draw_policy.h:206`).
- Whether `bone0` carries bind space or view — `bfbc2vr_draws_*` prints it; nobody has recorded the answer.
- `GetGlobalVariable("Game.AutoAimEnabled", …)` at `.text:00567B49` — a named-variable registry, never probed.
