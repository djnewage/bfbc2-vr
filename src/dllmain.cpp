// Entry point and exported D3D9 surface.
//
// Load chain:  BFBC2Game.exe -> our d3d9.dll -> real implementation
//
// The "real implementation" is preferentially DXVK, renamed to dxvk_d3d9.dll so
// it can live in the same directory as us without a name collision. If that is
// absent we fall back to the system d3d9.dll, so the proxy still works on the
// native D3D9 path.

#include "logger.h"
#include "vrinput.h"
#include "wrappers.h"

#include <windows.h>

namespace {

HMODULE g_real_dll = nullptr;

using PFN_Direct3DCreate9   = IDirect3D9* (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);

PFN_Direct3DCreate9   g_real_create    = nullptr;
PFN_Direct3DCreate9Ex g_real_create_ex = nullptr;

// Loads the backing d3d9 implementation. Never loads ourselves - that would
// recurse - so the DXVK copy must be renamed rather than left as d3d9.dll.
bool load_real_d3d9()
{
    if (g_real_dll) return true;

    const std::string dxvk = vrlog::module_dir() + "dxvk_d3d9.dll";
    g_real_dll = LoadLibraryA(dxvk.c_str());

    if (g_real_dll) {
        VRLOG("[init] backend: DXVK (%s)", dxvk.c_str());
    } else {
        char sys[MAX_PATH] = {};
        GetSystemDirectoryA(sys, MAX_PATH);
        const std::string fallback = std::string(sys) + "\\d3d9.dll";
        g_real_dll = LoadLibraryA(fallback.c_str());

        if (!g_real_dll) {
            VRLOG("[init] FATAL: no d3d9 backend found (tried %s and %s)", dxvk.c_str(), fallback.c_str());
            return false;
        }
        VRLOG("[init] backend: system D3D9 (%s)", fallback.c_str());
        VRLOG("[init] NOTE: DXVK not found. Rename DXVK's d3d9.dll to dxvk_d3d9.dll to chain it.");
    }

    g_real_create    = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(g_real_dll, "Direct3DCreate9"));
    g_real_create_ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(g_real_dll, "Direct3DCreate9Ex"));

    if (!g_real_create) {
        VRLOG("[init] FATAL: backend exports no Direct3DCreate9");
        return false;
    }
    return true;
}

// Generic forwarder for the D3DPERF_* profiling entry points. Games call these
// unconditionally; failing to export them can break startup.
FARPROC forward(const char* name)
{
    if (!g_real_dll && !load_real_d3d9()) return nullptr;
    return GetProcAddress(g_real_dll, name);
}

} // namespace

// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        vrlog::init();
        VRLOG("[init] bfbc2vr proxy attached to process %lu", GetCurrentProcessId());
        break;
    case DLL_PROCESS_DETACH:
        VRLOG("[init] detaching");
        vrinput::release_all();   // never leave a key held down behind us
        vrlog::shutdown();
        break;
    default:
        break;
    }
    return TRUE;
}

extern "C" {

// Self-identification. Tooling needs to answer "is the d3d9.dll sitting in the
// game directory ours, DXVK's, or something else?" before overwriting it.
// A sidecar marker file was the first attempt and it desynced immediately -
// installs predating the marker looked like a stranger's DLL. A magic string
// compiled into the binary cannot drift from the binary.
const char* WINAPI BFBC2VR_ProxyVersion()
{
    return "BFBC2VR_PROXY_MAGIC_v1 bfbc2vr-proxy/0.1";
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk_version)
{
    if (!load_real_d3d9()) return nullptr;

    IDirect3D9* real = g_real_create(sdk_version);
    if (!real) {
        VRLOG("[d3d9] Direct3DCreate9(sdk=%u) FAILED in backend", sdk_version);
        return nullptr;
    }

    VRLOG("[d3d9] Direct3DCreate9(sdk=%u) -> wrapping %p", sdk_version, real);
    return new D3D9Wrapper(real);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** out)
{
    if (!load_real_d3d9()) return E_FAIL;

    if (!g_real_create_ex) {
        VRLOG("[d3d9] Direct3DCreate9Ex requested but backend does not export it");
        return E_NOTIMPL;
    }

    // Not wrapped yet. If BFBC2 ever takes this path we get no hooks at all, so
    // shout about it rather than silently rendering flat.
    VRLOG("[d3d9] *** Direct3DCreate9Ex called - NOT WRAPPED, hooks inactive ***");
    return g_real_create_ex(sdk_version, out);
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    using Fn = int (WINAPI*)(D3DCOLOR, LPCWSTR);
    auto fn = reinterpret_cast<Fn>(forward("D3DPERF_BeginEvent"));
    return fn ? fn(col, name) : 0;
}

int WINAPI D3DPERF_EndEvent()
{
    using Fn = int (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forward("D3DPERF_EndEvent"));
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    using Fn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    if (auto fn = reinterpret_cast<Fn>(forward("D3DPERF_SetMarker"))) fn(col, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    using Fn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    if (auto fn = reinterpret_cast<Fn>(forward("D3DPERF_SetRegion"))) fn(col, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    using Fn = BOOL (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forward("D3DPERF_QueryRepeatFrame"));
    return fn ? fn() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    using Fn = void (WINAPI*)(DWORD);
    if (auto fn = reinterpret_cast<Fn>(forward("D3DPERF_SetOptions"))) fn(options);
}

DWORD WINAPI D3DPERF_GetStatus()
{
    using Fn = DWORD (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forward("D3DPERF_GetStatus"));
    return fn ? fn() : 0;
}

} // extern "C"
