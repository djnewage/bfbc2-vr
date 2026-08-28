#include "logger.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace vrlog {
namespace {

std::mutex g_mutex;
FILE*      g_file = nullptr;
std::string g_dir;
bool       g_initialized = false;

// Resolves the directory containing this DLL. We deliberately do NOT use the
// process working directory - Steam does not guarantee what that is.
std::string resolve_module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&resolve_module_dir),
        &self);

    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);

    std::string s(path);
    const size_t slash = s.find_last_of('\\');
    return (slash == std::string::npos) ? std::string() : s.substr(0, slash + 1);
}

} // namespace

void init(const char* filename)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    g_initialized = true;

    g_dir = resolve_module_dir();
    const std::string path = g_dir + (filename ? filename : "bfbc2vr.log");

    // _fsopen with _SH_DENYWR, NOT fopen_s. fopen_s opens with exclusive access,
    // which locks the log so hard that nothing can read it while the game runs -
    // exactly when we most want to watch it. This lets readers in and keeps
    // other writers out.
    g_file = _fsopen(path.c_str(), "w", _SH_DENYWR);

    if (g_file) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_file, "=== bfbc2vr proxy log - %04d-%02d-%02d %02d:%02d:%02d ===\n",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fflush(g_file);
    }
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void write(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);   // we will crash the host process eventually; never buffer
}

const std::string& module_dir() { return g_dir; }

} // namespace vrlog
