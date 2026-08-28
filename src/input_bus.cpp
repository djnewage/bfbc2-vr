#include "input_bus.h"

#include <windows.h>
#include <cstdio>

namespace inputbus {
namespace {

constexpr std::uint32_t kMagic = 0xBFB0C2A1;
HANDLE g_mapping = nullptr;
Bus*   g_bus = nullptr;

std::uint32_t now_ms() { return static_cast<std::uint32_t>(GetTickCount64() & 0xFFFFFFFFull); }

} // namespace

Bus* get()
{
    if (g_bus) return g_bus;

    char name[64] = {};
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\bfbc2vr_bus_%lu", GetCurrentProcessId());

    // Create-or-open: whichever module attaches first wins, and the other
    // simply opens the same view. No rendezvous, no load-order assumption.
    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name);
    if (!g_mapping) return nullptr;
    const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);

    void* view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
    if (!view) { CloseHandle(g_mapping); g_mapping = nullptr; return nullptr; }

    g_bus = static_cast<Bus*>(view);
    if (!existed) {
        // A fresh mapping is already zeroed by the OS; only the magic needs
        // writing. Do not memset an existing one - the other module may be
        // mid-flight in it.
        g_bus->magic.store(kMagic, std::memory_order_release);
    }
    return g_bus;
}

bool hook_alive(unsigned max_age_ms)
{
    Bus* b = get();
    if (!b) return false;
    const std::uint32_t t = b->hook_tick.load(std::memory_order_acquire);
    return t != 0 && (now_ms() - t) < max_age_ms;
}

bool publisher_alive(unsigned max_age_ms)
{
    Bus* b = get();
    if (!b) return false;
    const std::uint32_t t = b->publisher_tick.load(std::memory_order_acquire);
    return t != 0 && (now_ms() - t) < max_age_ms;
}

void publish_levels(std::uint32_t keys, std::uint32_t buttons)
{
    Bus* b = get();
    if (!b) return;
    // Seqlock write: odd while in progress, so a reader can tell it raced.
    const std::uint32_t v = b->version.load(std::memory_order_relaxed);
    b->version.store(v + 1, std::memory_order_release);
    b->keys.store(keys, std::memory_order_relaxed);
    b->buttons.store(buttons, std::memory_order_relaxed);
    b->publisher_tick.store(now_ms(), std::memory_order_relaxed);
    b->version.store(v + 2, std::memory_order_release);
}

bool read_levels(std::uint32_t& keys, std::uint32_t& buttons)
{
    Bus* b = get();
    if (!b) return false;
    // A publisher that has gone quiet must not leave keys held: report
    // everything released and let the caller's edge diff emit the key-ups.
    if (!publisher_alive()) { keys = 0; buttons = 0; return true; }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t v1 = b->version.load(std::memory_order_acquire);
        if (v1 & 1u) continue;                       // writer mid-update
        keys = b->keys.load(std::memory_order_relaxed);
        buttons = b->buttons.load(std::memory_order_relaxed);
        const std::uint32_t v2 = b->version.load(std::memory_order_acquire);
        if (v1 == v2) return true;                   // consistent snapshot
    }
    return false;
}

void add_impulse(int dx, int dy)
{
    Bus* b = get();
    if (!b) return;
    if (dx) b->pending_dx.fetch_add(dx, std::memory_order_acq_rel);
    if (dy) b->pending_dy.fetch_add(dy, std::memory_order_acq_rel);
}

void take_impulse(int& dx, int& dy)
{
    dx = 0; dy = 0;
    Bus* b = get();
    if (!b) return;
    dx = b->pending_dx.exchange(0, std::memory_order_acq_rel);
    dy = b->pending_dy.exchange(0, std::memory_order_acq_rel);
    if (dx) b->consumed_dx.fetch_add(dx, std::memory_order_relaxed);
    if (dy) b->consumed_dy.fetch_add(dy, std::memory_order_relaxed);
}

void note_injected(unsigned events, std::uint32_t levels, bool was_capped)
{
    Bus* b = get();
    if (!b) return;
    if (events) b->injected_events.fetch_add(events, std::memory_order_relaxed);
    b->drain_calls.fetch_add(1, std::memory_order_relaxed);
    b->levels_seen.store(levels, std::memory_order_relaxed);
    if (was_capped) b->capped.fetch_add(1, std::memory_order_relaxed);
}

void note_hook_alive(std::uint32_t device_flags)
{
    Bus* b = get();
    if (!b) return;
    b->hook_tick.store(now_ms(), std::memory_order_release);
    if (device_flags) b->devices.fetch_or(device_flags, std::memory_order_relaxed);
    b->polls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace inputbus
