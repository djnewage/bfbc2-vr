#include "vr_compositor.h"
#include "vr_tracking.h"
#include "camera_override.h"
#include "logger.h"

#include <windows.h>
#include <openvr.h>
#include <d3d9_interfaces.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

namespace vrcomp {
namespace {

IDirect3DDevice9*     g_device      = nullptr;   // real device, not our wrapper
IDirect3DSurface9* g_backbuffer = nullptr;   // non-owning identity of backbuffer 0
ID3D9VkInteropDevice* g_interop_dev = nullptr;

// One texture per eye for alternate-eye rendering: each frame refreshes the
// eye the game just rendered; the other eye re-submits last frame's image.
IDirect3DTexture9*     g_eye_tex[2]     = {};
ID3D9VkInteropTexture* g_eye_interop[2] = {};

VkInstance       g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_physdev  = VK_NULL_HANDLE;
VkDevice         g_vk_device   = VK_NULL_HANDLE;
VkQueue          g_vk_queue    = VK_NULL_HANDLE;
uint32_t         g_vk_queue_family = 0;

UINT g_width = 0, g_height = 0;

// HUD overlay state. Two textures: the one being drawn this frame and the one
// the compositor is still reading from last frame.
IDirect3DTexture9*     g_hud_tex[2]     = {};
ID3D9VkInteropTexture* g_hud_interop[2] = {};
int      g_hud_cur = 0;
vr::VROverlayHandle_t g_hud_handle = vr::k_ulOverlayHandleInvalid;
bool     g_hud_enabled = false;   // opt-in via 'hud on' until proven in a level
float    g_hud_dist = 1.5f;     // metres in front of the body direction
float    g_hud_width = 1.6f;    // metres; height follows the texture aspect
bool     g_hud_redirected = false;
IDirect3DSurface9* g_hud_saved_rt = nullptr;
unsigned g_hud_draws = 0, g_hud_draws_last = 0;
bool     g_hud_failed = false;
unsigned g_hud_submits = 0;
bool g_scene_ok   = false;   // compositor interface acquired
bool g_interop_ok = false;
bool g_enabled    = true;    // F4 kill switch - keeps the game playable if VR path hangs
bool g_stamp_debug = false;  // identity markers served their purpose (found the frustum bug)
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
    if (g_eye_tex[0] && g_eye_tex[1]) return true;
    if (!g_device || !g_width) return false;

    for (int e = 0; e < 2; ++e) {
        if (g_eye_tex[e]) continue;
        // Render target in default pool: lives GPU-side, backed by a real VkImage.
        if (FAILED(g_device->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET,
                                           D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_eye_tex[e], nullptr))) {
            VRLOG("[comp] eye %d texture creation failed (%ux%u)", e, g_width, g_height);
            return false;
        }
        if (FAILED(g_eye_tex[e]->QueryInterface(__uuidof(ID3D9VkInteropTexture),
                                                reinterpret_cast<void**>(&g_eye_interop[e])))) {
            VRLOG("[comp] eye %d texture has no interop interface", e);
            g_eye_tex[e]->Release(); g_eye_tex[e] = nullptr;
            return false;
        }
    }
    VRLOG("[comp] eye textures ready (2x %ux%u)", g_width, g_height);
    return true;
}

bool init_hud()
{
    if (g_hud_failed || !g_hud_enabled) return false;
    if (g_hud_tex[0] && g_hud_tex[1] && g_hud_handle != vr::k_ulOverlayHandleInvalid) return true;
    if (!g_device || !g_width || !vr::VROverlay()) return false;

    for (int i = 0; i < 2; ++i) {
        if (g_hud_tex[i]) continue;
        if (FAILED(g_device->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET,
                                           D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud_tex[i], nullptr))) {
            VRLOG("[hud] texture %d creation failed", i); g_hud_failed = true; return false;
        }
        if (FAILED(g_hud_tex[i]->QueryInterface(__uuidof(ID3D9VkInteropTexture),
                                                reinterpret_cast<void**>(&g_hud_interop[i])))) {
            VRLOG("[hud] texture %d has no interop", i); g_hud_failed = true; return false;
        }
        IDirect3DSurface9* surf = nullptr;
        if (SUCCEEDED(g_hud_tex[i]->GetSurfaceLevel(0, &surf)) && surf) {
            g_device->ColorFill(surf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
            surf->Release();
        }
    }
    if (g_hud_handle == vr::k_ulOverlayHandleInvalid) {
        const auto err = vr::VROverlay()->CreateOverlay("bfbc2vr.hud", "BFBC2 VR HUD", &g_hud_handle);
        if (err != vr::VROverlayError_None) {
            VRLOG("[hud] CreateOverlay failed: %d", (int)err); g_hud_failed = true; return false;
        }
        vr::VROverlay()->SetOverlayWidthInMeters(g_hud_handle, g_hud_width);
        vr::VROverlay()->SetOverlayAlpha(g_hud_handle, 1.0f);
        vr::VROverlay()->SetOverlayInputMethod(g_hud_handle, vr::VROverlayInputMethod_None);
        vr::VROverlay()->ShowOverlay(g_hud_handle);
        VRLOG("[hud] overlay created (%ux%u, %.2f m wide at %.2f m)", g_width, g_height, g_hud_width, g_hud_dist);
    }
    return true;
}

void release_hud_textures()
{
    hud_end_draw();
    for (int i = 0; i < 2; ++i) {
        if (g_hud_interop[i]) { g_hud_interop[i]->Release(); g_hud_interop[i] = nullptr; }
        if (g_hud_tex[i])     { g_hud_tex[i]->Release();     g_hud_tex[i] = nullptr; }
    }
}

// Overlay pose: in front of the BODY direction (the HMD reference captured at
// recenter), in the standing tracking space. Yaw/pitch are in the convention
// of camover::hmd_yaw_pitch (yaw 0 = -Z, positive toward -X; pitch up positive).
void update_hud_transform()
{
    float yaw = 0.0f, pitch = 0.0f, pos[3] = {};
    if (!camover::body_frame(yaw, pitch, pos)) return;
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // forward in OpenVR space
    const float f[3] = { sy * cp, sp, -cy * cp };
    // overlay local +Z faces the viewer: z = -f; x = up x z; y = z x x
    float z[3] = { -f[0], -f[1], -f[2] };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float x[3] = { up[1]*z[2] - up[2]*z[1], up[2]*z[0] - up[0]*z[2], up[0]*z[1] - up[1]*z[0] };
    const float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (xl < 1e-4f) { x[0] = 1.0f; x[1] = 0.0f; x[2] = 0.0f; } else { x[0] /= xl; x[1] /= xl; x[2] /= xl; }
    const float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
    vr::HmdMatrix34_t m = {};
    for (int r = 0; r < 3; ++r) {
        m.m[r][0] = x[r]; m.m[r][1] = y[r]; m.m[r][2] = z[r];
        m.m[r][3] = pos[r] + f[r] * g_hud_dist;
    }
    vr::VROverlay()->SetOverlayTransformAbsolute(g_hud_handle, vr::TrackingUniverseStanding, &m);
}

// Steam's F12 screenshot crashes the game in a VR scene session (overlay GPU
// capture colliding with our submit path), so the mod takes its own: both eye
// textures dumped as BMPs into the game directory on F10.
void save_eye_bmps()
{
    static int shot = 0;
    for (int e = 0; e < 2; ++e) {
        if (!g_eye_tex[e]) continue;

        IDirect3DSurface9* src = nullptr;
        if (FAILED(g_eye_tex[e]->GetSurfaceLevel(0, &src)) || !src) continue;

        IDirect3DSurface9* sys = nullptr;
        if (FAILED(g_device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8,
                                                         D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) {
            src->Release(); continue;
        }

        if (SUCCEEDED(g_device->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr = {};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                char path[MAX_PATH];
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%sbfbc2vr_eye%c_%02d.bmp",
                            vrlog::module_dir().c_str(), e == 0 ? 'L' : 'R', shot);
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f) {
                    const unsigned img = g_width * g_height * 4;
                    unsigned char hdr[54] = { 'B','M' };
                    *reinterpret_cast<unsigned*>(hdr + 2)  = 54 + img;
                    *reinterpret_cast<unsigned*>(hdr + 10) = 54;
                    *reinterpret_cast<unsigned*>(hdr + 14) = 40;
                    *reinterpret_cast<int*>(hdr + 18)      = static_cast<int>(g_width);
                    *reinterpret_cast<int*>(hdr + 22)      = -static_cast<int>(g_height);  // top-down
                    *reinterpret_cast<short*>(hdr + 26)    = 1;
                    *reinterpret_cast<short*>(hdr + 28)    = 32;
                    *reinterpret_cast<unsigned*>(hdr + 34) = img;
                    fwrite(hdr, 1, 54, f);
                    const auto* row = static_cast<const unsigned char*>(lr.pBits);
                    for (UINT y = 0; y < g_height; ++y, row += lr.Pitch)
                        fwrite(row, 1, g_width * 4, f);
                    fclose(f);
                    VRLOG("[shot] wrote %s", path);
                }
                sys->UnlockRect();
            }
        }
        sys->Release();
        src->Release();
    }
    ++shot;
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
        g_backbuffer = bb;   // identity only; the swapchain owns it
        bb->Release();
    }
    VRLOG("[comp] device registered, backbuffer %ux%u", g_width, g_height);
}

void on_reset_begin()
{
    release_hud_textures();
    for (int e = 0; e < 2; ++e) {
        if (g_eye_interop[e]) { g_eye_interop[e]->Release(); g_eye_interop[e] = nullptr; }
        if (g_eye_tex[e])     { g_eye_tex[e]->Release();     g_eye_tex[e] = nullptr; }
    }
    VRLOG("[comp] reset: eye textures released");
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
    // HOME: the mod's own screenshot (Steam's F12 crashes VR scene sessions).
    {
        static bool down = false;
        const bool now = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (now && !down && g_device) save_eye_bmps();
        down = now;
    }

    hud_end_draw();   // whatever the game left redirected, the frame is over

    if (!g_device || !g_enabled) return false;
    if (!init_compositor() || !init_interop_device()) return false;

    // Empty-Present policy. An isolated Present with no corrected 3D draws is
    // a pump frame - submitting it would churn eye textures with stale
    // content, so skip entirely (no WaitGetPoses; the next real frame
    // re-paces). SUSTAINED emptiness is a menu or loading screen - keep the
    // compositor fed with mono so the panels never freeze.
    static unsigned s_empty_streak = 0;
    if (camover::stereo_active()) {
        if (camover::modified_last_frame() == 0) {
            ++s_empty_streak;
            if (s_empty_streak < 5) return false;   // isolated pump frame(s)
        } else {
            s_empty_streak = 0;
        }
    } else {
        s_empty_streak = 0;
    }

    // The compositor requires WaitGetPoses before the first Submit ever lands.
    // After that it moves to the END of the frame (post-Submit), which is the
    // conventional order: submit what we have, then block for next frame's
    // poses - instead of blocking up front while holding the game's Present.
    if (!g_wgp_called) {
        stage("first WaitGetPoses");
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        vrtrack::set_render_pose(hmd.mDeviceToAbsoluteTracking, hmd.bPoseIsValid);
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

    // 1. Mirror the backbuffer into the eye texture the game just rendered
    //    for. In stereo the eye alternates per frame; in mono it stays 0 and
    //    both panels get the same picture via the submit below.
    // Mono when tracking is off, or when emptiness is sustained (menu/loading
    // screen - the empty-streak gate above let this frame through). Stereo
    // only for real 3D content frames.
    const bool mono = !camover::stereo_active() || camover::modified_last_frame() == 0;
    const int fresh = mono ? 0 : camover::last_rendered_eye();

    stage("StretchRect");
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(g_eye_tex[fresh]->GetSurfaceLevel(0, &dst)) || !dst) { bb->Release(); return false; }
    const HRESULT copy_hr = g_device->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
    bb->Release();
    if (FAILED(copy_hr)) {
        dst->Release();
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[comp] StretchRect backbuffer->eye failed 0x%08lX", copy_hr); }
        return false;
    }

    // Identity stamps, painted into the texture itself so a stereo screenshot
    // carries its own forensics: which eye texture is on which panel, and how
    // many frames apart the two panels are.
    //   top-left 48x48:  GREEN = eye texture 0, RED = eye texture 1
    //   top strip x=64+: 8 blocks, (g_submits & 0xFF) in binary, white=1
    if (g_stamp_debug) {
        RECT eye_rect = { 0, 0, 48, 48 };
        g_device->ColorFill(dst, &eye_rect,
                            fresh == 0 ? D3DCOLOR_XRGB(0, 220, 0) : D3DCOLOR_XRGB(220, 0, 0));
        for (int bit = 0; bit < 8; ++bit) {
            RECT r = { 64 + bit * 24, 0, 64 + bit * 24 + 24, 24 };
            const bool set = (g_submits >> bit) & 1;
            g_device->ColorFill(dst, &r, set ? D3DCOLOR_XRGB(255, 255, 255) : D3DCOLOR_XRGB(0, 0, 0));
        }
    }
    dst->Release();

    // 2. Vulkan handles + layouts for both eye images.
    stage("GetVulkanImageInfo");
    VkImage image[2] = {};
    VkImageLayout layout[2] = {};
    VkImageCreateInfo info[2] = {};
    for (int e = 0; e < 2; ++e) {
        info[e].sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        if (FAILED(g_eye_interop[e]->GetVulkanImageInfo(&image[e], &layout[e], &info[e]))) {
            static bool logged = false;
            if (!logged) { logged = true; VRLOG("[comp] GetVulkanImageInfo failed (eye %d)", e); }
            return false;
        }
    }

    // 3. Compositor wants TRANSFER_SRC_OPTIMAL. Transition both, flush, submit
    //    under the queue lock, release, transition back. The lock rules are
    //    strict: no D3D9 calls while held, or DXVK deadlocks.
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    // HUD overlay image for this frame (the buffer the HUD was drawn into).
    const bool hud_ok = init_hud() && camover::stereo_active();
    VkImage hud_image = VK_NULL_HANDLE; VkImageLayout hud_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo hud_info = {}; hud_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    const int hud_buf = g_hud_cur;
    if (hud_ok && FAILED(g_hud_interop[hud_buf]->GetVulkanImageInfo(&hud_image, &hud_layout, &hud_info))) hud_image = VK_NULL_HANDLE;

    stage("transition to TRANSFER_SRC");
    for (int e = 0; e < 2; ++e) {
        g_interop_dev->TransitionTextureLayout(g_eye_interop[e], &range, layout[e],
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (hud_image) g_interop_dev->TransitionTextureLayout(g_hud_interop[hud_buf], &range, hud_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    stage("FlushRenderingCommands");
    g_interop_dev->FlushRenderingCommands();

    vr::VRVulkanTextureData_t vkdata[2] = {};
    vr::Texture_t tex[2] = {};
    for (int e = 0; e < 2; ++e) {
        // On x86 VkImage is a non-dispatchable handle, already a uint64_t.
        vkdata[e].m_nImage            = static_cast<uint64_t>(image[e]);
        vkdata[e].m_pDevice           = reinterpret_cast<VkDevice_T*>(g_vk_device);
        vkdata[e].m_pPhysicalDevice   = reinterpret_cast<VkPhysicalDevice_T*>(g_vk_physdev);
        vkdata[e].m_pInstance         = reinterpret_cast<VkInstance_T*>(g_vk_instance);
        vkdata[e].m_pQueue            = reinterpret_cast<VkQueue_T*>(g_vk_queue);
        vkdata[e].m_nQueueFamilyIndex = g_vk_queue_family;
        vkdata[e].m_nWidth            = g_width;
        vkdata[e].m_nHeight           = g_height;
        vkdata[e].m_nFormat           = static_cast<uint32_t>(info[e].format);
        vkdata[e].m_nSampleCount      = 1;
        tex[e].handle      = &vkdata[e];
        tex[e].eType       = vr::TextureType_Vulkan;
        tex[e].eColorSpace = vr::ColorSpace_Auto;
    }

    // Mono: both panels get the fresh texture. Stereo: each panel gets its
    // own texture - one refreshed this frame, the other one frame stale.
    vr::Texture_t* tex_l = mono ? &tex[fresh] : &tex[0];
    vr::Texture_t* tex_r = mono ? &tex[fresh] : &tex[1];

    // THE FUSION FIX. Without bounds, the compositor stretches our symmetric
    // ~55-degree image across each eye's ASYMMETRIC ~108-degree frustum -
    // shifting the two panels in opposite directions (screen-center content
    // landed panel-left in one eye, panel-right in the other; the corner
    // stamps fell outside the visible crop entirely). That opposite shift is
    // artificial disparity on everything, unfusable at any IPD.
    //
    // VRTextureBounds_t maps frustum tangents onto texture UVs:
    //   u(x) = (x + tg) / (2*tg)  for game half-angle tangent tg
    // evaluated at the eye frustum's raw tangents (GetProjectionRaw). Values
    // outside [0,1] are expected - the headset sees wider than the game
    // renders - and produce black borders there instead of stretching.
    vr::VRTextureBounds_t bounds[2] = { { 0, 0, 1, 1 }, { 0, 0, 1, 1 } };
    bool have_bounds = false;
    float tgh = 0.0f, tgv = 0.0f;
    if (vrtrack::system() && camover::game_proj_tangents(tgh, tgv)) {
        for (int e = 0; e < 2; ++e) {
            float l, r, t, b;
            vrtrack::system()->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
            bounds[e].uMin = (l + tgh) / (2.0f * tgh);
            bounds[e].uMax = (r + tgh) / (2.0f * tgh);
            bounds[e].vMin = (t + tgv) / (2.0f * tgv);
            bounds[e].vMax = (b + tgv) / (2.0f * tgv);
        }
        have_bounds = true;
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("[comp] game tangents h=%.4f v=%.4f", tgh, tgv);
            for (int e = 0; e < 2; ++e) {
                VRLOG("[comp] eye %d bounds u=[%.3f, %.3f] v=[%.3f, %.3f]",
                      e, bounds[e].uMin, bounds[e].uMax, bounds[e].vMin, bounds[e].vMax);
            }
        }
    }

    stage("LockSubmissionQueue");
    g_interop_dev->LockSubmissionQueue();
    stage("Submit L");
    const auto err_l = vr::VRCompositor()->Submit(vr::Eye_Left,  tex_l, have_bounds ? &bounds[0] : nullptr);
    stage("Submit R");
    const auto err_r = vr::VRCompositor()->Submit(vr::Eye_Right, tex_r, have_bounds ? &bounds[1] : nullptr);
    if (hud_image) {
        vr::VRVulkanTextureData_t hv = {};
        hv.m_nImage = static_cast<uint64_t>(hud_image);
        hv.m_pDevice = reinterpret_cast<VkDevice_T*>(g_vk_device);
        hv.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(g_vk_physdev);
        hv.m_pInstance = reinterpret_cast<VkInstance_T*>(g_vk_instance);
        hv.m_pQueue = reinterpret_cast<VkQueue_T*>(g_vk_queue);
        hv.m_nQueueFamilyIndex = g_vk_queue_family;
        hv.m_nWidth = g_width; hv.m_nHeight = g_height;
        hv.m_nFormat = static_cast<uint32_t>(hud_info.format);
        hv.m_nSampleCount = 1;
        vr::Texture_t ht = {}; ht.handle = &hv; ht.eType = vr::TextureType_Vulkan; ht.eColorSpace = vr::ColorSpace_Auto;
        stage("SetOverlayTexture");
        const auto herr = vr::VROverlay()->SetOverlayTexture(g_hud_handle, &ht);
        if (herr != vr::VROverlayError_None) {
            static unsigned logged = 0;
            if (logged++ < 3) VRLOG("[hud] SetOverlayTexture failed: %d", (int)herr);
        } else {
            ++g_hud_submits;
        }
    }
    stage("ReleaseSubmissionQueue");
    g_interop_dev->ReleaseSubmissionQueue();

    stage("transition back");
    if (hud_image) {
        g_interop_dev->TransitionTextureLayout(g_hud_interop[hud_buf], &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hud_layout);
        // Flip: next frame draws into the other buffer, cleared to transparent.
        g_hud_cur ^= 1;
        IDirect3DSurface9* surf = nullptr;
        if (SUCCEEDED(g_hud_tex[g_hud_cur]->GetSurfaceLevel(0, &surf)) && surf) {
            g_device->ColorFill(surf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
            surf->Release();
        }
        update_hud_transform();
        vr::VROverlay()->SetOverlayWidthInMeters(g_hud_handle, g_hud_width);
        if (g_hud_enabled) vr::VROverlay()->ShowOverlay(g_hud_handle); else vr::VROverlay()->HideOverlay(g_hud_handle);
    }
    g_hud_draws_last = g_hud_draws; g_hud_draws = 0;
    for (int e = 0; e < 2; ++e) {
        g_interop_dev->TransitionTextureLayout(g_eye_interop[e], &range,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout[e]);
    }

    // End-of-frame heartbeat: pace to HMD refresh and fetch next frame's
    // predicted pose. Handing it to vrtrack is what keeps our render pose and
    // the compositor's display assumption identical - the anti-double-vision
    // contract.
    stage("WaitGetPoses");
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    const auto& hmd_pose = poses[vr::k_unTrackedDeviceIndex_Hmd];
    vrtrack::set_render_pose(hmd_pose.mDeviceToAbsoluteTracking, hmd_pose.bPoseIsValid);
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

IDirect3DSurface9* backbuffer_surface() { return g_backbuffer; }

void hud_begin_draw()
{
    if (!g_hud_enabled || g_hud_failed || !g_device) return;
    if (!init_hud()) return;
    ++g_hud_draws;
    if (g_hud_redirected) return;
    IDirect3DSurface9* cur = nullptr;
    if (FAILED(g_device->GetRenderTarget(0, &cur)) || !cur) return;
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(g_hud_tex[g_hud_cur]->GetSurfaceLevel(0, &dst)) || !dst) { cur->Release(); return; }
    const HRESULT hr = g_device->SetRenderTarget(0, dst);
    dst->Release();
    if (FAILED(hr)) {
        cur->Release();
        static bool logged = false;
        if (!logged) { logged = true; VRLOG("[hud] SetRenderTarget(HUD) failed 0x%08lX - HUD overlay disabled", hr); }
        g_hud_failed = true;
        return;
    }
    g_hud_saved_rt = cur;   // keep the reference until restore
    g_hud_redirected = true;
    // Alpha must ACCUMULATE coverage for the overlay: separate alpha blend
    // ONE / INVSRCALPHA while colour keeps the game's own blend.
    g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    g_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    g_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
}

void hud_end_draw()
{
    if (!g_hud_redirected) return;
    g_hud_redirected = false;
    if (g_device) {
        g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        if (g_hud_saved_rt) g_device->SetRenderTarget(0, g_hud_saved_rt);
    }
    if (g_hud_saved_rt) { g_hud_saved_rt->Release(); g_hud_saved_rt = nullptr; }
}

bool hud_redirected() { return g_hud_redirected; }

void hud_on_game_set_render_target()
{
    // The game is setting its own target now; our redirect is implicitly over
    // (the game's call will land after this). Restore the blend state only.
    if (!g_hud_redirected) return;
    g_hud_redirected = false;
    if (g_device) g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    if (g_hud_saved_rt) { g_hud_saved_rt->Release(); g_hud_saved_rt = nullptr; }
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    if (!strcmp(cmd, "shot")) {
        if (!g_device) { _snprintf_s(reply, n, _TRUNCATE, "shot: no device yet"); return true; }
        save_eye_bmps();
        _snprintf_s(reply, n, _TRUNCATE, "shot: eye BMPs written (bfbc2vr_eyeL/R_NN.bmp)");
        return true;
    }
    if (!strcmp(cmd, "hud")) {
        char a1[32] = {}, a2[32] = {};
        if (args) sscanf_s(args, "%31s %31s", a1, static_cast<unsigned>(sizeof(a1)), a2, static_cast<unsigned>(sizeof(a2)));
        if (a1[0]) {
            if (!_stricmp(a1, "on")) { g_hud_enabled = true; g_hud_failed = false; }
            else if (!_stricmp(a1, "off")) { g_hud_enabled = false; hud_end_draw(); if (g_hud_handle != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) vr::VROverlay()->HideOverlay(g_hud_handle); }
            else if (!_stricmp(a1, "dist") && a2[0]) g_hud_dist = (std::min)((std::max)(static_cast<float>(atof(a2)), 0.3f), 10.0f);
            else if (!_stricmp(a1, "width") && a2[0]) g_hud_width = (std::min)((std::max)(static_cast<float>(atof(a2)), 0.2f), 10.0f);
        }
        _snprintf_s(reply, n, _TRUNCATE, "hud %s dist=%.2f width=%.2f draws/frame=%u submits=%u%s",
                    g_hud_enabled ? "on" : "off", g_hud_dist, g_hud_width, g_hud_draws_last, g_hud_submits, g_hud_failed ? " FAILED" : "");
        return true;
    }
    if (!strcmp(cmd, "vr")) {
        char a1[16] = {};
        _snprintf_s(reply, n, _TRUNCATE, "vr submit %s", g_enabled ? "on" : "off");
        (void)a1;
        return true;
    }
    return false;
}

void status(FILE* f)
{
    fprintf(f, "compositor: scene=%d interop=%d enabled=%d backbuffer=%ux%u submits=%u\n",
            g_scene_ok ? 1 : 0, g_interop_ok ? 1 : 0, g_enabled ? 1 : 0, g_width, g_height, g_submits);
    fprintf(f, "hud: %s dist=%.2f width=%.2f draws/frame=%u submits=%u%s\n",
            g_hud_enabled ? "on" : "off", g_hud_dist, g_hud_width, g_hud_draws_last, g_hud_submits, g_hud_failed ? " FAILED" : "");
}
void backbuffer_size(unsigned& w, unsigned& h) { w = g_width; h = g_height; }

void shutdown()
{
    on_reset_begin();
    if (g_interop_dev) { g_interop_dev->Release(); g_interop_dev = nullptr; }
}

} // namespace vrcomp
