# Command channel, status file, and the FOV hunt

Added 2026-08-20 so the mod can be driven and observed **without anyone at the keyboard or
in the headset** — the game just sits in a singleplayer level with SteamVR running.

| File (next to the log, in the game dir) | Role |
|---|---|
| `bfbc2vr_cmd.txt` | one command per line; read every ~10 frames, executed, then truncated |
| `bfbc2vr_status.txt` | rewritten every ~2 s and after every command: correction/HMD state, FOV tangents, viewmodel counters, scanner/hunt state |
| `bfbc2vr.log` | every command echoes `[cmd] <name> <args> -> <reply>` |
| `bfbc2vr_eyeL_NN.bmp` / `eyeR` | `shot` — the two eye textures as submitted |
| `bfbc2vr_draws_NN.txt` | `census` — 4-frame draw signature table |

## Commands

| Command | Effect |
|---|---|
| `status` | force a status write |
| `shot` | save both eye textures (BMP) |
| `census` | 4-frame draw census |
| `widen <x>` / `auto on\|off` | manual FOV widen factor / automatic headset match |
| `mode <0-3>` / `push <m>` | weapon projection mode / arm's-length push |
| `ownproj on\|off` | per-draw own-projection correction (off = old global path) |
| `bones on\|off` | classifier bone requirement |
| `pos on\|off` / `ipd <m>` / `recenter` / `correct on\|off` | 6DOF, IPD override, F5, F7 |
| `scan <lo> <hi>` / `scanv <v> [eps]` | find writable floats in range / near a value |
| `snapshot` / `changed` / `unchanged` / `refine <v> [eps]` | Cheat-Engine-style narrowing |
| `list [n]` | log the first n candidates with module attribution (exe/heap) |
| `poke <hex> <f>` / `lock <hex> <f>` / `unlock` / `watch <hex>` | write once / write every frame / clear / report in status |
| `fovhunt [factor]` | **autonomous FOV hunt** (below) |
| `hunthits` / `fovlock [factor] [i]` | list hits / lock hit *i* to `original×factor` every frame |

## The FOV hunt

Goal: find the engine's own camera FOV so the game renders **and culls** a headset-sized
field (the widen trick can only stretch what the engine already drew; everything outside its
~58°×45° cone is void — see `viewmodel-census.md` §5).

`fovhunt` derives candidate values from the tangents we recover from VP every frame
(degrees / radians / half-angles / tangents / projection factors a,b / the ini literals
55 and 90), scans all writable memory for each, then for every candidate in turn: writes
`original×1.3` (÷1.3 for a/b), waits 3 frames, reads the recovered tangents again, restores
the original, and records a **HIT** if the tangents moved >5 %. No human needed; results are
`[hunt] HIT` lines and the status file. A hit that is a *source* value (degrees, in the
camera/settings object) will move culling too; a hit that is a cached projection matrix will
move only the render. `fovlock` then holds the best hit every frame; `shot` + `census` show
whether the void is gone.

Risk, stated plainly: poking a wrong float can crash the game. Relaunch and reload the level.

## Result 2026-08-20: the engine FOV found and verified

Two-state hunt (zoom toggled by `rmb`, `scanv` zoomed encoding, `refine` un-zoomed) left a
handful of degree-encoded survivors; poke-verify picked **one heap float holding the camera's
vertical FOV in degrees** (45.1 default): writing 60 made the recovered tangents exactly
tan(30°), and locking it at 100 rendered **116°×100° with no void** — the cull frustum follows.
A structure family (~20 copies, `rad_v` at +0, `tan_h` at +0x350) also tracks it but is derived.

New commands: `fovfind` (input-free re-locate: scan the current vertical FOV in degrees at
±0.05 %, poke-verify every match, adopt the one that moves the tangents — runs automatically
~10 s after the world projection appears), `fov <deg>|auto|off|addr <hex>` (hold the engine FOV
every frame; `auto` = the headset's vertical field from GetProjectionRaw), `dump <hex> [n]`,
`scanptr <hexlo> <hexhi>`, and `list` now prints the raw bits. Four words in the exe's static
data pointed into the object's neighbourhood — the lead for a fixed pointer path.

Known cost: while held, the game's own zoom (ADS) cannot change the FOV; scopes need their
own treatment (BFVR: projection scale + eye-filling reticle quad).

## Session results (2026-08-20, late)

- **In-headset, engine FOV held at the Index's 109° vertical**: full field, no border, no void,
  world holds still when leaning, stereo depth sane. Pitch was inverted → default sign flipped
  (`pitchsign`/`yawsign` commands, F6/F8 keys).
- Camera object: vtable `0x014755C8`, base seen at `0x1BF90648` (same address across three
  launches — allocation looks deterministic), **FOV at +0x50** (the only live field; +0x78 mirrors
  it, +0x7C holds 45.1, +0x60/+0x70 hold the base 55 = settings FOV; 45.1 = 55 × 0.82 infantry
  multiplier; none of those drive the render).
- Weapon FOV: `0x11A9F90C` = 30.0 (vertical), found by `fovfind 1.3 weapon` (poke → weapon
  tangents exactly tan(19.5°)). Holding it equal to the camera FOV (`vmfov follow`) renders the
  weapon natively wide but does **not** fix the stretched left arm and makes the weapon
  unclassifiable (no push) — so it defaults to off.
- Arm deformation: present at camera FOV ≥ 55 with our correction on; the decisive test (our
  correction fully off at 109°) is still pending — the game was paused when it ran.
