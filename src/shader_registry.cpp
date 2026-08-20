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

    ShaderFacts facts;

    // Pre-resolved clip-space transform spans, so the per-write hot path is an
    // array read instead of string lookups under a lock.
    Span     transform_spans[kMaxSpans];
    size_t   transform_span_count = 0;
};

bool is_transform_name(const std::string& n)
{
    return n == "worldViewProj" || n == "worldViewProjMatrix" || n == "viewProjMatrix";
}

std::mutex g_mutex;
std::map<IDirect3DVertexShader9*, ShaderInfo> g_shaders;

// Aggregated evidence across all shaders: name -> set of "start:count" spans.
// This is what gets logged; per-shader detail stays queryable in memory.
std::map<std::string, std::set<std::pair<unsigned, unsigned>>> g_name_spans;
std::set<std::string> g_logged_names;

unsigned g_parsed = 0, g_no_ctab = 0;
thread_local IDirect3DVertexShader9* t_active = nullptr;
thread_local const ShaderFacts*     t_active_facts = nullptr;   // map nodes are pointer-stable
unsigned g_ordinal = 0;

// Length of the token stream in DWORDs, including the end token. Bounded the
// same way find_ctab is.
size_t token_count(const DWORD* code)
{
    if (!code) return 0;
    constexpr size_t kMaxTokens = 1 << 20;
    size_t i = 1;
    while (i < kMaxTokens) {
        const DWORD tok = code[i];
        if (tok == 0x0000FFFF) return i + 1;
        if ((tok & 0x0000FFFF) == 0x0000FFFE) { i += ((tok >> 16) & 0x7FFF) + 1; continue; }
        ++i;
    }
    return kMaxTokens;
}

unsigned long long fnv1a(const void* data, size_t bytes)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

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

    ShaderFacts facts;
    facts.bytes = static_cast<unsigned>(token_count(bytecode) * sizeof(DWORD));
    facts.hash  = fnv1a(bytecode, facts.bytes);

    std::lock_guard<std::mutex> lock(g_mutex);
    facts.ordinal = ++g_ordinal;

    if (!ctab || bytes_available < sizeof(CtabHeader)) {
        ++g_no_ctab;
        g_shaders[shader].facts = facts;   // known, but no constant table
        return;
    }

    ShaderInfo info;
    info.facts = facts;
    info.facts.has_ctab = true;
    const auto* consts = reinterpret_cast<const ConstantInfo*>(
        reinterpret_cast<const char*>(ctab) + ctab->constant_info);

    // Bounds-check the constant array as a whole before reading entries.
    const size_t needed = ctab->constant_info + ctab->constants * sizeof(ConstantInfo);
    if (needed > bytes_available) { ++g_no_ctab; g_shaders[shader].facts = facts; return; }

    for (DWORD c = 0; c < ctab->constants; ++c) {
        if (consts[c].register_set != 2) continue;   // float4 registers only
        const char* name = ctab_string(ctab, consts[c].name, bytes_available);
        if (!name) continue;

        info.constants[name] = { consts[c].register_index, consts[c].register_count };
        g_name_spans[name].insert({ consts[c].register_index, consts[c].register_count });

        // Skinning palette: the viewmodel (and every soldier) goes through
        // these. Recorded as a fact for the draw classifier, never corrected.
        if (std::strcmp(name, "boneMatrices") == 0 || std::strcmp(name, "boneVectors") == 0) {
            info.facts.has_bones = true;
            if (std::strcmp(name, "boneMatrices") == 0 || info.facts.bone_count == 0) {
                info.facts.bone_start = consts[c].register_index;
                info.facts.bone_count = consts[c].register_count;
            }
        }

        // A clip-space transform must be a full 4x4 - four registers. CTAB
        // shows viewMatrix spans of 2, which would be a packed partial matrix;
        // refuse anything that is not exactly 4 rather than corrupt it.
        if (is_transform_name(name) && consts[c].register_count == 4 &&
            info.transform_span_count < kMaxSpans) {
            info.transform_spans[info.transform_span_count++] =
                { consts[c].register_index, consts[c].register_count };
        }
    }

    info.facts.transform_span_count = static_cast<unsigned>(info.transform_span_count);
    g_shaders[shader] = std::move(info);
    ++g_parsed;
}

void on_set_vertex_shader(IDirect3DVertexShader9* shader)
{
    t_active = shader;
    t_active_facts = nullptr;
    if (!shader) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_shaders.find(shader);
    if (it != g_shaders.end()) t_active_facts = &it->second.facts;
}

const ShaderFacts* active_facts() { return t_active_facts; }

IDirect3DVertexShader9* active_shader() { return t_active; }

bool active_shader_uncharted()
{
    if (!t_active) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_shaders.find(t_active);
    if (it == g_shaders.end()) return false;
    return it->second.transform_span_count == 0 && !it->second.constants.empty();
}

size_t active_transform_spans(Span out[kMaxSpans])
{
    if (!t_active) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_shaders.find(t_active);
    if (it == g_shaders.end()) return 0;

    const ShaderInfo& info = it->second;
    for (size_t i = 0; i < info.transform_span_count; ++i) out[i] = info.transform_spans[i];
    return info.transform_span_count;
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
