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
