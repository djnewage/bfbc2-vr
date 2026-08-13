#include "vr_compositor.h"
#include "vr_tracking.h"
#include "logger.h"

#include <windows.h>
#include <openvr.h>
#include <d3d9_interfaces.h>
#include <cstring>

namespace vrcomp {
namespace {

IDirect3DDevice9*     g_device      = nullptr;   // real device, not our wrapper
ID3D9VkInteropDevice* g_interop_dev = nullptr;

IDirect3DTexture9*    g_eye_tex     = nullptr;   // shared by both eyes for first light
ID3D9VkInteropTexture* g_eye_interop = nullptr;

VkInstance       g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_physdev  = VK_NULL_HANDLE;
VkDevice         g_vk_device   = VK_NULL_HANDLE;
VkQueue          g_vk_queue    = VK_NULL_HANDLE;
uint32_t         g_vk_queue_family = 0;

UINT g_width = 0, g_height = 0;
bool g_scene_ok   = false;   // compositor interface acquired
bool g_interop_ok = false;
bool g_enabled    = true;    // F4 kill switch - keeps the game playable if VR path hangs
bool g_wgp_called = false;   // WaitGetPoses must precede the first Submit
unsigned g_submits = 0, g_submit_errors = 0;

// Hang forensics. The level-load freeze gives us no stack, but the log's last
// line does: stage-mark every step of the submit path. With the logger's
// per-line flush, whatever stage the log ends on is where the thread stopped.
// Verbose for the first frames after init/reset, then every 600th frame.
unsigned g_verbose_frames = 10;
void stage(const char* s)
{
    if (g_verbose_frames > 0 || g_submits % 600 == 0) VRLOG("[stage] %s", s);
}

bool init_interop_device()
{
    if (g_interop_dev) return true;
    if (!g_device) return false;

    if (FAILED(g_device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                        reinterpret_cast<void**>(&g_interop_dev)))) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("[comp] device does not expose ID3D9VkInteropDevice - is DXVK actually the backend?");
        }
        return false;
    }

    g_interop_dev->GetVulkanHandles(&g_vk_instance, &g_vk_physdev, &g_vk_device);
    VkQueue q = VK_NULL_HANDLE; uint32_t qi = 0, qf = 0;
    g_interop_dev->GetSubmissionQueue(&q, &qi, &qf);
    g_vk_queue = q; g_vk_queue_family = qf;

    VRLOG("[comp] DXVK interop: instance=%p physdev=%p device=%p queue=%p family=%u",
          (void*)g_vk_instance, (void*)g_vk_physdev, (void*)g_vk_device, (void*)g_vk_queue, qf);
    g_interop_ok = true;
    return true;
}

bool init_eye_texture()
{
    if (g_eye_tex) return true;
    if (!g_device || !g_width) return false;

    // Render target in default pool: lives GPU-side, backed by a real VkImage.
    if (FAILED(g_device->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_eye_tex, nullptr))) {
        VRLOG("[comp] eye texture creation failed (%ux%u)", g_width, g_height);
        return false;
    }
    if (FAILED(g_eye_tex->QueryInterface(__uuidof(ID3D9VkInteropTexture),
                                         reinterpret_cast<void**>(&g_eye_interop)))) {
        VRLOG("[comp] eye texture has no interop interface");
        g_eye_tex->Release(); g_eye_tex = nullptr;
        return false;
    }
    VRLOG("[comp] eye texture ready (%ux%u)", g_width, g_height);
    return true;
}

bool init_compositor()
{
    if (g_scene_ok) return true;
    if (!vrtrack::ensure_init()) return false;   // shares the OpenVR session

    if (!vr::VRCompositor()) {
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[comp] IVRCompositor unavailable (Background session?)"); }
        return false;
    }
    g_scene_ok = true;
    VRLOG("[comp] compositor acquired");
    return true;
}

} // namespace

void on_device_created(IDirect3DDevice9* real_device)
{
    g_device = real_device;

    // Backbuffer size drives the eye texture size for the mirror copy.
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(real_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d = {};
        bb->GetDesc(&d);
        g_width = d.Width; g_height = d.Height;
        bb->Release();
    }
    VRLOG("[comp] device registered, backbuffer %ux%u", g_width, g_height);
}

void on_reset_begin()
{
    if (g_eye_interop) { g_eye_interop->Release(); g_eye_interop = nullptr; }
    if (g_eye_tex)     { g_eye_tex->Release();     g_eye_tex = nullptr; }
    VRLOG("[comp] reset: eye texture released");
}

bool submit_frame()
{
    // F4: VR submission kill switch, usable even mid-hang-recovery. Polled
    // before the enable check so it can turn the path back ON too.
    {
        static bool down = false;
        const bool now = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (now && !down) {
            g_enabled = !g_enabled;
            VRLOG("[comp] VR submission %s (F4)", g_enabled ? "ENABLED" : "DISABLED");
        }
        down = now;
    }

    if (!g_device || !g_enabled) return false;
    if (!init_compositor() || !init_interop_device()) return false;

    // The compositor requires WaitGetPoses before the first Submit ever lands.
    // After that it moves to the END of the frame (post-Submit), which is the
    // conventional order: submit what we have, then block for next frame's
    // poses - instead of blocking up front while holding the game's Present.
    if (!g_wgp_called) {
        stage("first WaitGetPoses");
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        g_wgp_called = true;
    }

    // Backbuffer can change size across level loads (Reset or swapchain
    // rebuild). Resize-check every frame rather than trusting init-time state.
    stage("backbuffer query");
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bbd = {};
    bb->GetDesc(&bbd);
    if (bbd.Width != g_width || bbd.Height != g_height) {
        VRLOG("[comp] backbuffer resized %ux%u -> %ux%u, recreating eye texture",
              g_width, g_height, bbd.Width, bbd.Height);
        g_width = bbd.Width; g_height = bbd.Height;
        on_reset_begin();
        g_verbose_frames = 10;
    }
    if (!init_eye_texture()) { bb->Release(); return false; }

    // 1. Mirror the backbuffer into our eye texture.
    stage("StretchRect");
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(g_eye_tex->GetSurfaceLevel(0, &dst)) || !dst) { bb->Release(); return false; }
    const HRESULT copy_hr = g_device->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
    bb->Release(); dst->Release();
    if (FAILED(copy_hr)) {
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[comp] StretchRect backbuffer->eye failed 0x%08lX", copy_hr); }
        return false;
    }

    // 2. Vulkan handle + current layout of the eye image.
    stage("GetVulkanImageInfo");
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (FAILED(g_eye_interop->GetVulkanImageInfo(&image, &layout, &info))) {
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[comp] GetVulkanImageInfo failed"); }
        return false;
    }

    // 3. Compositor wants TRANSFER_SRC_OPTIMAL. Transition, flush, submit
    //    under the queue lock, release, transition back. The lock rules are
    //    strict: no D3D9 calls while held, or DXVK deadlocks.
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    stage("transition to TRANSFER_SRC");
    g_interop_dev->TransitionTextureLayout(g_eye_interop, &range, layout,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    stage("FlushRenderingCommands");
    g_interop_dev->FlushRenderingCommands();

    vr::VRVulkanTextureData_t vkdata = {};
    // On x86 VkImage is a non-dispatchable handle, already a uint64_t.
    vkdata.m_nImage            = static_cast<uint64_t>(image);
    vkdata.m_pDevice           = reinterpret_cast<VkDevice_T*>(g_vk_device);
    vkdata.m_pPhysicalDevice   = reinterpret_cast<VkPhysicalDevice_T*>(g_vk_physdev);
    vkdata.m_pInstance         = reinterpret_cast<VkInstance_T*>(g_vk_instance);
    vkdata.m_pQueue            = reinterpret_cast<VkQueue_T*>(g_vk_queue);
    vkdata.m_nQueueFamilyIndex = g_vk_queue_family;
    vkdata.m_nWidth            = g_width;
    vkdata.m_nHeight           = g_height;
    vkdata.m_nFormat           = static_cast<uint32_t>(info.format);
    vkdata.m_nSampleCount      = 1;

    vr::Texture_t tex = {};
    tex.handle      = &vkdata;
    tex.eType       = vr::TextureType_Vulkan;
    tex.eColorSpace = vr::ColorSpace_Auto;

    stage("LockSubmissionQueue");
    g_interop_dev->LockSubmissionQueue();
    stage("Submit L");
    const auto err_l = vr::VRCompositor()->Submit(vr::Eye_Left,  &tex);
    stage("Submit R");
    const auto err_r = vr::VRCompositor()->Submit(vr::Eye_Right, &tex);
    stage("ReleaseSubmissionQueue");
    g_interop_dev->ReleaseSubmissionQueue();

    stage("transition back");
    g_interop_dev->TransitionTextureLayout(g_eye_interop, &range,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);

    // End-of-frame heartbeat: pace to HMD refresh and fetch next frame's poses.
    stage("WaitGetPoses");
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    stage("frame done");

    ++g_submits;
    if (g_verbose_frames > 0) --g_verbose_frames;
    if (err_l != vr::VRCompositorError_None || err_r != vr::VRCompositorError_None) {
        ++g_submit_errors;
        if (g_submit_errors <= 5) {
            VRLOG("[comp] Submit error L=%d R=%d (submit #%u)", (int)err_l, (int)err_r, g_submits);
        }
        return false;
    }
    if (g_submits == 1)      VRLOG("[comp] FIRST FRAME SUBMITTED to both eyes");
    if (g_submits % 1000 == 0) VRLOG("[comp] %u frames submitted (%u errors)", g_submits, g_submit_errors);
    return true;
}

bool active() { return g_scene_ok && g_interop_ok; }

void shutdown()
{
    on_reset_begin();
    if (g_interop_dev) { g_interop_dev->Release(); g_interop_dev = nullptr; }
}

} // namespace vrcomp
