# Command channel, status file, and the FOV hunt

Added 2026-08-20 so the mod can be driven and observed **without anyone at the keyboard or
in the headset** — the game just sits in a singleplayer level with SteamVR running.

| File (next to the log, in the game dir) | Role |
|---|---|
| `bfbc2vr_cmd.txt` | one command per line; read every ~10 frames, executed, then truncated |
| `bfbc2vr_status.txt` | rewritten every ~2 s and after every command: correction/HMD state, FOV tangents, viewmodel counters, scanner/hunt state |
| `bfbc2vr.log` | every command echoes `[cmd] <name> <args> -> <reply>` |
| `bfbc2vr_eyeL_<stamp>_NN.bmp` / `eyeR` | `shot` — the two eye textures as submitted |
| `bfbc2vr_draws_<stamp>_NN.txt` | `census` — 4-frame draw signature table |
| `bfbc2vr_shaders_<stamp>_NN.txt` | `shaders` — every CTAB, vertex and pixel, plus a register→names index |
| `bfbc2vr_passes_<stamp>_NN.txt` | `passes` — ordered render passes with targets, formats, camera blocks, election |
| `bfbc2vr_snapshot_<stamp>_NN.txt` / `diff` | `consts` — the 256-register mirror with the STABLE/VARYING split |

`<stamp>` is the launch time (`YYYYMMDD-HHMMSS`, also printed in the log header). Before
2026-09-02 every index restarted at 0 per launch and each run silently overwrote the last.

## Commands

| Command | Effect |
|---|---|
| `status` | force a status write |
| `shot` | save both eye textures (BMP) |
| `census` | 4-frame draw census |
| `shaders` | dump every parsed constant table — see *Engine recon* below |
| `passes [n]` | record `n` frames of render passes (default 2, max 8) |
| `consts` / `consts baseline` / `consts diff` | F9 / F10 / F11 without the keyboard |
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

## Engine recon (2026-09-02, branch `engine-recon`)

Three dumps added so the engine is read rather than inferred. None of them changes rendering;
each is inert until its verb is issued.

**`shaders`** — the CTAB the game embeds in every shader, which the mod had been parsing at
`CreateVertexShader` time and discarding. Now kept for every register set (float / int / bool /
sampler), with the `D3DXSHADER_TYPEINFO` class and shape, for pixel shaders as well, and with a
count of how often each shader was bound. The file ends with a **register → names** index for the
vertex float file: for `c185`, every name any shader declares there and how heavily those shaders
are used. That is the declaration behind `kViewProjBase = 185`, which until now rested on watching
values change.

**`passes`** — a pass is a contiguous run of draws with the same render-target set. Per pass: every
bound colour target and the depth-stencil with identity, size, format and multisample; the `Clear`
calls; `BeginScene`/`EndScene`; draw and primitive counts; the vertex shaders used; and every
`c185` projection written during it with its `c189` heading, marked `ELECTED` if it is the one the
end-of-frame camera election adopted. This is the table that decides whether the election is
picking the right pass — a shadow cascade should show as its own smaller pass into its own
target, not as a competitor for "the player's camera".

**`consts`** — the existing F9/F10/F11 constant-mirror tools, driveable from the command file.
