// Thread-safe file logging. The game is multithreaded and D3D9 calls can arrive
// from more than one thread, so every write takes a lock.
#pragma once

#include <string>

namespace vrlog {

// Opens the log next to our DLL. Safe to call more than once.
void init();
void shutdown();

void write(const char* fmt, ...);

// Directory our DLL lives in (the game directory), with trailing backslash.
const std::string& module_dir();

} // namespace vrlog

#define VRLOG(...) ::vrlog::write(__VA_ARGS__)
