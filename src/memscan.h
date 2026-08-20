// In-process memory scanner + autonomous FOV hunt.
//
// We live inside BFBC2Game.exe, so the Cheat-Engine workflow (scan for a
// value, change something, refine, poke, watch) can run as a closed loop with
// NO human: the verification signal is the world projection we already
// recover from VP every frame. If poking a candidate changes the recovered
// tangents, that address feeds the engine's projection - and because the
// engine derives its cull frustum from the same camera, it almost certainly
// feeds culling too (the thing the widen trick cannot reach).
//
// HUNT: candidate value sets are derived from the current tangents (degrees,
// radians, half-angles, tangents, projection factors a/b, the ini literals).
// For each matching writable address, in turn: remember original, write
// original*factor (or /factor for a/b), wait a few frames, compare tangents,
// restore, record a hit if they moved. Runs from on_present, bounded.
//
// Writes are restricted to committed, writable, non-guard private/mapped
// pages outside our own DLL; every access is SEH-guarded. A wrong poke can
// still crash the game - that is the accepted cost, paid by the hunt and not
// by a human's afternoon.
#pragma once

#include <cstddef>
#include <cstdio>

namespace memscan {

// Command dispatch; returns true if `cmd` was ours. Reply written to `reply`.
bool command(const char* cmd, const char* args, char* reply, size_t reply_size);

// Frame tick: advances the hunt state machine, applies locks, samples watches.
void on_present();

// Append scanner/hunt state to a status file.
void status(FILE* f);

} // namespace memscan
