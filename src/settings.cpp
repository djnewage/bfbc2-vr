#include "settings.h"
#include "logger.h"

#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "console.h"

namespace settings {
namespace {

// Verbs worth keeping across launches. Everything not listed here is transient
// or diagnostic and must never be replayed at startup - `recenter` would fire
// against a headset that has not moved yet, `shot` would dump images, and the
// memory-scanner verbs would poke addresses that mean nothing in a new process.
const char* const kPersistable[] = {
    "grip", "gripsmooth", "weaponoffset", "gripoffset", "push",
    "mode", "ownproj", "bones", "pos", "ipd",
    "widen", "auto", "headroll", "yawsign", "pitchsign", "correct",
    "hud", "input", "path", "snap", "deadzone", "aim",
    "fov", "vmfov", "zoom", "hide", "key",
};

struct Entry { std::string key; std::string line; };
std::vector<Entry> g_entries;      // ordered, one per (verb + sub-key)
std::vector<std::string> g_pending;
bool g_loaded = false;
bool g_dirty = false;
unsigned g_applied = 0, g_skipped = 0;

std::string cfg_path() { return vrlog::module_dir() + "bfbc2vr.cfg"; }

// A few sub-commands of an otherwise persistable verb are actions, not
// settings. "aim recal" re-captures the pointing reference from where the
// controller is RIGHT NOW - replaying that at startup would baseline against a
// controller that has not been picked up yet.
const char* const kTransientSub[] = { "recal", "recenter", "reset" };

bool persistable(const char* cmd)
{
    for (const char* v : kPersistable) if (_stricmp(cmd, v) == 0) return true;
    return false;
}

bool transient_sub(const char* args)
{
    if (!args || !*args) return false;
    char first[32] = {};
    sscanf_s(args, "%31s", first, static_cast<unsigned>(sizeof(first)));
    for (const char* v : kTransientSub) if (_stricmp(first, v) == 0) return true;
    return false;
}

// Some verbs are really several settings sharing a name ("hud dist", "hud
// width", "aim kp"). Key on the verb plus its first argument in those cases, so
// setting one does not erase the others.
bool sub_keyed(const char* cmd)
{
    return _stricmp(cmd, "hud") == 0 || _stricmp(cmd, "aim") == 0 ||
           _stricmp(cmd, "grip") == 0 || _stricmp(cmd, "input") == 0 ||
           _stricmp(cmd, "path") == 0 || _stricmp(cmd, "fov") == 0 ||
           _stricmp(cmd, "vmfov") == 0 || _stricmp(cmd, "key") == 0;
}

// For a sub-keyed verb, a first argument that is itself a value (a number, or
// on/off) is the setting - otherwise the first argument names the setting.
bool arg_is_value(const char* a)
{
    if (!a || !*a) return true;
    if (_stricmp(a, "on") == 0 || _stricmp(a, "off") == 0) return true;
    return (*a >= '0' && *a <= '9') || *a == '-' || *a == '.' || *a == '+';
}

std::string make_key(const char* cmd, const char* args)
{
    std::string key = cmd;
    if (sub_keyed(cmd) && args && *args) {
        char first[32] = {};
        sscanf_s(args, "%31s", first, static_cast<unsigned>(sizeof(first)));
        if (first[0] && !arg_is_value(first)) { key += ' '; key += first; }
    }
    return key;
}

} // namespace

void load()
{
    if (g_loaded) return;
    g_loaded = true;

    FILE* f = _fsopen(cfg_path().c_str(), "r", _SH_DENYNO);
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        char* end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!*p || *p == '#') continue;
        g_pending.push_back(p);
    }
    fclose(f);
    VRLOG("[settings] %zu line(s) loaded from %s", g_pending.size(), cfg_path().c_str());
}

bool has_pending() { return !g_pending.empty(); }

void apply_pending()
{
    if (g_pending.empty()) return;
    std::vector<std::string> lines;
    lines.swap(g_pending);
    for (const std::string& l : lines) {
        char cmd[64] = {};
        sscanf_s(l.c_str(), "%63s", cmd, static_cast<unsigned>(sizeof(cmd)));
        if (!persistable(cmd)) {
            // A stale file from an older build, or hand-edited. Skip loudly
            // rather than replaying something transient.
            VRLOG("[settings] skipping non-persistable line: %s", l.c_str());
            ++g_skipped;
            continue;
        }
        vrcmd::run_line_external(l.c_str());
        ++g_applied;
    }
    VRLOG("[settings] applied %u setting(s)", g_applied);
}

void note(const char* cmd, const char* args)
{
    if (!cmd || !persistable(cmd)) return;
    if (transient_sub(args)) return;          // an action, not a setting

    std::string line = cmd;
    if (args && *args) { line += ' '; line += args; }
    // Trim - a trailing space would make two identical settings look different.
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();

    const std::string key = make_key(cmd, args);
    for (Entry& e : g_entries) {
        if (e.key == key) {
            if (e.line == line) return;      // unchanged; do not rewrite the file
            e.line = line;
            g_dirty = true;
            save();
            return;
        }
    }
    g_entries.push_back({ key, line });
    g_dirty = true;
    save();
}

void save()
{
    // _SH_DENYWR, like the log: the file stays readable - and hand-editable -
    // while the game runs. fopen_s would lock it exclusively.
    FILE* f = _fsopen(cfg_path().c_str(), "w", _SH_DENYWR);
    if (!f) return;
    fprintf(f, "# bfbc2vr settings - replayed as commands at startup.\n");
    fprintf(f, "# Same syntax as bfbc2vr_cmd.txt. Delete a line to restore its default.\n");
    for (const Entry& e : g_entries) fprintf(f, "%s\n", e.line.c_str());
    fclose(f);
    g_dirty = false;
}

void status(FILE* f)
{
    fprintf(f, "settings: %zu stored, %u applied at startup, %u skipped%s\n",
            g_entries.size(), g_applied, g_skipped, g_dirty ? " (unsaved)" : "");
}

} // namespace settings
