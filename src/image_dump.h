// Write the game's in-memory image to disk as a loadable PE.
//
// BFBC2Game.exe's .text is SteamStub-encrypted on the file and decrypted in
// place at startup, so static analysis of the on-disk executable sees noise
// (Ghidra found 2,182 functions in 19.7 MB). Dumping the image from inside
// the process - which this DLL already is - produces the decrypted code that
// Ghidra can actually analyse.
//
// The dump is the raw [base, base+SizeOfImage) range with the section headers
// rewritten so file offsets equal virtual addresses (PointerToRawData = VA,
// SizeOfRawData = VirtualSize rounded up). Uncommitted or no-access pages are
// written as zeros rather than faulted on.
//
// Console verb `dumpimage`. Singleplayer only; reads memory, writes one file.
#pragma once

#include <cstdio>

namespace imgdump {

bool dump(char* path_out, size_t path_size);
bool command(const char* cmd, const char* args, char* reply, size_t n);

} // namespace imgdump
