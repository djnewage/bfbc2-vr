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

- The reflection layout (`ITypedObject::getType` at vslot 3, `FieldInfoData`) — `tools/ghidra/FindFrostbiteTypes.java` output pending.
- Statics `GameContext 0x1570C40`, `WorldRender 0x156B7F4` — inside the image, unverified.
- Every class/field offset in `bc2-engine.md` — unverified.
- Which shader names `c185`/`c189` — `shaders` dump pending (first in-game run on this branch).
- The render-pass order and which pass the camera election picks — `passes` dump pending.
- World scale (`units_per_metre`, assumed 1.0 at `draw_policy.h:206`).
- Whether `bone0` carries bind space or view — `bfbc2vr_draws_*` prints it; nobody has recorded the answer.
- `GetGlobalVariable("Game.AutoAimEnabled", …)` at `.text:00567B49` — a named-variable registry, never probed.
