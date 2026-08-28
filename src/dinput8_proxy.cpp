#include "dinput8_proxy.h"
#include "input_bus.h"
#include "logger.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstring>

namespace dinput8proxy {
namespace {

HMODULE g_real = nullptr;
using PFN_Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_GetClassObject = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
PFN_Create g_real_create = nullptr;
PFN_GetClassObject g_real_gco = nullptr;

// DIK scancodes for the keys we drive. These are set-1 MAKE codes, not virtual
// keys: MapVirtualKey would drop the E0 prefix on extended keys and hand back
// something DirectInput never reports. Positional, so they assume the default
// QWERTY binds - a rebound profile is out of scope and documented as such.
struct KeyMap { std::uint32_t flag; unsigned dik; };
const KeyMap kKeys[] = {
    { inputbus::kW,     DIK_W },
    { inputbus::kA,     DIK_A },
    { inputbus::kS,     DIK_S },
    { inputbus::kD,     DIK_D },
    { inputbus::kSpace, DIK_SPACE },
    { inputbus::kCtrl,  DIK_LCONTROL },
    { inputbus::kShift, DIK_LSHIFT },
    { inputbus::kR,     DIK_R },
    { inputbus::kE,     DIK_E },
    { inputbus::kF,     DIK_F },
};

bool load_real()
{
    if (g_real) return true;
    // The genuine one lives in the system directory. Under WOW64 that is
    // already SysWOW64, so do NOT hardcode it and do NOT disable filesystem
    // redirection. Never load a dinput8.dll from the game folder - that is us.
    char sys[MAX_PATH] = {};
    GetSystemDirectoryA(sys, MAX_PATH);
    const std::string path = std::string(sys) + "\\dinput8.dll";
    g_real = LoadLibraryA(path.c_str());
    if (!g_real) {
        VRLOG("[dinput] FATAL: cannot load %s", path.c_str());
        return false;
    }
    g_real_create = reinterpret_cast<PFN_Create>(GetProcAddress(g_real, "DirectInput8Create"));
    g_real_gco = reinterpret_cast<PFN_GetClassObject>(GetProcAddress(g_real, "DllGetClassObject"));
    VRLOG("[dinput] real dinput8 loaded (%s)", path.c_str());
    return g_real_create != nullptr;
}

enum class Kind { Other, Mouse, Keyboard };

// ---------------------------------------------------------------------------
// Device wrapper.
//
// IDirectInputDevice8A and ...8W have identical vtable layouts; only the string
// types in methods we forward verbatim differ. Wrapping through the A type and
// forwarding pointers unchanged is therefore safe for both.
class DeviceWrapper final : public IDirectInputDevice8A {
public:
    DeviceWrapper(IDirectInputDevice8A* real, Kind kind) : m_real(real), m_kind(kind) {}

    // --- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (!obj) return E_POINTER;
        // Never hand out the raw device: the game would bypass us entirely.
        if (riid == IID_IUnknown || riid == IID_IDirectInputDevice8A || riid == IID_IDirectInputDevice8W) {
            AddRef(); *obj = this; return S_OK;
        }
        return m_real->QueryInterface(riid, obj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++m_refs; return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = m_real->Release();
        if (--m_refs == 0) delete this;
        return r;
    }

    // --- the methods we interpret
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override
    {
        if (df) {
            m_data_size = df->dwDataSize;   // 256 keyboard, 16/20 mouse
            VRLOG("[dinput] %s SetDataFormat size=%lu", name(), df->dwDataSize);
        }
        return m_real->SetDataFormat(df);
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID guid, LPCDIPROPHEADER hdr) override
    {
        if (&guid == &DIPROP_BUFFERSIZE && hdr) {
            const auto* dw = reinterpret_cast<const DIPROPDWORD*>(hdr);
            m_buffered = dw->dwData > 0;
            VRLOG("[dinput] %s buffer size %lu -> %s", name(), dw->dwData, m_buffered ? "BUFFERED" : "immediate");
        }
        return m_real->SetProperty(guid, hdr);
    }

    HRESULT STDMETHODCALLTYPE Acquire() override { m_acquired = true; return m_real->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        m_acquired = false;
        m_emitted_keys = 0; m_emitted_buttons = 0;   // forget what we injected
        return m_real->Unacquire();
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE h) override
    {
        // If the game waits on this event for buffered data, injected events
        // that arrive without a SetEvent are simply never drained.
        m_event = h;
        return m_real->SetEventNotification(h);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override
    {
        const HRESULT hr = m_real->GetDeviceState(cb, data);
        if (FAILED(hr) || !data) return hr;
        note(m_kind == Kind::Mouse ? inputbus::kMouseImmediate : inputbus::kKeyboardImmediate);

        std::uint32_t keys = 0, buttons = 0;
        if (!inputbus::read_levels(keys, buttons)) return hr;

        if (m_kind == Kind::Keyboard && cb >= 256) {
            auto* k = static_cast<unsigned char*>(data);
            for (const KeyMap& km : kKeys) {
                // OR only. Never write 0 - that would clobber a key the player
                // is physically holding.
                if (keys & km.flag) k[km.dik] |= 0x80;
            }
        } else if (m_kind == Kind::Mouse && cb >= sizeof(DIMOUSESTATE)) {
            auto* m = static_cast<DIMOUSESTATE*>(data);
            int dx = 0, dy = 0;
            inputbus::take_impulse(dx, dy);
            m->lX += dx;                    // additive: the player's own motion survives
            m->lY += dy;
            if (buttons & inputbus::kLeft)  m->rgbButtons[0] |= 0x80;
            if (buttons & inputbus::kRight) m->rgbButtons[1] |= 0x80;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbData, LPDIDEVICEOBJECTDATA rgdod,
                                            LPDWORD pdwInOut, DWORD flags) override
    {
        // The caller's buffer CAPACITY is what it passes in; DirectInput
        // overwrites it with the number actually returned. Capture it first, or
        // appending past the returned count writes off the end of the buffer.
        const DWORD capacity = pdwInOut ? *pdwInOut : 0;
        const HRESULT hr = m_real->GetDeviceData(cbData, rgdod, pdwInOut, flags);
        if (FAILED(hr) || !pdwInOut) return hr;
        note(m_kind == Kind::Mouse ? inputbus::kMouseBuffered : inputbus::kKeyboardBuffered);

        // A null buffer means "how many are pending?" - report ours too, but
        // consume nothing. PEEK likewise must not consume.
        const bool peek = (flags & DIGDD_PEEK) != 0;
        if (!rgdod) { *pdwInOut += pending_event_count(); return hr; }
        if (peek) return hr;

        DWORD used = *pdwInOut;
        const DWORD room = (capacity > used) ? (capacity - used) : 0;
        const DWORD added = append_events(rgdod, used, cbData, room);
        used += added;
        *pdwInOut = used;
        if (m_event && added) SetEvent(m_event);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Poll() override { note(0); return m_real->Poll(); }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD flags) override
    {
        VRLOG("[dinput] %s cooperative level 0x%lX", name(), flags);
        return m_real->SetCooperativeLevel(w, flags);
    }

    // --- everything else forwards verbatim
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override { return m_real->GetCapabilities(c); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID p, DWORD f) override { return m_real->EnumObjects(cb, p, f); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER h) override { return m_real->GetProperty(g, h); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h) override { return m_real->GetObjectInfo(i, o, h); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA i) override { return m_real->GetDeviceInfo(i); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD f) override { return m_real->RunControlPanel(w, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override { return m_real->Initialize(h, v, g); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* o, LPUNKNOWN u) override { return m_real->CreateEffect(g, e, o, u); }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID p, DWORD t) override { return m_real->EnumEffects(cb, p, t); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA i, REFGUID g) override { return m_real->GetEffectInfo(i, g); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override { return m_real->GetForceFeedbackState(s); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD f) override { return m_real->SendForceFeedbackCommand(f); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID p, DWORD f) override { return m_real->EnumCreatedEffectObjects(cb, p, f); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override { return m_real->Escape(e); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD c, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) override { return m_real->SendDeviceData(c, d, n, f); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID p, DWORD fl) override { return m_real->EnumEffectsInFile(f, cb, p, fl); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl) override { return m_real->WriteEffectToFile(f, n, e, fl); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return m_real->BuildActionMap(a, u, f); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return m_real->SetActionMap(a, u, f); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA h) override { return m_real->GetImageInfo(h); }

private:
    const char* name() const { return m_kind == Kind::Mouse ? "mouse" : (m_kind == Kind::Keyboard ? "keyboard" : "device"); }
    void note(std::uint32_t flag)
    {
        inputbus::note_hook_alive(flag | (m_kind == Kind::Mouse ? inputbus::kMouseSeen : inputbus::kKeyboardSeen));
    }

    DWORD pending_event_count() const
    {
        std::uint32_t keys = 0, buttons = 0;
        if (!inputbus::read_levels(keys, buttons)) return 0;
        DWORD n = 0;
        if (m_kind == Kind::Keyboard) {
            for (const KeyMap& km : kKeys) if (((keys & km.flag) != 0) != ((m_emitted_keys & km.flag) != 0)) ++n;
        } else {
            if (((buttons & inputbus::kLeft) != 0) != ((m_emitted_buttons & inputbus::kLeft) != 0)) ++n;
            if (((buttons & inputbus::kRight) != 0) != ((m_emitted_buttons & inputbus::kRight) != 0)) ++n;
            n += 2;   // a potential x and y event
        }
        return n;
    }

    // Append our events after the real ones, with a monotone sequence and a
    // timestamp not earlier than the last real event.
    DWORD append_events(LPDIDEVICEOBJECTDATA out, DWORD used, DWORD stride, DWORD room)
    {
        if (stride < sizeof(DIDEVICEOBJECTDATA)) return 0;
        std::uint32_t keys = 0, buttons = 0;
        if (!inputbus::read_levels(keys, buttons)) { inputbus::note_injected(0, 0xFFFFFFFFu, false); return 0; }

        DWORD n = 0;
        bool capped = false;
        const DWORD tick = GetTickCount();
        auto emit = [&](DWORD ofs, DWORD data) {
            if (n >= room) { capped = true; return; }   // residue is kept: the
            // emitted[] shadow is only updated on a successful emit, so an
            // event that did not fit is re-offered on the next drain rather
            // than being silently lost.
            auto* e = reinterpret_cast<LPDIDEVICEOBJECTDATA>(reinterpret_cast<char*>(out) + (used + n) * stride);
            e->dwOfs = ofs;
            e->dwData = data;
            e->dwTimeStamp = tick;
            e->dwSequence = ++m_sequence;
            e->uAppData = 0;
            ++n;
        };

        if (m_kind == Kind::Keyboard) {
            for (const KeyMap& km : kKeys) {
                const bool want = (keys & km.flag) != 0;
                const bool have = (m_emitted_keys & km.flag) != 0;
                if (want == have) continue;
                const DWORD before = n;
                emit(km.dik, want ? 0x80 : 0x00);
                if (n > before) m_emitted_keys ^= km.flag;
            }
        } else {
            int dx = 0, dy = 0;
            if (room >= 2) {
                inputbus::take_impulse(dx, dy);   // only consume if it can be delivered
                if (dx) emit(DIMOFS_X, static_cast<DWORD>(dx));
                if (dy) emit(DIMOFS_Y, static_cast<DWORD>(dy));
            } else if (room) {
                capped = true;
            }
            const struct { std::uint32_t flag; DWORD ofs; } btns[] = {
                { inputbus::kLeft,  DIMOFS_BUTTON0 },
                { inputbus::kRight, DIMOFS_BUTTON1 },
            };
            for (const auto& b : btns) {
                const bool want = (buttons & b.flag) != 0;
                const bool have = (m_emitted_buttons & b.flag) != 0;
                if (want == have) continue;
                const DWORD before = n;
                emit(b.ofs, want ? 0x80 : 0x00);
                if (n > before) m_emitted_buttons ^= b.flag;
            }
        }
        inputbus::note_injected(n, keys, capped);
        return n;
    }

    IDirectInputDevice8A* m_real;
    Kind  m_kind;
    ULONG m_refs = 1;
    DWORD m_data_size = 0;
    DWORD m_sequence = 0;
    DWORD m_last_cap = 0;
    bool  m_buffered = false;
    bool  m_acquired = false;
    HANDLE m_event = nullptr;
    std::uint32_t m_emitted_keys = 0, m_emitted_buttons = 0;
};

// ---------------------------------------------------------------------------
class InputWrapper final : public IDirectInput8A {
public:
    explicit InputWrapper(IDirectInput8A* real) : m_real(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (!obj) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInput8A || riid == IID_IDirectInput8W) {
            AddRef(); *obj = this; return S_OK;
        }
        return m_real->QueryInterface(riid, obj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++m_refs; return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = m_real->Release();
        if (--m_refs == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override
    {
        const HRESULT hr = m_real->CreateDevice(guid, out, outer);
        if (FAILED(hr) || !out || !*out) return hr;

        Kind kind = Kind::Other;
        if (guid == GUID_SysMouse || guid == GUID_SysMouseEm || guid == GUID_SysMouseEm2) kind = Kind::Mouse;
        else if (guid == GUID_SysKeyboard || guid == GUID_SysKeyboardEm) kind = Kind::Keyboard;

        if (kind == Kind::Other) return hr;   // gamepads etc. pass through untouched
        VRLOG("[dinput] wrapping %s device", kind == Kind::Mouse ? "mouse" : "keyboard");
        *out = new DeviceWrapper(*out, kind);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID p, DWORD f) override { return m_real->EnumDevices(t, cb, p, f); }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return m_real->GetDeviceStatus(g); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD f) override { return m_real->RunControlPanel(w, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return m_real->Initialize(h, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, LPCSTR n, LPGUID o) override { return m_real->FindDevice(g, n, o); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID p, DWORD f) override { return m_real->EnumDevicesBySemantics(u, a, cb, p, f); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID d) override { return m_real->ConfigureDevices(cb, p, f, d); }

private:
    IDirectInput8A* m_real;
    ULONG m_refs = 1;
};

} // namespace

void init()
{
    load_real();
    inputbus::get();
    VRLOG("[dinput] input role active (pid %lu)", GetCurrentProcessId());
}

HRESULT WINAPI create(HINSTANCE inst, DWORD version, REFIID riid, void** out, void* outer)
{
    if (!load_real()) return E_FAIL;
    const HRESULT hr = g_real_create(inst, version, riid, out, static_cast<LPUNKNOWN>(outer));
    if (FAILED(hr) || !out || !*out) return hr;
    VRLOG("[dinput] DirectInput8Create v0x%lX -> wrapping", version);
    *out = new InputWrapper(static_cast<IDirectInput8A*>(*out));
    return hr;
}

HRESULT WINAPI get_class_object(REFCLSID rclsid, REFIID riid, void** out)
{
    // CoCreateInstance(CLSID_DirectInput8) reaches DirectInput without ever
    // calling DirectInput8Create - an unhooked side door if left alone. We do
    // not wrap the class factory itself yet; log loudly if it is ever used, so
    // a silent bypass shows up as a line in the log rather than as "injection
    // mysteriously does nothing".
    if (!load_real() || !g_real_gco) return E_FAIL;
    VRLOG("[dinput] DllGetClassObject used - COM path is NOT wrapped");
    return g_real_gco(rclsid, riid, out);
}

FARPROC forward(const char* name)
{
    if (!load_real()) return nullptr;
    return GetProcAddress(g_real, name);
}

} // namespace dinput8proxy
