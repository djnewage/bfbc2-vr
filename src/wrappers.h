// COM wrappers around IDirect3D9 and IDirect3DDevice9.
//
// We inherit from the real interfaces so the compiler refuses to build unless
// every method is present with the exact signature. That check is the whole
// reason for preferring a wrapper over vtable patching: a missed method or a
// wrong signature becomes a build error instead of silent ABI corruption
// inside the game's address space.
//
// Methods we do not care about forward inline. The interesting ones are
// declared here and defined in wrapper_device.cpp.
#pragma once

#include <d3d9.h>

// ---------------------------------------------------------------------------
// A game can present two ways: IDirect3DDevice9::Present, or by fetching a
// swap chain and calling IDirect3DSwapChain9::Present on it. Wrapping only the
// device leaves the second path unhooked - the game escapes through a pointer
// we handed it - and the frame boundary is where all our VR work eventually
// happens. So both paths get wrapped.
class D3D9SwapChainWrapper final : public IDirect3DSwapChain9 {
public:
    D3D9SwapChainWrapper(IDirect3DSwapChain9* real, IDirect3DDevice9* device)
        : m_real(real), m_device(device) {}

    IDirect3DSwapChain9* real() const { return m_real; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** dev) override;

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* s) override { return m_real->GetFrontBufferData(s); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** s) override { return m_real->GetBackBuffer(i, t, s); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* s) override { return m_real->GetRasterStatus(s); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* m) override { return m_real->GetDisplayMode(m); }
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pp) override { return m_real->GetPresentParameters(pp); }

private:
    IDirect3DSwapChain9* m_real   = nullptr;
    IDirect3DDevice9*    m_device = nullptr;   // our device wrapper
};

// ---------------------------------------------------------------------------

class D3D9DeviceWrapper final : public IDirect3DDevice9 {
public:
    D3D9DeviceWrapper(IDirect3DDevice9* real, IDirect3D9* parent)
        : m_real(real), m_parent(parent) {}

    IDirect3DDevice9* real() const { return m_real; }

    // --- IUnknown ---------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // --- intercepted ------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* data, UINT count) override;

    // --- straight forwarding ---------------------------------------------
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return m_real->TestCooperativeLevel(); }
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return m_real->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* c) override { return m_real->GetDeviceCaps(c); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT i, D3DDISPLAYMODE* m) override { return m_real->GetDisplayMode(i, m); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { return m_real->GetCreationParameters(p); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) override { return m_real->SetCursorProperties(x, y, s); }
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD f) override { m_real->SetCursorPosition(x, y, f); }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL b) override { return m_real->ShowCursor(b); }
    // Both return wrapped swap chains - see wrapper_device.cpp.
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT i, IDirect3DSwapChain9** sc) override;
    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override { return m_real->GetNumberOfSwapChains(); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, UINT b, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** s) override { return m_real->GetBackBuffer(i, b, t, s); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT i, D3DRASTER_STATUS* s) override { return m_real->GetRasterStatus(i, s); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL b) override { return m_real->SetDialogBoxMode(b); }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT i, DWORD f, const D3DGAMMARAMP* r) override { m_real->SetGammaRamp(i, f, r); }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT i, D3DGAMMARAMP* r) override { m_real->GetGammaRamp(i, r); }

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) override { return m_real->CreateTexture(w, h, l, u, f, p, t, sh); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* sh) override { return m_real->CreateVolumeTexture(w, h, d, l, u, f, p, t, sh); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* sh) override { return m_real->CreateCubeTexture(e, l, u, f, p, t, sh); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** vb, HANDLE* sh) override { return m_real->CreateVertexBuffer(len, u, fvf, p, vb, sh); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** ib, HANDLE* sh) override { return m_real->CreateIndexBuffer(len, u, f, p, ib, sh); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateRenderTarget(w, h, f, ms, q, lk, s, sh); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dis, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateDepthStencilSurface(w, h, f, ms, q, dis, s, sh); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* a, const RECT* r, IDirect3DSurface9* b, const POINT* p) override { return m_real->UpdateSurface(a, r, b, p); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b) override { return m_real->UpdateTexture(a, b); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* a, IDirect3DSurface9* b) override { return m_real->GetRenderTargetData(a, b); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT i, IDirect3DSurface9* s) override { return m_real->GetFrontBufferData(i, s); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const RECT* rb, D3DTEXTUREFILTERTYPE f) override { return m_real->StretchRect(a, ra, b, rb, f); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) override { return m_real->ColorFill(s, r, c); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateOffscreenPlainSurface(w, h, f, p, s, sh); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9* s) override { return m_real->SetRenderTarget(i, s); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i, IDirect3DSurface9** s) override { return m_real->GetRenderTarget(i, s); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* s) override { return m_real->SetDepthStencilSurface(s); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** s) override { return m_real->GetDepthStencilSurface(s); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD st) override { return m_real->Clear(c, r, f, col, z, st); }

    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) override { return m_real->GetTransform(s, m); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override { return m_real->MultiplyTransform(s, m); }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* v) override { return m_real->SetViewport(v); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* v) override { return m_real->GetViewport(v); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* m) override { return m_real->SetMaterial(m); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* m) override { return m_real->GetMaterial(m); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* l) override { return m_real->SetLight(i, l); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT9* l) override { return m_real->GetLight(i, l); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL e) override { return m_real->LightEnable(i, e); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL* e) override { return m_real->GetLightEnable(i, e); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float* p) override { return m_real->SetClipPlane(i, p); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float* p) override { return m_real->GetClipPlane(i, p); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override { return m_real->SetRenderState(s, v); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) override { return m_real->GetRenderState(s, v); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** b) override { return m_real->CreateStateBlock(t, b); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return m_real->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** b) override { return m_real->EndStateBlock(b); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* s) override { return m_real->SetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* s) override { return m_real->GetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD s, IDirect3DBaseTexture9** t) override { return m_real->GetTexture(s, t); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD s, IDirect3DBaseTexture9* t) override { return m_real->SetTexture(s, t); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override { return m_real->GetTextureStageState(s, t, v); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) override { return m_real->SetTextureStageState(s, t, v); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) override { return m_real->GetSamplerState(s, t, v); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override { return m_real->SetSamplerState(s, t, v); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* n) override { return m_real->ValidateDevice(n); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT i, const PALETTEENTRY* e) override { return m_real->SetPaletteEntries(i, e); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT i, PALETTEENTRY* e) override { return m_real->GetPaletteEntries(i, e); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT i) override { return m_real->SetCurrentTexturePalette(i); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* i) override { return m_real->GetCurrentTexturePalette(i); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* r) override { return m_real->SetScissorRect(r); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* r) override { return m_real->GetScissorRect(r); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL b) override { return m_real->SetSoftwareVertexProcessing(b); }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float n) override { return m_real->SetNPatchMode(n); }
    float   STDMETHODCALLTYPE GetNPatchMode() override { return m_real->GetNPatchMode(); }

    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT s, UINT c) override { return m_real->DrawPrimitive(t, s, c); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) override { return m_real->DrawIndexedPrimitive(t, b, mi, nv, si, pc); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT c, const void* d, UINT st) override { return m_real->DrawPrimitiveUP(t, c, d, st); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* id, D3DFORMAT idf, const void* vd, UINT vs) override { return m_real->DrawIndexedPrimitiveUP(t, mi, nv, pc, id, idf, vd, vs); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT sv, UINT di, UINT vc, IDirect3DVertexBuffer9* vb, IDirect3DVertexDeclaration9* vd, DWORD f) override { return m_real->ProcessVertices(sv, di, vc, vb, vd, f); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d) override { return m_real->CreateVertexDeclaration(e, d); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* d) override { return m_real->SetVertexDeclaration(d); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** d) override { return m_real->GetVertexDeclaration(d); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD f) override { return m_real->SetFVF(f); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* f) override { return m_real->GetFVF(f); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** s) override { return m_real->CreateVertexShader(fn, s); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* s) override { return m_real->SetVertexShader(s); }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** s) override { return m_real->GetVertexShader(s); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT r, float* d, UINT c) override { return m_real->GetVertexShaderConstantF(r, d, c); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT r, const int* d, UINT c) override { return m_real->SetVertexShaderConstantI(r, d, c); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT r, int* d, UINT c) override { return m_real->GetVertexShaderConstantI(r, d, c); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT r, const BOOL* d, UINT c) override { return m_real->SetVertexShaderConstantB(r, d, c); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT r, BOOL* d, UINT c) override { return m_real->GetVertexShaderConstantB(r, d, c); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) override { return m_real->SetStreamSource(n, vb, off, stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT n, IDirect3DVertexBuffer9** vb, UINT* off, UINT* stride) override { return m_real->GetStreamSource(n, vb, off, stride); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT n, UINT d) override { return m_real->SetStreamSourceFreq(n, d); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT n, UINT* d) override { return m_real->GetStreamSourceFreq(n, d); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* ib) override { return m_real->SetIndices(ib); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ib) override { return m_real->GetIndices(ib); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** s) override { return m_real->CreatePixelShader(fn, s); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* s) override { return m_real->SetPixelShader(s); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** s) override { return m_real->GetPixelShader(s); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT r, const float* d, UINT c) override { return m_real->SetPixelShaderConstantF(r, d, c); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT r, float* d, UINT c) override { return m_real->GetPixelShaderConstantF(r, d, c); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT r, const int* d, UINT c) override { return m_real->SetPixelShaderConstantI(r, d, c); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT r, int* d, UINT c) override { return m_real->GetPixelShaderConstantI(r, d, c); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT r, const BOOL* d, UINT c) override { return m_real->SetPixelShaderConstantB(r, d, c); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT r, BOOL* d, UINT c) override { return m_real->GetPixelShaderConstantB(r, d, c); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT h, const float* seg, const D3DRECTPATCH_INFO* i) override { return m_real->DrawRectPatch(h, seg, i); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT h, const float* seg, const D3DTRIPATCH_INFO* i) override { return m_real->DrawTriPatch(h, seg, i); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT h) override { return m_real->DeletePatch(h); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** q) override { return m_real->CreateQuery(t, q); }

private:
    IDirect3DDevice9* m_real   = nullptr;
    IDirect3D9*       m_parent = nullptr;   // our wrapper, handed back by GetDirect3D
};

// ---------------------------------------------------------------------------

class D3D9Wrapper final : public IDirect3D9 {
public:
    explicit D3D9Wrapper(IDirect3D9* real) : m_real(real) {}

    IDirect3D9* real() const { return m_real; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // The one that matters - returns a wrapped device.
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override;

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return m_real->RegisterSoftwareDevice(init); }
    UINT    STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id) override { return m_real->GetAdapterIdentifier(a, f, id); }
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return m_real->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* mode) override { return m_real->EnumAdapterModes(a, f, m, mode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* m) override { return m_real->GetAdapterDisplayMode(a, m); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf, BOOL win) override { return m_real->CheckDeviceType(a, t, df, bf, win); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT cf) override { return m_real->CheckDeviceFormat(a, t, af, u, rt, cf); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT sf, BOOL win, D3DMULTISAMPLE_TYPE ms, DWORD* q) override { return m_real->CheckDeviceMultiSampleType(a, t, sf, win, ms, q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override { return m_real->CheckDepthStencilMatch(a, t, af, rf, df); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT sf, D3DFORMAT tf) override { return m_real->CheckDeviceFormatConversion(a, t, sf, tf); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override { return m_real->GetDeviceCaps(a, t, c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return m_real->GetAdapterMonitor(a); }

private:
    IDirect3D9* m_real = nullptr;
};
