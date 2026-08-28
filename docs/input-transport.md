# Input transport: from SendInput to a DirectInput proxy

## Why this changed

Controller input started on Win32 `SendInput`, which was the only thing available without
engine knowledge. It has three problems that no amount of care fixes: it needs the game to be
the foreground window, Windows pointer acceleration distorts injected relative motion (so
counts-per-radian is not even constant), and a stray moment types into whatever the user is
actually doing. After several sessions it was also still **unverified** that it reached the game
at all.

Reading `BFBC2Game.exe` settled it:

| Fact | Consequence |
|---|---|
| Imports `dinput8.dll` / `DirectInput8Create` | DirectInput is the input path to hook |
| `GetRawInputData`, `RegisterRawInputDevices` **absent** | no raw input to compete with |
| `GetCursorPos`, `GetKeyboardState`, `GetAsyncKeyState` **absent** | the game does not poll the OS directly |

So the device state DirectInput hands the game is a better place to inject than the OS input
queue: exact counts, no acceleration, and structurally incapable of leaking keystrokes into
another application.

**Correction (measured, 2026-08-28).** An earlier version of this document claimed the change
also made input *focus-independent*. It does not. The game acquires both devices at FOREGROUND
cooperative level - mouse `0x6` (exclusive | foreground), keyboard `0x16` (foreground |
nonexclusive | nowinkey) - so when the game loses focus DirectInput unacquires them and the game
stops reading input at all, whatever the transport. The other benefits are real and measured;
that one was an assumption and it was wrong.

## Shape

**One binary, two filenames.** The same DLL is installed as `d3d9.dll` (renderer) and
`dinput8.dll` (input). Windows loads them as two independent modules with separate globals, so
`DllMain` reads its own filename and picks a role. The input role initialises almost nothing —
if the game imports dinput8 statically, that copy loads *before* d3d9 and long before any VR
runtime exists. Separate log file, because the two modules hold separate `FILE` handles.

- The real dinput8 comes from `GetSystemDirectoryA()` (already SysWOW64 under WOW64 — never
  hardcoded, never with FS redirection disabled), and we refuse to load any `dinput8.dll` from
  the game folder, which would be ourselves.
- `DirectInput8Create` → `IDirectInput8::CreateDevice` → wrap only `GUID_SysMouse*` /
  `GUID_SysKeyboard*`; gamepads pass through untouched. `QueryInterface` never hands out the
  raw device, or the game would bypass us.
- `DllGetClassObject` is a real side door (`CoCreateInstance(CLSID_DirectInput8)` reaches
  DirectInput without `DirectInput8Create`). It is forwarded and **logs loudly**, so a silent
  bypass shows up as a log line instead of as "injection mysteriously does nothing".

## Two rules that make bug classes impossible

- **Keyboard state is OR-ed in, never written as zero.** "Our release must not clobber a key
  the player is physically holding" is then true by construction — and if this module dies, the
  player's keyboard is untouched.
- **Mouse counts are additive and consumed exactly once.** The renderer `fetch_add`s onto the
  bus; the input thread `exchange(0)`s. Total injected equals total requested whatever the two
  cadences are, which is what preserves "one correction per fresh controller sample". A dropped
  count would silently corrupt the counts-per-radian measurement the turn system rests on.

## The bus

`src/input_bus.cpp` — a page-sized named mapping, POD, all atomics, created-or-opened by
whichever module attaches first (symmetric, no load-order assumption). *Levels* (held keys and
buttons) travel by seqlock and are idempotent; *impulses* (mouse counts) are exactly-once. If
the renderer stops publishing, the input side reports every level released and the buffered-mode
edge diff emits the key-ups by itself — a dead publisher cannot leave a key down.

## Falling back

`path auto|dinput|sendinput|off`, default `auto`: use the wrappers when they report in, else the
old SendInput path with its foreground gate. A partial install is therefore still a working
install — including the case where `dinput8.dll` already exists and is not ours, which the
installer refuses to overwrite.

## Measured behaviour (2026-08-28, in game)

- The proxy loads, wraps the keyboard and the mouse, and the game reads through it continuously
  (`hook=LIVE`, tens of thousands of drains). **DirectInput genuinely feeds gameplay here** - it
  is not a text-entry-only device, which was the main risk in this design.
- Both devices are **buffered** (keyboard buffer 32, mouse 512), so `GetDeviceData` is the live
  path and `GetDeviceState` is secondary.
- Injecting `DIK_W` walks the character: the recovered camera position moved 1.4 world units
  over a 2 s hold, with exactly two events emitted (down and up).
- Mouse counts arrive exactly - `consumed` tracks requested with no loss - and turn the body at
  roughly **334-406 counts per radian**, mildly non-linear across 100-500 counts. That spread is
  in-game smoothing, and it is why the aim loop measures its gain continuously instead of
  trusting one calibration.

## Verifying it

`path` reports whether the hook is live, which device classes have been read, immediate vs
buffered, poll counts, and cumulative consumed counts. `dik w 800` holds one key with no VR in
the loop. `mouse 400` reports requested vs consumed and the measured yaw change.

**The honest caveat:** a `DirectInput8Create` import proves DirectInput is *initialised*, not
that it feeds gameplay — some titles of this era create a DI keyboard for text entry only. The
poll counters answer that per device class within seconds of gameplay, and if both come back
cold the next option is subclassing the game window.
