#include "wrappers.h"
#include "vr_compositor.h"
#include "logger.h"

HRESULT STDMETHODCALLTYPE D3D9Wrapper::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    // Hand back ourselves for interfaces we implement, so the game never
    // escapes the wrapper and reaches the real object directly.
    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    const HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr)) {
        // Most likely IID_IDirect3D9Ex. We do not wrap it, which means hooks
        // would be bypassed - worth a loud log line rather than silence.
        VRLOG("[d3d9] QueryInterface for an unwrapped interface succeeded - hooks may be bypassed");
    }
    return hr;
}

ULONG STDMETHODCALLTYPE D3D9Wrapper::AddRef()
{
    return m_real->AddRef();
}

ULONG STDMETHODCALLTYPE D3D9Wrapper::Release()
{
    const ULONG count = m_real->Release();
    if (count == 0) delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE D3D9Wrapper::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus,
                                                    DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                                    IDirect3DDevice9** out)
{
    if (!out) return E_POINTER;

    if (pp) {
        VRLOG("[device] CreateDevice: %ux%u windowed=%d fmt=%d backbuffers=%u presentInterval=0x%X",
              pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed,
              static_cast<int>(pp->BackBufferFormat), pp->BackBufferCount, pp->PresentationInterval);
        VRLOG("[device] flags=0x%08lX devType=%d adapter=%u", flags, static_cast<int>(type), adapter);
    }

    IDirect3DDevice9* real_device = nullptr;
    const HRESULT hr = m_real->CreateDevice(adapter, type, focus, flags, pp, &real_device);

    if (FAILED(hr) || !real_device) {
        VRLOG("[device] CreateDevice FAILED hr=0x%08lX", hr);
        return hr;
    }

    VRLOG("[device] wrapping device %p", real_device);
    vrcomp::on_device_created(real_device);
    *out = new D3D9DeviceWrapper(real_device, this);
    return hr;
}
