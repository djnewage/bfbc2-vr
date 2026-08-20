#include "draw_diag.h"
#include "logger.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace drawdiag {
namespace {

constexpr size_t   kMaxSignatures = 192;
constexpr unsigned kStackDepth    = 12;

struct Key {
    const void* shader;
    unsigned    ret_rva;          // 0 if outside the game image
    unsigned    z_enable, z_write, alpha_blend;
    bool        rt_is_backbuffer;
    unsigned    rt_w, rt_h;
    drawpolicy::ProjClass proj;
    bool        has_bones;
    bool        had_wvp;
    bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
};

struct Agg {
    Key      key;
    unsigned long long shader_hash = 0;
    unsigned shader_bytes = 0;
    unsigned draws = 0, prims = 0, indexed = 0, classified = 0, ownproj = 0;
    unsigned bones_at_write = 0, require_bones = 0;
    float    view_min = 1e9f, view_max = -1.0f;
    float    world_min = 1e9f, world_max = -1.0f;
    float    vz_min = 1e9f, vz_max = -1e9f;
    drawpolicy::ProjParams p_first;
    float    a_min = 1e9f, a_max = -1.0f;
    float    n_min = 1e9f, n_max = -1.0f;
    float    w33_min = 1e9f, w33_max = -1e9f, w23_min = 1e9f, w23_max = -1e9f;
    bool     have_bone0 = false;
    float    bone0[12] = {};
    unsigned stack_rva[4] = {};
    unsigned stack_n = 0;
};

std::mutex g_mutex;
std::vector<Agg> g_aggs;
unsigned g_frames_left = 0;
unsigned g_frames_total = 0;
unsigned g_overflow = 0;          // draws dropped because the table was full
unsigned g_total_draws = 0;
unsigned g_dump_index = 0;
bool     g_active = false;

// Game image range, for turning absolute return addresses into RVAs that
// survive ASLR across runs. Resolved lazily.
const char* g_image_base = nullptr;
size_t      g_image_size = 0;

void resolve_image()
{
    if (g_image_base) return;
    const HMODULE exe = GetModuleHandleW(nullptr);
    g_image_base = reinterpret_cast<const char*>(exe);
    g_image_size = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_image_base + dos->e_lfanew);
        g_image_size = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_image_size = 0;
    }
    VRLOG("[draws] game image %p + 0x%zx", g_image_base, g_image_size);
}

unsigned to_rva(const void* p)
{
    if (!g_image_base || !g_image_size) return 0;
    const char* c = static_cast<const char*>(p);
    if (c < g_image_base || c >= g_image_base + g_image_size) return 0;
    return static_cast<unsigned>(c - g_image_base);
}

void capture_stack(Agg& a)
{
    void* frames[kStackDepth] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, kStackDepth, frames, nullptr);
    a.stack_n = 0;
    for (USHORT i = 0; i < n && a.stack_n < 4; ++i) {
        const unsigned rva = to_rva(frames[i]);
        if (rva) a.stack_rva[a.stack_n++] = rva;
    }
}

void dump()
{
    std::vector<Agg> aggs;
    unsigned frames, overflow, total;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        aggs = g_aggs;
        frames = g_frames_total; overflow = g_overflow; total = g_total_draws;
    }
    std::sort(aggs.begin(), aggs.end(), [](const Agg& x, const Agg& y) { return x.draws > y.draws; });

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sbfbc2vr_draws_%02u.txt", vrlog::module_dir().c_str(), g_dump_index);
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) { VRLOG("[draws] failed to open %s", path); return; }

    fprintf(f, "bfbc2vr draw census: %u frames, %u draws, %zu signatures%s\n",
            frames, total, aggs.size(), overflow ? " (TABLE FULL - some draws dropped)" : "");
    if (overflow) fprintf(f, "dropped %u draws after the table filled; low-count signatures may be missing\n", overflow);
    fprintf(f, "game image base %p size 0x%zx; RVAs below are relative to it\n\n", g_image_base, g_image_size);
    fprintf(f, "Columns: draws/frame  prims/frame  shader(hash bytes)  ret-rva  z/zw/ab  rt  proj-class  a,b(tan-h,tan-v)  near  view-dist[min,max]  view-z[min,max]  world-dist[min,max]  residue w33/w23  bones  stack\n\n");

    const float inv_frames = frames ? 1.0f / static_cast<float>(frames) : 1.0f;
    for (const Agg& a : aggs) {
        const Key& k = a.key;
        fprintf(f, "%7.1f %9.1f  vs=%p h=%016llx/%-5u  ret=%06x  z=%u zw=%u ab=%u  rt=%s(%ux%u)  %-16s",
                a.draws * inv_frames, a.prims * inv_frames, k.shader, a.shader_hash, a.shader_bytes,
                k.ret_rva, k.z_enable, k.z_write, k.alpha_blend,
                k.rt_is_backbuffer ? "BB" : "other", k.rt_w, k.rt_h,
                drawpolicy::proj_class_name(k.proj));
        if (k.had_wvp && a.p_first.perspective) {
            fprintf(f, "  a=%.4f b=%.4f (tan %.4f/%.4f) a[%.4f..%.4f]  near=%.4f[%.4f..%.4f]  view=[%.3f..%.3f] z=[%.3f..%.3f]  world=[%.2f..%.2f]  w33=[%.3f..%.3f] w23=[%.4f..%.4f]",
                    a.p_first.a, a.p_first.b, a.p_first.tan_half_h(), a.p_first.tan_half_v(), a.a_min, a.a_max,
                    a.p_first.near_z(), a.n_min, a.n_max,
                    a.view_min, a.view_max, a.vz_min, a.vz_max, a.world_min, a.world_max,
                    a.w33_min, a.w33_max, a.w23_min, a.w23_max);
        } else {
            fprintf(f, "  (no perspective wvp)");
        }
        fprintf(f, "  bones=%d", k.has_bones ? 1 : 0);
        if (a.classified) fprintf(f, "  VIEWMODEL=%u", a.classified);
        if (a.ownproj)    fprintf(f, "  own-proj=%u", a.ownproj);
        if (a.classified || a.ownproj)
            fprintf(f, " (at write: bones=%u/%u require=%u/%u)", a.bones_at_write, a.draws, a.require_bones, a.draws);
        if (a.have_bone0) {
            // Bone 0 as a 3x4 (three registers): translation column + the 3x3.
            // Near-identity 3x3 with small translation = bind/object space;
            // a rotation resembling the camera = bones carry view.
            const float* b = a.bone0;
            const float tx = b[3], ty = b[7], tz = b[11];
            const float diag = (b[0] + b[5] + b[10]) / 3.0f;
            const float off = std::fabs(b[1]) + std::fabs(b[2]) + std::fabs(b[4]) + std::fabs(b[6]) + std::fabs(b[8]) + std::fabs(b[9]);
            fprintf(f, "  bone0: t=(%.3f,%.3f,%.3f) |t|=%.3f diag=%.3f offdiag=%.3f", tx, ty, tz, std::sqrt(tx*tx+ty*ty+tz*tz), diag, off);
        }
        if (a.stack_n) {
            fprintf(f, "  stack=");
            for (unsigned i = 0; i < a.stack_n; ++i) fprintf(f, "%s%06x", i ? ">" : "", a.stack_rva[i]);
        }
        fprintf(f, "\n");
    }
    fclose(f);

    VRLOG("[draws] census %02u written -> %s  (%u frames, %u draws, %zu signatures%s)",
          g_dump_index, path, frames, total, aggs.size(), overflow ? ", TABLE FULL" : "");
    // Short in-log summary: the bone signatures and anything with its own projection.
    unsigned listed = 0;
    for (const Agg& a : aggs) {
        const bool interesting = a.key.has_bones ||
            a.key.proj == drawpolicy::ProjClass::FovDiffers ||
            a.key.proj == drawpolicy::ProjClass::DepthDiffers ||
            a.key.proj == drawpolicy::ProjClass::Both;
        if (!interesting) continue;
        VRLOG("[draws]  %5.1f/frame vs=%p ret=%06x %s bones=%d view=[%.2f..%.2f] near=%.3f a=%.3f%s",
              a.draws * inv_frames, a.key.shader, a.key.ret_rva, drawpolicy::proj_class_name(a.key.proj),
              a.key.has_bones ? 1 : 0, a.view_min, a.view_max, a.p_first.near_z(), a.p_first.a,
              a.classified ? " VIEWMODEL" : (a.ownproj ? " own-proj" : ""));
        if (++listed == 12) { VRLOG("[draws]  ... see file for the rest"); break; }
    }
    ++g_dump_index;
}

} // namespace

void request_capture(unsigned frames)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) { VRLOG("[draws] capture already running"); return; }
    resolve_image();
    g_aggs.clear();
    g_aggs.reserve(kMaxSignatures);
    g_frames_left = g_frames_total = frames;
    g_overflow = 0; g_total_draws = 0;
    g_active = true;
    VRLOG("[draws] capturing %u frames of draw signatures...", frames);
}

bool capturing() { return g_active; }

void on_draw(const Record& r)
{
    if (!g_active) return;

    Key k = {};
    k.shader = r.shader;
    k.ret_rva = to_rva(r.ret);
    k.z_enable = r.z_enable; k.z_write = r.z_write; k.alpha_blend = r.alpha_blend;
    k.rt_is_backbuffer = r.rt_is_backbuffer; k.rt_w = r.rt_w; k.rt_h = r.rt_h;
    k.proj = r.proj; k.has_bones = r.has_bones; k.had_wvp = r.had_wvp;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) return;
    ++g_total_draws;

    Agg* a = nullptr;
    for (Agg& x : g_aggs) { if (x.key == k) { a = &x; break; } }
    if (!a) {
        if (g_aggs.size() >= kMaxSignatures) { ++g_overflow; return; }
        g_aggs.push_back(Agg{});
        a = &g_aggs.back();
        a->key = k;
        a->shader_hash = r.shader_hash; a->shader_bytes = r.shader_bytes;
        a->p_first = r.p;
        capture_stack(*a);
    }
    ++a->draws;
    a->prims += r.prims;
    if (r.indexed) ++a->indexed;
    if (r.cls == drawpolicy::DrawClass::Viewmodel) ++a->classified;
    if (r.cls == drawpolicy::DrawClass::OwnProjection) ++a->ownproj;
    if (r.bones_at_write) ++a->bones_at_write;
    if (r.require_bones) ++a->require_bones;
    if (r.view_dist >= 0.0f) {
        a->view_min = (std::min)(a->view_min, r.view_dist);
        a->view_max = (std::max)(a->view_max, r.view_dist);
        a->vz_min = (std::min)(a->vz_min, r.view_origin[2]);
        a->vz_max = (std::max)(a->vz_max, r.view_origin[2]);
    }
    if (r.world_dist >= 0.0f) {
        a->world_min = (std::min)(a->world_min, r.world_dist);
        a->world_max = (std::max)(a->world_max, r.world_dist);
    }
    if (r.p.perspective) {
        a->a_min = (std::min)(a->a_min, r.p.a); a->a_max = (std::max)(a->a_max, r.p.a);
        const float n = r.p.near_z();
        a->n_min = (std::min)(a->n_min, n); a->n_max = (std::max)(a->n_max, n);
        a->w33_min = (std::min)(a->w33_min, r.residue_w33); a->w33_max = (std::max)(a->w33_max, r.residue_w33);
        a->w23_min = (std::min)(a->w23_min, r.residue_w23); a->w23_max = (std::max)(a->w23_max, r.residue_w23);
    }
    if (r.have_bone0 && !a->have_bone0) {
        a->have_bone0 = true;
        std::memcpy(a->bone0, r.bone0, sizeof(a->bone0));
    }
}

void on_present()
{
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) return;
        if (g_frames_left > 0) --g_frames_left;
        if (g_frames_left == 0) { g_active = false; finished = true; }
    }
    if (finished) dump();
}

} // namespace drawdiag
