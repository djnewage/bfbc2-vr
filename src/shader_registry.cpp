#include "shader_registry.h"
#include "logger.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace shaderreg {
namespace {

// D3DXSHADER_CONSTANTTABLE layout, as documented for the CTAB comment block.
// Offsets are relative to the start of the CTAB data (after the fourcc).
#pragma pack(push, 1)
struct CtabHeader {
    DWORD size;             // sizeof(CtabHeader)
    DWORD creator;          // offset to creator string
    DWORD version;
    DWORD constants;        // number of constant infos
    DWORD constant_info;    // offset to the first ConstantInfo
    DWORD flags;
    DWORD target;           // offset to target string ("vs_3_0")
};
struct ConstantInfo {
    DWORD name;             // offset to name string
    WORD  register_set;     // 0=bool 1=int4 2=float4 3=sampler
    WORD  register_index;
    WORD  register_count;
    WORD  reserved;
    DWORD type_info;
    DWORD default_value;
};
#pragma pack(pop)

struct ShaderInfo {
    // name -> (first register, register count), float constants only
    std::map<std::string, std::pair<unsigned, unsigned>> constants;
};

std::mutex g_mutex;
std::map<IDirect3DVertexShader9*, ShaderInfo> g_shaders;

// Aggregated evidence across all shaders: name -> set of "start:count" spans.
// This is what gets logged; per-shader detail stays queryable in memory.
std::map<std::string, std::set<std::pair<unsigned, unsigned>>> g_name_spans;
std::set<std::string> g_logged_names;

unsigned g_parsed = 0, g_no_ctab = 0;
thread_local IDirect3DVertexShader9* t_active = nullptr;

// Walk the token stream looking for the CTAB comment. Comment tokens are
// 0x0000FFFE in the low word, length in DWORDs in bits 16..30.
const CtabHeader* find_ctab(const DWORD* code, size_t& bytes_available)
{
    if (!code) return nullptr;

    // First token is the version (0xFFFE0300 for vs_3_0). We do not know the
    // blob length up front; the stream is terminated by 0x0000FFFF. Bound the
    // scan defensively in case of a malformed blob.
    constexpr size_t kMaxTokens = 1 << 20;
    size_t i = 1;
    while (i < kMaxTokens) {
        const DWORD tok = code[i];
        if (tok == 0x0000FFFF) break;                       // end token
        if ((tok & 0x0000FFFF) == 0x0000FFFE) {             // comment
            const size_t len = (tok >> 16) & 0x7FFF;        // in DWORDs
            if (len >= 2 && code[i + 1] == MAKEFOURCC('C','T','A','B')) {
                bytes_available = (len - 1) * sizeof(DWORD);
                return reinterpret_cast<const CtabHeader*>(&code[i + 2]);
            }
            i += len + 1;
            continue;
        }
        ++i;
    }
    return nullptr;
}

const char* ctab_string(const CtabHeader* ctab, DWORD offset, size_t bytes_available)
{
    if (offset == 0 || offset >= bytes_available) return nullptr;
    const char* base = reinterpret_cast<const char*>(ctab);
    // Ensure NUL before the end of the block.
    const char* s = base + offset;
    const size_t max = bytes_available - offset;
    if (!memchr(s, 0, max)) return nullptr;
    return s;
}

} // namespace

void on_create_vertex_shader(const DWORD* bytecode, IDirect3DVertexShader9* shader)
{
    if (!bytecode || !shader) return;

    size_t bytes_available = 0;
    const CtabHeader* ctab = find_ctab(bytecode, bytes_available);

    std::lock_guard<std::mutex> lock(g_mutex);

    if (!ctab || bytes_available < sizeof(CtabHeader)) {
        ++g_no_ctab;
        g_shaders[shader];   // known, but empty
        return;
    }

    ShaderInfo info;
    const auto* consts = reinterpret_cast<const ConstantInfo*>(
        reinterpret_cast<const char*>(ctab) + ctab->constant_info);

    // Bounds-check the constant array as a whole before reading entries.
    const size_t needed = ctab->constant_info + ctab->constants * sizeof(ConstantInfo);
    if (needed > bytes_available) { ++g_no_ctab; g_shaders[shader]; return; }

    for (DWORD c = 0; c < ctab->constants; ++c) {
        if (consts[c].register_set != 2) continue;   // float4 registers only
        const char* name = ctab_string(ctab, consts[c].name, bytes_available);
        if (!name) continue;

        info.constants[name] = { consts[c].register_index, consts[c].register_count };
        g_name_spans[name].insert({ consts[c].register_index, consts[c].register_count });
    }

    g_shaders[shader] = std::move(info);
    ++g_parsed;
}

void on_set_vertex_shader(IDirect3DVertexShader9* shader)
{
    t_active = shader;
}

void on_present()
{
    // Flush newly seen names every couple of seconds rather than per create -
    // level loads create shaders in bursts.
    static unsigned frame = 0;
    if (++frame % 120 != 0) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [name, spans] : g_name_spans) {
        if (g_logged_names.count(name)) continue;
        g_logged_names.insert(name);

        char span_buf[160] = {};
        size_t off = 0;
        unsigned listed = 0;
        for (const auto& [start, count] : spans) {
            if (listed++ == 6) { off += _snprintf_s(span_buf + off, sizeof(span_buf) - off, _TRUNCATE, " ..."); break; }
            off += _snprintf_s(span_buf + off, sizeof(span_buf) - off, _TRUNCATE, " c%u+%u", start, count);
        }
        VRLOG("[ctab] \"%s\" %s", name.c_str(), span_buf);
    }

    static bool stats_logged = false;
    if (!stats_logged && (g_parsed + g_no_ctab) > 0 && frame % 1200 == 0) {
        stats_logged = true;
        VRLOG("[ctab] %u shaders parsed, %u without CTAB", g_parsed, g_no_ctab);
    }
}

} // namespace shaderreg
