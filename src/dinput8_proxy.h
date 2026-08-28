// DirectInput8 wrappers - the input half of the mod.
//
// BFBC2Game.exe imports dinput8.dll and calls DirectInput8Create, and has NO
// raw input at all (GetRawInputData / RegisterRawInputDevices are absent from
// the binary), no GetCursorPos, no GetKeyboardState, no GetAsyncKeyState. So
// the device state DirectInput hands the game is the place to inject, and
// injecting there beats SendInput on every axis that matters: exact counts
// (no Windows pointer acceleration), no dependency on the game being the
// foreground window, and no possibility of leaking keystrokes into whatever
// the user is actually doing.
//
// Two injection rules that make whole classes of bug impossible:
//
//   * Keyboard state is OR-ed in, NEVER written as zero. "A key we release
//     must not clobber a key the player is physically holding" is then true by
//     construction rather than by care - and if this module dies, the player's
//     own keyboard is untouched.
//   * Mouse counts are ADDITIVE and consumed exactly once from the bus, so the
//     player's real motion in the same sample survives and no injected count
//     is ever delivered twice or lost. A dropped count would silently corrupt
//     the counts-per-radian measurement the whole turn system rests on.
//
// Caveat worth stating plainly: a DirectInput8Create import proves DirectInput
// is INITIALISED, not that it feeds gameplay - some titles of this era create a
// DI keyboard for text entry only. The poll counters in the status file answer
// that within seconds of gameplay, per device class.
#pragma once

#include <windows.h>

namespace dinput8proxy {

// Called from DllMain when this module is loaded under the dinput8.dll name.
void init();

// The real DirectInput8Create, wrapped.
HRESULT WINAPI create(HINSTANCE inst, DWORD version, REFIID riid, void** out, void* outer);

// DllGetClassObject is a genuine side door: CoCreateInstance(CLSID_DirectInput8)
// reaches DirectInput without ever calling DirectInput8Create. Wrapped too.
HRESULT WINAPI get_class_object(REFCLSID rclsid, REFIID riid, void** out);

// Forward an export we do not interpret to the real dinput8.
FARPROC forward(const char* name);

} // namespace dinput8proxy
