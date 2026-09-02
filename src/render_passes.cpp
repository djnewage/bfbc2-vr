#include "render_passes.h"
#include "logger.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace rpass {
namespace {

constexpr unsigned kMaxFrames  = 8;     // capture window cap
constexpr unsigned kMaxPasses  = 96;    // per frame; overflow is counted, not lost silently
constexpr unsigned kMaxShaders = 6;     // distinct vertex shaders remembered per pass
constexpr unsigned kMaxCams    = 4;     // distinct camera projections remembered per pass

struct Surf {
    const void* ptr = nullptr;
    unsigned w = 0, h = 0;
    unsigned fmt = 0;            // D3DFORMAT
    unsigned ms = 0;             // D3DMULTISAMPLE_TYPE
    bool     is_bb = false;
    bool     set = false;
};

struct Cam {
    drawpolicy::ProjParams proj;
    unsigned writes = 0;
    float fwd[3] = { 0, 0, 0 };
    bool have_fwd = false;
};

struct Pass {
    unsigned index = 0;
    Surf rt[4];
    Surf ds;
    unsigned clears = 0;
    unsigned clear_flags = 0;    // union
    unsigned draws = 0, prims = 0;
    bool begin_scene = false, end_scene = false;
    unsigned long long vs_hash[kMaxShaders] = {};
    unsigned vs_count[kMaxShaders] = {};
    unsigned vs_n = 0, vs_other = 0;
    Cam cam[kMaxCams];
    unsigned cam_n = 0, cam_other = 0;
};

struct Frame {
    unsigned number = 0;
    Pass passes[kMaxPasses];
    unsigned pass_n = 0;
    unsigned overflow = 0;
    drawpolicy::ProjParams elected;
    bool have_elected = false;
    unsigned total_draws = 0;
};

// All state is guarded by one mutex. The hot-path entry points check
// g_armed first, without the lock: when not capturing, that is the whole cost.
std::mutex g_mutex;
volatile bool g_armed = false;
unsigned g_frames_wanted = 0;
unsigned g_frame_n = 0;
Frame    g_frames[kMaxFrames];
unsigned g_dump_index = 0;
unsigned g_captures = 0;

// The render-target set carried across passes: SetRenderTarget on one slot
// does not unbind the others, so the "current set" persists until changed.
Surf g_cur_rt[4];
Surf g_cur_ds;
bool g_pass_open = false;

Frame& cur_frame() { return g_frames[g_frame_n < kMaxFrames ? g_frame_n : kMaxFrames - 1]; }

// A new pass begins at the first draw after the RT set changed. Called with
// the lock held.
Pass* open_pass()
{
    Frame& f = cur_frame();
    if (f.pass_n >= kMaxPasses) { ++f.overflow; return nullptr; }
    Pass& p = f.passes[f.pass_n];
    p = Pass{};
    p.index = f.pass_n++;
    for (int i = 0; i < 4; ++i) p.rt[i] = g_cur_rt[i];
    p.ds = g_cur_ds;
    g_pass_open = true;
    return &p;
}

Pass* current_pass()
{
    Frame& f = cur_frame();
    if (!g_pass_open || f.pass_n == 0) return open_pass();
    return &f.passes[f.pass_n - 1];
}

void describe(IDirect3DSurface9* s, Surf& out, bool is_bb)
{
    out = Surf{};
    out.set = true;
    out.ptr = s;
    out.is_bb = is_bb;
    if (!s) return;
    D3DSURFACE_DESC d = {};
    if (SUCCEEDED(s->GetDesc(&d))) {
        out.w = d.Width; out.h = d.Height; out.fmt = d.Format; out.ms = d.MultiSampleType;
    }
}

const char* fmt_name(unsigned f)
{
    switch (f) {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";  case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_R16F: return "R16F";   case D3DFMT_R32F: return "R32F";
        case D3DFMT_G16R16F: return "G16R16F"; case D3DFMT_G32R32F: return "G32R32F";
        case D3DFMT_A2R10G10B10: return "A2R10G10B10"; case D3DFMT_A2B10G10R10: return "A2B10G10R10";
        case D3DFMT_R5G6B5: return "R5G6B5"; case D3DFMT_A8: return "A8"; case D3DFMT_L8: return "L8";
        case D3DFMT_D24S8: return "D24S8"; case D3DFMT_D24X8: return "D24X8"; case D3DFMT_D16: return "D16";
        case D3DFMT_D32: return "D32"; case D3DFMT_D24FS8: return "D24FS8";
        case D3DFMT_G16R16: return "G16R16"; case D3DFMT_A8L8: return "A8L8";
        case D3DFMT_UNKNOWN: return "UNKNOWN";
    }
    // FOURCC formats (INTZ, DF24, NULL, ...) print as their four characters.
    static thread_local char buf[16];
    const unsigned char* c = reinterpret_cast<const unsigned char*>(&f);
    if (c[0] >= 32 && c[0] < 127 && c[1] >= 32 && c[1] < 127 && c[2] >= 32 && c[2] < 127 && c[3] >= 32 && c[3] < 127) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "'%c%c%c%c'", c[0], c[1], c[2], c[3]);
    } else {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "fmt%u", f);
    }
    return buf;
}

void write_surf(FILE* f, const char* label, const Surf& s)
{
    if (!s.set) return;
    if (!s.ptr) { fprintf(f, " %s=null", label); return; }
    fprintf(f, " %s=%p %ux%u %s", label, s.ptr, s.w, s.h, fmt_name(s.fmt));
    if (s.ms > 1) fprintf(f, " ms%u", s.ms);
    if (s.is_bb) fprintf(f, " [BB]");
}

bool same_proj(const drawpolicy::ProjParams& a, const drawpolicy::ProjParams& b)
{
    if (a.perspective != b.perspective) return false;
    if (!a.perspective) return true;
    const float ah = a.tan_half_h(), bh = b.tan_half_h();
    if (!(ah > 1e-6f) || !(bh > 1e-6f)) return false;
    return std::fabs(ah / bh - 1.0f) < 0.01f;
}

void dump()
{
    // Called with the lock held, after the last armed frame.
    const std::string path = vrlog::dump_path("passes", g_dump_index++);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) { VRLOG("[passes] failed to open %s", path.c_str()); return; }

    constexpr float kRad2Deg = 180.0f / 3.14159265358979f;
    fprintf(f, "# bfbc2vr render passes  launch=%s  frames=%u\n", vrlog::launch_stamp().c_str(), g_frame_n);
    fprintf(f, "# One line per pass, in order. A pass = a contiguous run of draws with the same render-target set.\n");
    fprintf(f, "# cam: every c185 projection written during the pass; ELECTED = the one the frame's camera election adopted.\n");
    fprintf(f, "# fov is the full horizontal x vertical field in degrees; fwd is the c189 forward row.\n\n");

    for (unsigned fi = 0; fi < g_frame_n && fi < kMaxFrames; ++fi) {
        const Frame& fr = g_frames[fi];
        fprintf(f, "frame %u  passes=%u draws=%u", fr.number, fr.pass_n, fr.total_draws);
        if (fr.overflow) fprintf(f, "  (+%u passes dropped, table full)", fr.overflow);
        if (fr.have_elected && fr.elected.perspective) {
            fprintf(f, "  elected fov=%.1fx%.1f",
                    2.0f * std::atan(fr.elected.tan_half_h()) * kRad2Deg,
                    2.0f * std::atan(fr.elected.tan_half_v()) * kRad2Deg);
        } else {
            fprintf(f, "  elected=none");
        }
        fprintf(f, "\n");

        for (unsigned pi = 0; pi < fr.pass_n; ++pi) {
            const Pass& p = fr.passes[pi];
            fprintf(f, "  pass %02u", p.index);
            for (int i = 0; i < 4; ++i) { char l[8]; _snprintf_s(l, sizeof(l), _TRUNCATE, "RT%d", i); write_surf(f, l, p.rt[i]); }
            write_surf(f, "DS", p.ds);
            if (p.begin_scene || p.end_scene) fprintf(f, " scene=%s%s", p.begin_scene ? "B" : "", p.end_scene ? "E" : "");
            if (p.clears) {
                fprintf(f, " clears=%u(", p.clears);
                bool first = true;
                if (p.clear_flags & D3DCLEAR_TARGET)  { fprintf(f, "%sTARGET",  first ? "" : "|"); first = false; }
                if (p.clear_flags & D3DCLEAR_ZBUFFER) { fprintf(f, "%sZ",       first ? "" : "|"); first = false; }
                if (p.clear_flags & D3DCLEAR_STENCIL) { fprintf(f, "%sSTENCIL", first ? "" : "|"); first = false; }
                fprintf(f, ")");
            }
            fprintf(f, " draws=%u prims=%u", p.draws, p.prims);

            for (unsigned ci = 0; ci < p.cam_n; ++ci) {
                const Cam& c = p.cam[ci];
                fprintf(f, " | cam%u writes=%u", ci, c.writes);
                if (c.proj.perspective) {
                    fprintf(f, " fov=%.1fx%.1f near=%.2f",
                            2.0f * std::atan(c.proj.tan_half_h()) * kRad2Deg,
                            2.0f * std::atan(c.proj.tan_half_v()) * kRad2Deg, c.proj.near_z());
                } else {
                    fprintf(f, " non-perspective");
                }
                if (c.have_fwd) fprintf(f, " fwd=(%.2f,%.2f,%.2f)", c.fwd[0], c.fwd[1], c.fwd[2]);
                if (fr.have_elected && same_proj(c.proj, fr.elected)) fprintf(f, " ELECTED");
            }
            if (p.cam_other) fprintf(f, " | +%u more cam", p.cam_other);

            if (p.vs_n) {
                fprintf(f, " | vs:");
                for (unsigned si = 0; si < p.vs_n; ++si) fprintf(f, " %016llX x%u", p.vs_hash[si], p.vs_count[si]);
                if (p.vs_other) fprintf(f, " +%u other", p.vs_other);
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }

    // Summary: every distinct RT0 seen, how many passes and draws it took.
    fprintf(f, "# --- targets summary (RT0 identity -> passes, draws across the capture) ---\n");
    struct Sum { const void* ptr; unsigned w, h, fmt; bool bb; unsigned passes, draws; };
    Sum sums[128]; unsigned sums_n = 0;
    for (unsigned fi = 0; fi < g_frame_n && fi < kMaxFrames; ++fi) {
        const Frame& fr = g_frames[fi];
        for (unsigned pi = 0; pi < fr.pass_n; ++pi) {
            const Surf& s = fr.passes[pi].rt[0];
            unsigned k = 0;
            for (; k < sums_n; ++k) if (sums[k].ptr == s.ptr) break;
            if (k == sums_n) {
                if (sums_n >= 128) continue;
                sums[sums_n++] = { s.ptr, s.w, s.h, s.fmt, s.is_bb, 0, 0 };
            }
            ++sums[k].passes; sums[k].draws += fr.passes[pi].draws;
        }
    }
    for (unsigned k = 0; k < sums_n; ++k) {
        fprintf(f, "%p %ux%u %-14s%s passes=%u draws=%u\n", sums[k].ptr, sums[k].w, sums[k].h,
                fmt_name(sums[k].fmt), sums[k].bb ? " [BB]" : "", sums[k].passes, sums[k].draws);
    }
    fclose(f);
    VRLOG("[passes] wrote %s (%u frames)", path.c_str(), g_frame_n);
}

} // namespace

void on_set_render_target(DWORD slot, IDirect3DSurface9* surface, bool is_backbuffer)
{
    if (!g_armed || slot > 3) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    describe(surface, g_cur_rt[slot], is_backbuffer);
    g_pass_open = false;   // next draw starts a new pass
}

void on_set_depth_stencil(IDirect3DSurface9* surface)
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    describe(surface, g_cur_ds, false);
    g_pass_open = false;
}

void on_clear(DWORD flags, D3DCOLOR, float, DWORD)
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    // A clear belongs to the pass that is about to draw into these targets.
    if (Pass* p = current_pass()) { ++p->clears; p->clear_flags |= flags; }
}

void on_begin_scene()
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Pass* p = current_pass()) p->begin_scene = true;
}

void on_end_scene()
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Frame& f = cur_frame();
    if (f.pass_n) f.passes[f.pass_n - 1].end_scene = true;
}

void on_draw(unsigned long long vs_hash, unsigned prims)
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Pass* p = current_pass();
    if (!p) return;
    ++p->draws; p->prims += prims; ++cur_frame().total_draws;
    if (!vs_hash) return;
    for (unsigned i = 0; i < p->vs_n; ++i) if (p->vs_hash[i] == vs_hash) { ++p->vs_count[i]; return; }
    if (p->vs_n < kMaxShaders) { p->vs_hash[p->vs_n] = vs_hash; p->vs_count[p->vs_n] = 1; ++p->vs_n; }
    else ++p->vs_other;
}

void on_camera_write(const drawpolicy::ProjParams& proj)
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Pass* p = current_pass();
    if (!p) return;
    for (unsigned i = 0; i < p->cam_n; ++i) if (same_proj(p->cam[i].proj, proj)) { ++p->cam[i].writes; return; }
    if (p->cam_n < kMaxCams) { p->cam[p->cam_n].proj = proj; p->cam[p->cam_n].writes = 1; ++p->cam_n; }
    else ++p->cam_other;
}

void on_camera_heading(const float rows[9])
{
    if (!g_armed || !rows) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Frame& f = cur_frame();
    if (!f.pass_n) return;
    Pass& p = f.passes[f.pass_n - 1];
    if (!p.cam_n) return;
    // Attach to the projection most recently written in this pass.
    Cam& c = p.cam[p.cam_n - 1];
    c.fwd[0] = rows[6]; c.fwd[1] = rows[7]; c.fwd[2] = rows[8];
    c.have_fwd = true;
}

void on_camera_elected(const drawpolicy::ProjParams& proj)
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    // The election happens at the START of the next Present for the frame
    // that just finished, so it refers to the most recently closed frame.
    const unsigned idx = g_frame_n ? g_frame_n - 1 : 0;
    if (idx >= kMaxFrames) return;
    g_frames[idx].elected = proj;
    g_frames[idx].have_elected = true;
}

void on_present()
{
    if (!g_armed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frame_n;
    g_pass_open = false;
    if (g_frame_n < g_frames_wanted && g_frame_n < kMaxFrames) {
        g_frames[g_frame_n] = Frame{};
        g_frames[g_frame_n].number = g_frame_n;
        return;
    }
    // Window closed. The election for the final frame arrives on the NEXT
    // Present, after this dump; accept that the last frame may read
    // elected=none rather than hold the recorder open for it.
    g_armed = false;
    dump();
    ++g_captures;
}

void request_capture(unsigned frames)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_armed) return;
    if (frames == 0) frames = 2;
    if (frames > kMaxFrames) frames = kMaxFrames;
    g_frames_wanted = frames;
    g_frame_n = 0;
    for (unsigned i = 0; i < kMaxFrames; ++i) g_frames[i] = Frame{};
    g_frames[0].number = 0;
    for (int i = 0; i < 4; ++i) g_cur_rt[i] = Surf{};
    g_cur_ds = Surf{};
    g_pass_open = false;
    g_armed = true;
    VRLOG("[passes] armed for %u frame(s)", frames);
}

bool capturing() { return g_armed; }

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    if (strcmp(cmd, "passes") != 0) return false;
    unsigned frames = 2;
    if (args && args[0]) { const int v = atoi(args); if (v > 0) frames = static_cast<unsigned>(v); }
    if (g_armed) { _snprintf_s(reply, n, _TRUNCATE, "passes: capture already running"); return true; }
    request_capture(frames);
    _snprintf_s(reply, n, _TRUNCATE, "passes: capturing %u frame(s) -> bfbc2vr_passes_<stamp>_NN.txt",
                frames > kMaxFrames ? kMaxFrames : frames);
    return true;
}

void status(FILE* f)
{
    fprintf(f, "passes: %s captures=%u\n", g_armed ? "CAPTURING" : "idle", g_captures);
}

} // namespace rpass
