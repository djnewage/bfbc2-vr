#include "shader_registry.h"
#include "logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
    DWORD type_info;        // offset to TypeInfo
    DWORD default_value;
};
// D3DXSHADER_TYPEINFO.
struct TypeInfo {
    WORD  cls;              // D3DXPARAMETER_CLASS: 0 scalar 1 vector 2 matrix_rows 3 matrix_cols 4 object 5 struct
    WORD  type;             // D3DXPARAMETER_TYPE
    WORD  rows;
    WORD  columns;
    WORD  elements;
    WORD  struct_members;
    DWORD struct_member_info;
};
#pragma pack(pop)

const char* set_name(unsigned s)
{
    switch (s) { case 0: return "bool"; case 1: return "int"; case 2: return "float"; case 3: return "sampler"; }
    return "?";
}
const char* class_name(unsigned c)
{
    switch (c) {
        case 0: return "scalar"; case 1: return "vector"; case 2: return "matrix_rows";
        case 3: return "matrix_cols"; case 4: return "object"; case 5: return "struct";
    }
    return "?";
}
// Register-set prefix as HLSL prints it: c/i/b/s.
char set_prefix(unsigned s)
{
    switch (s) { case 0: return 'b'; case 1: return 'i'; case 2: return 'c'; case 3: return 's'; }
    return '?';
}

// One CTAB entry, every register set kept. The old code dropped everything but
// float4, which is why no texture-slot or int/bool map existed.
struct Entry {
    std::string name;
    unsigned set = 0, index = 0, count = 0;
    unsigned cls = 0, type = 0, rows = 0, cols = 0, elements = 0;
};

struct ShaderInfo {
    // name -> (first register, register count), float constants only. Kept
    // exactly as before: the hot path and the classifier read this.
    std::map<std::string, std::pair<unsigned, unsigned>> constants;

    std::vector<Entry> entries;   // everything the CTAB declared, in order
    std::string creator, target;

    ShaderFacts facts;

    // Pre-resolved clip-space transform spans, so the per-write hot path is an
    // array read instead of string lookups under a lock.
    Span     transform_spans[kMaxSpans];
    size_t   transform_span_count = 0;

    // How many times this shader was selected. It is the weight a shader's
    // constant declarations carry: a shader bound 900 times a frame says more
    // about c185 than one bound once at level load.
    unsigned long long sets = 0;
};

bool is_transform_name(const std::string& n)
{
    return n == "worldViewProj" || n == "worldViewProjMatrix" || n == "viewProjMatrix";
}

std::mutex g_mutex;
std::map<IDirect3DVertexShader9*, ShaderInfo> g_shaders;
std::map<IDirect3DPixelShader9*,  ShaderInfo> g_pixel_shaders;

// Aggregated evidence across all shaders: name -> set of "start:count" spans.
// This is what gets logged; per-shader detail stays queryable in memory.
std::map<std::string, std::set<std::pair<unsigned, unsigned>>> g_name_spans;
std::set<std::string> g_logged_names;

unsigned g_parsed = 0, g_no_ctab = 0;
unsigned g_ps_parsed = 0, g_ps_no_ctab = 0;
thread_local IDirect3DVertexShader9* t_active = nullptr;
thread_local const ShaderFacts*     t_active_facts = nullptr;   // map nodes are pointer-stable
thread_local IDirect3DPixelShader9*  t_active_ps = nullptr;
unsigned g_ordinal = 0, g_ps_ordinal = 0;
unsigned g_dump_index = 0;

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

    // First token is the version (0xFFFE0300 for vs_3_0, 0xFFFF0300 for
    // ps_3_0). We do not know the blob length up front; the stream is
    // terminated by 0x0000FFFF. Bound the scan defensively in case of a
    // malformed blob.
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

// Shared by the vertex and pixel paths: both stages carry the same CTAB shape.
// Fills `info` and returns whether a usable table was found. Caller holds the
// lock. The float-only map and transform spans are filled exactly as the
// original code did, so nothing on the hot path changes.
bool parse_ctab(const DWORD* bytecode, ShaderInfo& info, bool aggregate)
{
    size_t bytes_available = 0;
    const CtabHeader* ctab = find_ctab(bytecode, bytes_available);
    if (!ctab || bytes_available < sizeof(CtabHeader)) return false;

    const auto* consts = reinterpret_cast<const ConstantInfo*>(
        reinterpret_cast<const char*>(ctab) + ctab->constant_info);

    // Bounds-check the constant array as a whole before reading entries.
    const size_t needed = ctab->constant_info + ctab->constants * sizeof(ConstantInfo);
    if (needed > bytes_available) return false;

    if (const char* s = ctab_string(ctab, ctab->creator, bytes_available)) info.creator = s;
    if (const char* s = ctab_string(ctab, ctab->target,  bytes_available)) info.target  = s;

    info.entries.reserve(ctab->constants);
    for (DWORD c = 0; c < ctab->constants; ++c) {
        const char* name = ctab_string(ctab, consts[c].name, bytes_available);
        if (!name) continue;

        Entry e;
        e.name  = name;
        e.set   = consts[c].register_set;
        e.index = consts[c].register_index;
        e.count = consts[c].register_count;
        if (consts[c].type_info && consts[c].type_info + sizeof(TypeInfo) <= bytes_available) {
            const auto* ti = reinterpret_cast<const TypeInfo*>(
                reinterpret_cast<const char*>(ctab) + consts[c].type_info);
            e.cls = ti->cls; e.type = ti->type; e.rows = ti->rows; e.cols = ti->columns;
            e.elements = ti->elements;
        }
        info.entries.push_back(e);

        if (consts[c].register_set != 2) continue;   // the maps below are float4 only

        info.constants[name] = { consts[c].register_index, consts[c].register_count };
        if (aggregate) g_name_spans[name].insert({ consts[c].register_index, consts[c].register_count });

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
    return true;
}

ShaderFacts facts_for(const DWORD* bytecode)
{
    ShaderFacts facts;
    facts.bytes = static_cast<unsigned>(token_count(bytecode) * sizeof(DWORD));
    facts.hash  = fnv1a(bytecode, facts.bytes);
    return facts;
}

void write_shader(FILE* f, const char* stage, const ShaderInfo& info)
{
    const ShaderFacts& fa = info.facts;
    fprintf(f, "%s #%u hash=%016llX bytes=%u sets=%llu ctab=%d target=%s creator=\"%s\" bones=%d",
            stage, fa.ordinal, fa.hash, fa.bytes, info.sets, fa.has_ctab ? 1 : 0,
            info.target.empty() ? "-" : info.target.c_str(), info.creator.c_str(), fa.has_bones ? 1 : 0);
    if (fa.has_bones) fprintf(f, " (c%u+%u)", fa.bone_start, fa.bone_count);
    if (info.transform_span_count) {
        fprintf(f, " transform=");
        for (size_t i = 0; i < info.transform_span_count; ++i)
            fprintf(f, "%sc%u+%u", i ? "," : "", info.transform_spans[i].start, info.transform_spans[i].count);
    }
    fprintf(f, "\n");
    for (const Entry& e : info.entries) {
        char reg[24];
        if (e.count > 1) _snprintf_s(reg, sizeof(reg), _TRUNCATE, "%c%u+%u", set_prefix(e.set), e.index, e.count);
        else             _snprintf_s(reg, sizeof(reg), _TRUNCATE, "%c%u",    set_prefix(e.set), e.index);
        fprintf(f, "  %-8s %-10s %-36s", set_name(e.set), reg, e.name.c_str());
        if (e.cls == 2 || e.cls == 3) fprintf(f, " %s %ux%u", class_name(e.cls), e.rows, e.cols);
        else if (e.cls == 1)          fprintf(f, " vector%u", e.cols);
        else                          fprintf(f, " %s", class_name(e.cls));
        if (e.elements > 1) fprintf(f, "[%u]", e.elements);
        fprintf(f, "\n");
    }
}

} // namespace

void on_create_vertex_shader(const DWORD* bytecode, IDirect3DVertexShader9* shader)
{
    if (!bytecode || !shader) return;

    ShaderInfo info;
    info.facts = facts_for(bytecode);

    std::lock_guard<std::mutex> lock(g_mutex);
    info.facts.ordinal = ++g_ordinal;
    if (!parse_ctab(bytecode, info, true)) {
        ++g_no_ctab;
        info.entries.clear(); info.constants.clear();
        g_shaders[shader] = std::move(info);   // known, but no constant table
        return;
    }
    info.facts.has_ctab = true;
    g_shaders[shader] = std::move(info);
    ++g_parsed;
}

void on_create_pixel_shader(const DWORD* bytecode, IDirect3DPixelShader9* shader)
{
    if (!bytecode || !shader) return;

    ShaderInfo info;
    info.facts = facts_for(bytecode);

    std::lock_guard<std::mutex> lock(g_mutex);
    info.facts.ordinal = ++g_ps_ordinal;
    // Pixel constants are NOT folded into g_name_spans: that aggregate feeds
    // the vertex-side transform search and a pixel-side "c0" is a different
    // register file.
    if (!parse_ctab(bytecode, info, false)) {
        ++g_ps_no_ctab;
        info.entries.clear(); info.constants.clear();
        g_pixel_shaders[shader] = std::move(info);
        return;
    }
    info.facts.has_ctab = true;
    g_pixel_shaders[shader] = std::move(info);
    ++g_ps_parsed;
}

void on_set_vertex_shader(IDirect3DVertexShader9* shader)
{
    t_active = shader;
    t_active_facts = nullptr;
    if (!shader) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_shaders.find(shader);
    if (it != g_shaders.end()) { t_active_facts = &it->second.facts; ++it->second.sets; }
}

void on_set_pixel_shader(IDirect3DPixelShader9* shader)
{
    t_active_ps = shader;
    if (!shader) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pixel_shaders.find(shader);
    if (it != g_pixel_shaders.end()) ++it->second.sets;
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

bool dump(char* path_out, size_t path_size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string path = vrlog::dump_path("shaders", g_dump_index);
    if (path_out && path_size) _snprintf_s(path_out, path_size, _TRUNCATE, "%s", path.c_str());
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) return false;
    ++g_dump_index;

    fprintf(f, "# bfbc2vr shader table  launch=%s\n", vrlog::launch_stamp().c_str());
    fprintf(f, "# vertex: %u with CTAB, %u without   pixel: %u with CTAB, %u without\n",
            g_parsed, g_no_ctab, g_ps_parsed, g_ps_no_ctab);
    fprintf(f, "# register prefix: c=float4 i=int4 b=bool s=sampler.  'sets' = times bound since launch.\n");
    fprintf(f, "# Shader pointers are recycled across level loads; the hash is the identity.\n\n");

    // Sorted by ordinal so the file is stable across dumps of the same run.
    std::vector<const ShaderInfo*> vs; vs.reserve(g_shaders.size());
    for (const auto& kv : g_shaders) vs.push_back(&kv.second);
    std::sort(vs.begin(), vs.end(), [](const ShaderInfo* a, const ShaderInfo* b) {
        return a->facts.ordinal < b->facts.ordinal; });
    for (const ShaderInfo* i : vs) write_shader(f, "VS", *i);

    fprintf(f, "\n");
    std::vector<const ShaderInfo*> ps; ps.reserve(g_pixel_shaders.size());
    for (const auto& kv : g_pixel_shaders) ps.push_back(&kv.second);
    std::sort(ps.begin(), ps.end(), [](const ShaderInfo* a, const ShaderInfo* b) {
        return a->facts.ordinal < b->facts.ordinal; });
    for (const ShaderInfo* i : ps) write_shader(f, "PS", *i);

    // The question this file exists to answer: for every register, which
    // NAMES do vertex shaders declare there, and how heavily are those shaders
    // used. c185 and c189 were chosen by observation; this is the declaration.
    fprintf(f, "\n# --- vertex float registers by name (register: name x shaders, weighted by sets) ---\n");
    std::map<unsigned, std::map<std::string, std::pair<unsigned, unsigned long long>>> by_reg;
    for (const auto& kv : g_shaders) {
        for (const Entry& e : kv.second.entries) {
            if (e.set != 2) continue;
            auto& slot = by_reg[e.index][e.name];
            slot.first += 1;
            slot.second += kv.second.sets;
        }
    }
    for (const auto& [reg, names] : by_reg) {
        fprintf(f, "c%-4u", reg);
        bool first = true;
        for (const auto& [name, w] : names) {
            fprintf(f, "%s%s x%u (sets %llu)", first ? " " : "; ", name.c_str(), w.first, w.second);
            first = false;
        }
        fprintf(f, "\n");
    }

    fprintf(f, "\n# --- vertex float names -> every span declared anywhere ---\n");
    for (const auto& [name, spans] : g_name_spans) {
        fprintf(f, "%-36s", name.c_str());
        for (const auto& [start, count] : spans) fprintf(f, " c%u+%u", start, count);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    (void)args;
    if (strcmp(cmd, "shaders") != 0) return false;
    char path[MAX_PATH] = {};
    if (dump(path, sizeof(path))) _snprintf_s(reply, n, _TRUNCATE, "shaders: wrote %s", path);
    else                          _snprintf_s(reply, n, _TRUNCATE, "shaders: could not open dump file");
    return true;
}

void status(FILE* f)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(f, "shaders: vs=%u (+%u no ctab) ps=%u (+%u no ctab) names=%zu\n",
            g_parsed, g_no_ctab, g_ps_parsed, g_ps_no_ctab, g_name_spans.size());
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
        VRLOG("[ctab] %u vertex shaders parsed (%u without CTAB), %u pixel shaders (%u without)",
              g_parsed, g_no_ctab, g_ps_parsed, g_ps_no_ctab);
    }
}

} // namespace shaderreg
