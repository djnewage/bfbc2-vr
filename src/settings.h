// Settings that survive a relaunch.
//
// WHY THIS EXISTS. Every runtime setting used to reset to a compiled-in default
// on launch, and twice that produced an in-headset report of "feature X doesn't
// work" when the feature was simply off - once for `grip`, once for `aim`. Each
// time it cost a full test cycle to discover a default rather than a bug. A new
// DLL requires a relaunch, so anything tuned live evaporated exactly when it was
// most needed.
//
// THE SHAPE. Every setting already has a command that both sets it and reports
// it, and the console already sees the verb and its arguments - so the config is
// just a list of command lines in the same syntax as bfbc2vr_cmd.txt:
//
//     grip on
//     aim kp 0.30
//     hud dist 1.80
//
// Capture happens in one place (after the console dispatches a command), so no
// module needs a new API and the stored value is by construction something the
// command layer accepts. Loading replays the lines through the same dispatcher.
//
// Only verbs on an allowlist are persisted: most commands are transient or
// diagnostic (`shot`, `census`, `recenter`, the memory scanner) and replaying
// them at startup would be at best useless and at worst destructive.
#pragma once

#include <cstddef>

namespace settings {

// Read the file. Safe before anything else is initialised; it only parses.
void load();

// Replay the loaded lines through the console dispatcher. Deliberately separate
// from load(): this must happen on a frame, not during loader lock, so nothing
// touches OpenVR or D3D from inside DllMain.
void apply_pending();

// Record a command that was accepted, if its verb is persistable. Called by the
// console for every handled command.
void note(const char* cmd, const char* args);

// Write the file. Called after a change; also available as the `save` command.
void save();

// True if there is anything waiting for apply_pending().
bool has_pending();

// For the status block.
void status(struct _iobuf* f);

} // namespace settings
