// The channel between the two halves of the mod.
//
// We ship ONE binary installed under TWO filenames: `d3d9.dll` (renderer: VR,
// tracking, the correction) and `dinput8.dll` (input: DirectInput wrappers).
// Windows loads those as two independent modules with two copies of every
// global, so they cannot simply share a variable. This is the bus between them.
//
// It is a page-sized named shared mapping, POD, all atomics, created-or-opened
// by whichever module attaches first - symmetric, so neither has to wait for
// the other and either works alone.
//
// Two kinds of traffic, with deliberately different semantics:
//
//   LEVELS   (keys and buttons held) are idempotent state. Published through a
//            seqlock: the reader retries while the writer is mid-update. If the
//            publisher dies, the level simply goes stale and the heartbeat
//            check below releases everything.
//   IMPULSES (mouse counts) are exactly-once. The renderer fetch_adds, the
//            input thread exchanges to zero. Total injected equals total
//            requested no matter how the two thread rates relate - which is
//            what preserves "one correction per fresh controller sample" across
//            a Present loop and an input poll that do not share a cadence.
#pragma once

#include <atomic>
#include <cstdint>

namespace inputbus {

// Held keys, as bit flags. Mapped to DIK scancodes on the input side.
enum Key : std::uint32_t {
    kW = 1u << 0, kA = 1u << 1, kS = 1u << 2, kD = 1u << 3,
    kSpace = 1u << 4, kCtrl = 1u << 5, kShift = 1u << 6,
    kR = 1u << 7, kE = 1u << 8, kF = 1u << 9,
};

enum MouseButton : std::uint32_t { kLeft = 1u << 0, kRight = 1u << 1 };

// Which device classes the input side has actually seen the game read, and how.
enum DeviceFlag : std::uint32_t {
    kMouseSeen        = 1u << 0,
    kKeyboardSeen     = 1u << 1,
    kMouseImmediate   = 1u << 2,
    kMouseBuffered    = 1u << 3,
    kKeyboardImmediate= 1u << 4,
    kKeyboardBuffered = 1u << 5,
};

struct Bus {
    std::atomic<std::uint32_t> magic;
    std::atomic<std::uint32_t> version;      // seqlock: odd = write in progress

    // Levels (renderer -> input)
    std::atomic<std::uint32_t> keys;
    std::atomic<std::uint32_t> buttons;
    std::atomic<std::uint32_t> publisher_tick;   // renderer heartbeat, ms

    // Impulses (renderer -> input), consumed exactly once
    std::atomic<std::int32_t>  pending_dx;
    std::atomic<std::int32_t>  pending_dy;

    // Telemetry (input -> renderer)
    std::atomic<std::uint32_t> hook_tick;        // input-side heartbeat, ms
    std::atomic<std::uint32_t> devices;          // DeviceFlag bits
    std::atomic<std::uint32_t> polls;            // device reads served
    std::atomic<std::int32_t>  consumed_dx;      // cumulative, for exactness checks
    std::atomic<std::int32_t>  consumed_dy;
    std::atomic<std::uint32_t> injected_events;  // buffered events appended
};

// Create-or-open. Safe to call repeatedly; returns null only if the mapping
// cannot be made, in which case callers fall back to the SendInput path.
Bus* get();

// True when the input side has reported in recently - i.e. the DirectInput
// wrappers are live and the game is reading through them.
bool hook_alive(unsigned max_age_ms = 500);

// True when the renderer is still publishing. The input side releases every
// level if this goes false, so a dead publisher cannot leave a key held.
bool publisher_alive(unsigned max_age_ms = 1000);

// Renderer side.
void publish_levels(std::uint32_t keys, std::uint32_t buttons);
void add_impulse(int dx, int dy);

// Input side.
void note_hook_alive(std::uint32_t device_flags);
void take_impulse(int& dx, int& dy);
bool read_levels(std::uint32_t& keys, std::uint32_t& buttons);

} // namespace inputbus
