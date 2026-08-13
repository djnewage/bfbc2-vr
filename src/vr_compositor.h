// Phase 4: frame submission to the SteamVR compositor.
//
// The game renders through DXVK, so every D3D9 surface is secretly a VkImage.
// DXVK's interop interfaces (thirdparty/dxvk/d3d9_interfaces.h) hand us the
// raw Vulkan handles, and OpenVR's compositor accepts Vulkan textures. So the
// path from "game rendered a frame" to "frame is on the HMD panels" is:
//
//   backbuffer --StretchRect--> eye texture (D3D9, ours)
//     --ID3D9VkInteropTexture--> VkImage
//     --TransitionTextureLayout--> TRANSFER_SRC_OPTIMAL
//     --FlushRenderingCommands / LockSubmissionQueue-->
//       IVRCompositor::Submit(left), Submit(right)
//     --ReleaseSubmissionQueue / transition back-->
//
// FIRST LIGHT SCOPE: submit the SAME image to both eyes. Zero stereo depth -
// deliberately. It proves scene-app handshake, Vulkan interop, layout
// discipline, and compositor pacing in one step, with the fewest moving
// parts. Per-eye images are the NEXT step (alternate-eye rendering), and they
// reuse everything here unchanged except which texture gets submitted.
//
// Launch order matters: SteamVR must be running BEFORE the game so DXVK's
// OpenVR extension provider enables the device extensions the compositor
// needs (that is what "Extension providers: OpenVR" in the DXVK log is for).
#pragma once

#include <d3d9.h>

namespace vrcomp {

// Call once when the (real, unwrapped) device is created.
void on_device_created(IDirect3DDevice9* real_device);

// Call before/after the game's device Reset - default-pool resources die.
void on_reset_begin();

// Submit the current backbuffer to both eyes. Call from the Present hook,
// before forwarding Present. No-op until everything is initialized; safe to
// call every frame. Returns true if a frame was submitted.
bool submit_frame();

// True once the scene-app handshake and interop init have all succeeded.
bool active();

void shutdown();

} // namespace vrcomp
