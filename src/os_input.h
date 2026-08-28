// Synthetic OS input, and the rules for when it is allowed.
//
// BFVR writes gameplay input into BF1942's own normalized input frame and
// deliberately never synthesises OS input for it. We cannot: no BC2 input
// structure is publicly known (docs/bc2-engine.md), and the published static
// offsets are null in this build. So controller-driven movement, firing and
// aiming go through SendInput - with two rules that are not optional:
//
//   1. NEVER inject unless the game window is the foreground window. Otherwise
//      keystrokes and mouse motion land in whatever the user is actually doing.
//   2. Everything pressed must be released on any exit path, or a key stays
//      down after the mod stops driving it.
//
// Keys are sent as SCANCODES, not virtual keys: raw-input and DirectInput
// games (this one included) read scancodes and ignore VK-only injection.
#pragma once

#include <windows.h>

namespace osinput {

// The game's own top-level window (cached, revalidated). Null if not found.
HWND game_window();

// True if the game window is the foreground window. Attempts to bring it
// forward first; a false return means "do not inject anything".
bool focus_game();

// True if the game is already foreground - a pure query that never steals
// focus. This is the per-frame gate; focus_game() is for explicit commands.
bool game_is_foreground();

void send_key(WORD vk, bool down);
void send_mouse_button(bool right, bool down);

// Relative mouse motion, in mouse counts. Tagged with a magic dwExtraInfo so
// our own injected motion can be told apart from the player's real mouse.
void send_mouse_move(int dx, int dy);
constexpr ULONG_PTR kInjectedTag = 0xBFBC2415;

} // namespace osinput
