#include "console.h"
#include "camera_override.h"
#include "vr_compositor.h"
#include "memscan.h"
#include "draw_diag.h"
#include "logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace vrcmd {
namespace {

std::string cmd_path()    { return vrlog::module_dir() + "bfbc2vr_cmd.txt"; }
std::string status_path() { return vrlog::module_dir() + "bfbc2vr_status.txt"; }

void write_status()
{
    FILE* f = nullptr;
    if (fopen_s(&f, status_path().c_str(), "w") != 0 || !f) return;
    fprintf(f, "bfbc2vr status (tick %llu ms)\n", static_cast<unsigned long long>(GetTickCount64()));
    camover::status(f);
    vrcomp::status(f);
    memscan::status(f);
    fprintf(f, "census: %s\n", drawdiag::capturing() ? "capturing" : "idle");
    fclose(f);
}

void run_line(const char* line)
{
    char cmd[64] = {};
    const char* args = nullptr;
    {
        size_t i = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && i < sizeof(cmd) - 1) { cmd[i] = line[i]; ++i; }
        while (line[i] == ' ' || line[i] == '\t') ++i;
        args = line + i;
    }
    if (!cmd[0]) return;
    for (char* c = cmd; *c; ++c) *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));

    char reply[512] = {};
    bool handled = false;
    if (!strcmp(cmd, "status")) { handled = true; _snprintf_s(reply, sizeof(reply), _TRUNCATE, "status written"); }
    else if (!strcmp(cmd, "census")) { drawdiag::request_capture(4); handled = true; _snprintf_s(reply, sizeof(reply), _TRUNCATE, "census requested"); }
    if (!handled) handled = camover::command(cmd, args, reply, sizeof(reply));
    if (!handled) handled = vrcomp::command(cmd, args, reply, sizeof(reply));
    if (!handled) handled = memscan::command(cmd, args, reply, sizeof(reply));
    if (!handled) _snprintf_s(reply, sizeof(reply), _TRUNCATE, "unknown command '%s'", cmd);
    VRLOG("[cmd] %s %s -> %s", cmd, args ? args : "", reply);
}

} // namespace

void on_present()
{
    static unsigned frame = 0;
    ++frame;

    // Every frame: hunt state machines, locks, watches.
    memscan::on_present();

    if (frame % 10 == 0) {
        // Read-and-truncate the command file. Opening for read+write in one
        // go would race a writer mid-line; read fully, then truncate.
        FILE* f = nullptr;
        if (fopen_s(&f, cmd_path().c_str(), "r") == 0 && f) {
            char buf[4096] = {};
            const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (got > 0) {
                FILE* t = nullptr;
                if (fopen_s(&t, cmd_path().c_str(), "w") == 0 && t) fclose(t);
                char* ctx = nullptr;
                for (char* line = strtok_s(buf, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx)) {
                    run_line(line);
                }
                write_status();
            }
        }
    }
    if (frame % 120 == 0) write_status();
}

} // namespace vrcmd
