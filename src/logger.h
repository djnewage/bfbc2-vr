// Thread-safe file logging. The game is multithreaded and D3D9 calls can arrive
// from more than one thread, so every write takes a lock.
#pragma once

#include <string>

namespace vrlog {

// Opens the log next to our DLL. Safe to call more than once.
// `filename` names the file; null means the default. The two roles of this
// binary (d3d9 and dinput8) are separate modules with separate FILE handles,
// so they must not share one path or their lines interleave and truncate.
void init(const char* filename = nullptr);
void shutdown();

void write(const char* fmt, ...);

// Directory our DLL lives in (the game directory), with trailing backslash.
const std::string& module_dir();

} // namespace vrlog

#define VRLOG(...) ::vrlog::write(__VA_ARGS__)
