// Controller-driven gameplay input: movement, actions, and snap turn.
//
// Stage 1 of the input work (see docs/controls.md). The weapon already follows
// the right controller; this makes the controllers actually play the game.
//
// Everything goes through synthetic OS input (os_input.h) because no BC2
// engine input structure is publicly known - BFVR's route into BF1942's own
// input frame is not available to us. Consequences that shape the design:
// nothing is injected unless the game is the foreground window, and every
// pressed key is released on any exit path.
//
// Stage 2 (body follows gun) will add the aim convergence loop here; the snap
// turn is already written the way that loop needs - as a view rotation through
// camover::apply_view_offset, not as a mouse turn.
#pragma once

#include <cstddef>
#include <cstdio>

namespace vrinput {

// Called once per presented frame, from camera_override's frame boundary,
// after the head deltas are computed and before the correction is rebuilt.
void on_present();

// Release everything currently held. Safe to call at any time.
void release_all();

bool command(const char* cmd, const char* args, char* reply, size_t reply_size);
void status(FILE* f);

// Yaw the aim loop has commanded that the game camera has not yet shown -
// mouse counts take a few frames to land. The view compensation must be
// applied against what has LANDED, not what was commanded, or the view jumps
// ahead of the body by this amount and snaps back as the camera catches up.
float aim_inflight();

} // namespace vrinput
