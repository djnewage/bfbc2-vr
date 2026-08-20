// File-driven command channel + status dump, so the mod can be driven and
// observed WITHOUT anyone at the keyboard or in the headset.
//
//   bfbc2vr_cmd.txt     (next to the log)  one command per line; consumed and
//                       truncated once executed. Replies go to the log.
//   bfbc2vr_status.txt  rewritten every ~2 s with the live state.
//
// Commands (see console.cpp dispatch):
//   status                 force a status write
//   widen <x> | auto on|off | mode <n> | push <x> | ownproj on|off | bones on|off
//   recenter | census | shot
//   scan <lo> <hi> | scanv <value> [eps] | refine <value> [eps] | changed | unchanged
//   list [n] | poke <hexaddr> <float> | lock <hexaddr> <float> | unlock | watch <hexaddr>
//   fovhunt [factor]       autonomous hunt for the engine's FOV (see memscan.h)
//   fovlock <factor>       write factor*original into the hunt's best hit every frame
#pragma once

namespace vrcmd {

// Poll the command file (every few frames) and refresh the status file.
void on_present();

} // namespace vrcmd
