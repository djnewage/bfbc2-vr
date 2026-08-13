// The intercepted device methods. Everything else forwards inline in wrappers.h.
//
// Phase 1/2 scope: observe only. Nothing here alters what the game renders yet.
// These are the four hook points the VR work will eventually use:
//
//   SetVertexShaderConstantF  camera matrices arrive here (Phase 2 -> Phase 5)
//   SetTransform              fixed-function matrices, used by 2D/HUD (Phase 6)
//   BeginScene / EndScene     scene boundary, replayed per eye (Phase 4)
//   Present                   frame submit, retargeted to OpenXR (Phase 3)

#include "wrappers.h"
#include "constants.h"
#include "logger.h"

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    const HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr)) {
        VRLOG("[device] QueryInterface for an unwrapped interface succeeded - hooks may be bypassed");
    }
    return hr;
}

ULONG STDMETHODCALLTYPE D3D9DeviceWrapper::AddRef()
{
    return m_real->AddRef();
}

ULONG STDMETHODCALLTYPE D3D9DeviceWrapper::Release()
{
    const ULONG count = m_real->Release();
    if (count == 0) delete this;
    return count;
}

// Return our IDirect3D9 wrapper, not the real one, so the game cannot walk back
// up to an unwrapped object and create a second unhooked device from it.
HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::GetDirect3D(IDirect3D9** ppD3D9)
{
    if (!ppD3D9) return E_POINTER;
    if (m_parent) {
        m_parent->AddRef();
        *ppD3D9 = m_parent;
        return S_OK;
    }
    return m_real->GetDirect3D(ppD3D9);
}

// ---------------------------------------------------------------------------
// Hook points
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::SetVertexShaderConstantF(UINT start, const float* data, UINT count)
{
    static bool first = true;
    if (first) { first = false; VRLOG("[hook] first SetVertexShaderConstantF (c%u, %u vec4) - constant path is live", start, count); }

    // Mirror into the shadow constant file before forwarding. Observation only -
    // the game still gets exactly the values it asked for.
    vrconst::record(start, data, count);
    return m_real->SetVertexShaderConstantF(start, data, count);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    // Fixed-function transforms. Frostbite is shader-driven, so anything showing
    // up here is likely 2D - HUD, menus - which makes it the Phase 6 lever.
    // Logged sparsely: this fires far less often than the shader constant path.
    static int logged = 0;
    if (m && logged < 32) {
        ++logged;
        VRLOG("[transform] state=%d  [%.3f %.3f %.3f %.3f]",
              static_cast<int>(state), m->_11, m->_12, m->_13, m->_14);
        if (logged == 32) VRLOG("[transform] (further SetTransform logging suppressed)");
    }
    return m_real->SetTransform(state, m);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::BeginScene()
{
    static bool first = true;
    if (first) { first = false; VRLOG("[hook] first BeginScene - device is rendering through our wrapper"); }
    return m_real->BeginScene();
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::EndScene()
{
    return m_real->EndScene();
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    // Announce the first frame explicitly. Waiting for the 600-frame heartbeat
    // to tell us the hook is alive cost us a whole test cycle once already.
    static bool first = true;
    if (first) { first = false; VRLOG("[present] first frame via IDirect3DDevice9::Present - device path is live"); }

    // Frame boundary: poll the discovery hotkeys and emit periodic stats.
    vrconst::on_present();
    return m_real->Present(src, dst, wnd, dirty);
}

// ---------------------------------------------------------------------------
// Swap chains
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::GetSwapChain(UINT i, IDirect3DSwapChain9** sc)
{
    if (!sc) return E_POINTER;

    IDirect3DSwapChain9* real_sc = nullptr;
    const HRESULT hr = m_real->GetSwapChain(i, &real_sc);
    if (FAILED(hr) || !real_sc) return hr;

    VRLOG("[swapchain] GetSwapChain(%u) -> wrapping %p", i, real_sc);
    *sc = new D3D9SwapChainWrapper(real_sc, this);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc)
{
    if (!sc) return E_POINTER;

    IDirect3DSwapChain9* real_sc = nullptr;
    const HRESULT hr = m_real->CreateAdditionalSwapChain(pp, &real_sc);
    if (FAILED(hr) || !real_sc) return hr;

    VRLOG("[swapchain] CreateAdditionalSwapChain -> wrapping %p", real_sc);
    *sc = new D3D9SwapChainWrapper(real_sc, this);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChainWrapper::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain9) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE D3D9SwapChainWrapper::AddRef()
{
    return m_real->AddRef();
}

ULONG STDMETHODCALLTYPE D3D9SwapChainWrapper::Release()
{
    const ULONG count = m_real->Release();
    if (count == 0) delete this;
    return count;
}

// Hand back our device wrapper, not the real device, so the game cannot walk
// from a swap chain to an unhooked device.
HRESULT STDMETHODCALLTYPE D3D9SwapChainWrapper::GetDevice(IDirect3DDevice9** dev)
{
    if (!dev) return E_POINTER;
    if (m_device) {
        m_device->AddRef();
        *dev = m_device;
        return S_OK;
    }
    return m_real->GetDevice(dev);
}

HRESULT STDMETHODCALLTYPE D3D9SwapChainWrapper::Present(const RECT* src, const RECT* dst, HWND wnd,
                                                        const RGNDATA* dirty, DWORD flags)
{
    static bool first = true;
    if (first) { first = false; VRLOG("[present] first frame via IDirect3DSwapChain9::Present - swapchain path is live"); }

    vrconst::on_present();
    return m_real->Present(src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceWrapper::Reset(D3DPRESENT_PARAMETERS* pp)
{
    if (pp) {
        VRLOG("[device] Reset: %ux%u windowed=%d", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed);
    }
    return m_real->Reset(pp);
}
