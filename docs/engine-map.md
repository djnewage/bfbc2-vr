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
| `.text` is **SteamStub-encrypted on disk** | entropy 8.00; entry point `0x019142EE` inside a section named `.bind` (entropy 7.99). The mod's `dumpimage` verb writes the decrypted in-memory image (`.text` entropy 6.32, `unreadable=0`), which is what Ghidra should analyse | `tools/ghidra/explore_typeinfo.py` section table, `build/ghidra/out/type_registration.txt` (the constructor decompiles as junk) |
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
| Shaders | 65 vertex + 145 pixel exist at launch (menu/core); **1,711 vertex + 3,920 pixel** once a level is loaded, all with CTAB | `bfbc2vr_shaders_20260902-094431_00.txt` / `_01.txt` |
| **`c185..c210` is an anonymous per-view constants block** | 44 vertex shaders declare `constants c185+26` (bound 13,605 times); its first 8 registers are the VP + camera-to-world the mod reads. Named shaders never see `c185` — they take `viewProjMatrix` at `c0`/`c4`/`c8`, `cameraPos c4/c8`, `viewMatrix c4+2/c8+2`. In skinned shaders `boneMatrices` (`c5..c21 +180`) overlaps the same registers | `_01.txt` "register -> names" index |
| Named transforms | `worldViewProjMatrix c0+4 / c4+4`, `viewProjMatrix c0+4 / c4+4 / c8+4`, `worldMatrix c0+3 / c4+3`, `worldViewProj c0+4`; `boneMatrices` at 9 different bases | same |
| Gun-body shader `2906751E3B758F6F` | `viewProjMatrix c0+4`, `outdoorLightShadowTransform0 c5+3`, `boneMatrices c9+180` (bones carry the world; no `worldMatrix`) | `_01.txt` |
| Vertex shader families | 28 `bones + constants + worldViewProjMatrix` (soldiers/weapons); 15 with no transform (post-process quads: `g_focalLen`, `radialBlur*`, `screenScale`); 11 `constants + viewProjMatrix` (megashaders, world in the blob); 4 `bones + worldMatrix + WVP`; **3 `viewProjMatrix + worldMatrix` separately** — the cleanest VR correction targets; 3 WVP-only; 1 `worldViewProj` | same |
| Post chain (pixel samplers) | `zBuffer`, `zBufferHalfRes`, `depthTexture`, `dofBlurTexture`, `exposureTexture`/`oldExposureTexure`, `tonemapBloomTexture`, `filmGrainTexture`, `flareTexture`, `avgColorTexture`; GI/lightmaps as `sampler_duster*` and `sampler_radiosityMap`; `sampler_BackgroundTextureY/R/B` = YUV video | same |

## Rendering — the frame, pass by pass

Measured by `passes 3` in a level, 2026-09-02 (`bfbc2vr_passes_20260902-094431_01.txt`). 31 passes,
identical across frames. A pass = a contiguous run of draws with the same render-target set.

| Pass | Target | Draws | What it is |
|---|---|---|---|
| 01 | backbuffer `X8R8G8B8` + `D24S8`, full clear | 0 | frame start |
| 02–04 | 3 × 2048² `'NULL'` colour + `D16` depth, `clears=Z` | 19 / 3 / 14 | **three shadow cascades** (depth-only via the NULL target). **Write no camera block.** |
| 05 | 256×32 `R32F` | 1 quad | lookup / histogram |
| **06** | 2048×1536 **`A16B16G16R16F`** + **`'INTZ'`**, full clear | 38, 25+ shaders | **main opaque scene** — HDR with readable depth. **Writes no camera block.** |
| 07, 09 | 2048×1536 `R32F` | 1 quad | depth linearise |
| 08 | HDR again, no clear | 13 | second depth slice / decals |
| 10 | 1024×768 HDR + INTZ | 5 | half-res (particles) |
| **11** | HDR, **`clears=Z\|STENCIL`** | 15, incl. gun-body `2906751E…` | **first-person weapon pass.** Depth cleared so it draws over the world. **The only pass that writes `c185`** (2 writes/frame, `near=0.10`), and the one the election picks. |
| 12–29 | 512×384 → 1×1, `A16B16G16R16F` and `R32F` | 1 quad each | bloom mip chain, exposure/luminance reduction |
| 30 | backbuffer, `clears=Z\|STENCIL` | 50 | HUD + final composite |

Consequences: the camera block at `c185` is the **viewmodel** camera (which is why the census found
it "carries only the 0.1 m projection" while 670 world draws use 7.48 / 21.34 m — the world passes
never upload it); the shadow cascades are **not** the contaminator that collapsed the world field
yesterday (they write nothing to `c185`); and the world is rendered HDR with INTZ depth, so a stereo
implementation has real depth to work with and passes 12–29 are screen-space.

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
| **Runtime `TypeInfo` object** (from the `dumpimage` image, where they are populated) | `+00` vtable (one per kind: `0x01412E18` class ×962, `0x0140C710` embedded-table class ×281, `0x01412888` enum ×231, `0x01412768` ×486, one each per scalar), `+04` `TypeInfoData*`, **`+08` `m_next`**, `+0C` runtime id, `+10` per-class function, **`+14` `m_super`** (except the `0x0140C710` kind, which keeps its vtable there), `+24` runtime `FieldInfo*` | `tools/ghidra/dump_reflection.py --live`; e.g. `AIMeshTestData→EntityData`, `AISettingsData→Asset`, `SoldierEntityData→ControllableEntityData`, `LevelData→WorldData` |
| **Registry head** | the static **`0x0154D4B4`** (`.data`, RVA `0x114D4B4`) holds the first `TypeInfo`; following `+08` visits **1,986 nodes** and ends in null. The last-registered type (`WorldBuildView`) is the head, so the constructor prepends | live image 2026-09-02 |
| Not reflected at all (runtime classes): `SoldierEntity`, `ClientPlayer`, `RenderView`, `ClientSoldierWeapon`, `AnimatedSoldier`, `PlayerManager` — no such type-name strings | so their offsets in `bc2-engine.md` can only be checked at runtime | `frostbite_types.txt` controls |

## Engine objects

| Fact | Value | Proof |
|---|---|---|
| The live FOV float | at `0x1BF90648 + 0x50` on three launches; `+0x60`/`+0x70` = settings 55; `+0x7C` = 45.1 (×0.82 infantry); poke-verified, cull frustum follows | `docs/console.md:51-84` |
| **Correction:** vtable `0x014755C8` does **not** own that float | its deleting destructor `FUN_00c439c0` frees **0x48 bytes**; the object is a two-list container (intrusive heads at `+0x14`, `+0x30`). `+0x50..+0x7C` is the *next heap allocation*. The vtable/FOV association in `console.md` was adjacency, not membership | `docs/recon/decompiled-live.txt` |
| **`GetGlobalVariable`** | **`0x004F4C40(const char* "Module.Field", TypeInfo* type) → void*`** (worker `0x004F4600`): splits on `.`, finds the module in a table on the registry object (`+0x28`/`+0x2C`), then resolves the field **by name through reflection** on the module's instance (`FUN_00429c00`). Callers cache the pointer in statics (`DAT_0156eed0 = (float*)GetGlobalVariable("Render.EdgeModelLodScale", &Float32)`) | same |
| **`"Render.ForceFov"` resolves and works** | `gvar` (console verb calling `0x004F4C40` with `this = *(0x0155E05C)`) returned `Render.ViewDistance @1BF500BC = 20000`, `NearPlane @1BF500C0 = 0.1`, **`ForceFov @1BF500C8 = -1.0`** (the "not forced" sentinel), `EdgeModelLodScale @1BF500F0`, `Renderer @1BF5011C = 1` — every address at its reflected offset from instance base **`0x1BF500B0`**. `gvarset Render.ForceFov 100` moved the recovered world field from `57.9 × 45.1` to **`115.6 × 100.0`**: `ForceFov` is the **vertical field in degrees**, and it is the engine-sanctioned lever the FOV hunt was emulating | `bfbc2vr.log` 2026-09-02 11:27 |
| Why the shape scans missed the instance | they required `0 ≤ ForceFov ≤ 180`; the sentinel is `-1.0` | `find_render_settings.py` |
| TypeInfo constructors, decompiled | `FUN_0043c390`: `this[2] = s_first; s_first = this` with `s_first = 0x0154D4B4` — **`m_next` at +08 confirmed in code**. `FUN_00500660`: `+10` fn, `+14` super, `+24` fields, `+28` first child, `+2C` next sibling | `docs/recon/decompiled-live.txt`, `type-registration.txt` |
| Codebase name | allocator source paths read `C:\monkey\RomePC.Nightly.Code.SteamClient\TnT\Code\External\EA\EASTL\…` — Rome, Steam client build; containers are EASTL | same |

## Input

| Fact | Value | Proof |
|---|---|---|
| Index thumbstick under legacy input | `rAxis[0]`, click bit 32 — **not** axis 3 / bit 35 | `legacy_bindings_index_controller.json`; 45 s capture 2026-09-01 |
| Stick deflection > 0.8 asserts bit 32 | `boolean_threshold` binding onto `axis0_press`, hysteresis 0.8/0.7 | same file |
| Mouse gain | 334–406 counts/radian (spread = the game's own smoothing) | `[turn]` log lines |

## Open questions (not yet verified on this build)

- Which class owns the live FOV float at `0x1BF90648+0x50`: not vtable `0x014755C8` (see correction). Lead: it is the allocation after a 0x48-byte list container; read the dword at `0x1BF90648+0x48` next launch — if that is the FOV object's own vtable, its destructor names its size.
- Whether the cull frustum follows `Render.ForceFov` the way it followed the poke (no grey void at the periphery at 100°) — needs an in-headset look.
- Whether `Render.ForceFov` persists across level loads, or the settings instance is re-read from `LevelData.DefaultFOV`/`InfantryFOVMultiplier` on each load.
- Statics `GameContext 0x1570C40`, `WorldRender 0x156B7F4` — inside the image, unverified.
- Offsets of the *runtime* classes in `bc2-engine.md` (`ClientControllableEntity.m_cameraLocalSpace`, `ClientWeaponFiringEffects.m_shootSpace`, `RenderView.*`) — not reflected; runtime only.
- Whether writing `GameRenderSettings.ForceFov` is the sanctioned lever the FOV hunt has been emulating, and whether the poke-found object (vtable `0x014755C8`) *is* the render-settings instance. Test: find the `GameRenderSettings` instance at runtime and compare `+0x18` against the value `fovfind` locates.
- What wrote the 18.5° projection to `c185` on 2026-09-01: not the shadow cascades (they write none). In a normal frame only pass 11 writes it. Needs a `passes` capture during a recurrence.
- World scale (`units_per_metre`, assumed 1.0 at `draw_policy.h:206`).
- Whether `bone0` carries bind space or view — `bfbc2vr_draws_*` prints it; nobody has recorded the answer.
- `GetGlobalVariable("Game.AutoAimEnabled", …)` at `.text:00567B49` — a named-variable registry, never probed.
