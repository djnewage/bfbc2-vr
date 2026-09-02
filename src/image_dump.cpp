#include "image_dump.h"
#include "logger.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace imgdump {
namespace {

// Copy [addr, addr+len) into out, substituting zeros for pages that cannot be
// read. Page-granular so one bad page does not lose a whole section.
void read_pages(const unsigned char* addr, size_t len, std::vector<unsigned char>& out, size_t at,
                size_t& unreadable)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    size_t off = 0;
    while (off < len) {
        const size_t n = (std::min)(page - (reinterpret_cast<uintptr_t>(addr + off) % page), len - off);
        MEMORY_BASIC_INFORMATION mbi = {};
        bool ok = VirtualQuery(addr + off, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                  mbi.State == MEM_COMMIT &&
                  !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS) && mbi.Protect != 0;
        if (ok) {
            __try { std::memcpy(&out[at + off], addr + off, n); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        }
        if (!ok) { std::memset(&out[at + off], 0, n); unreadable += n; }
        off += n;
    }
}

} // namespace

bool dump(char* path_out, size_t path_size)
{
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    std::vector<unsigned char> img(image_size);
    size_t unreadable = 0;
    read_pages(base, image_size, img, 0, unreadable);

    // Rewrite the copy's section table so raw layout == virtual layout.
    auto* nt2 = reinterpret_cast<IMAGE_NT_HEADERS32*>(&img[dos->e_lfanew]);
    nt2->OptionalHeader.FileAlignment = nt2->OptionalHeader.SectionAlignment;
    auto* sec = IMAGE_FIRST_SECTION(nt2);
    const DWORD align = nt2->OptionalHeader.SectionAlignment;
    for (unsigned i = 0; i < nt2->FileHeader.NumberOfSections; ++i) {
        sec[i].PointerToRawData = sec[i].VirtualAddress;
        sec[i].SizeOfRawData = (sec[i].Misc.VirtualSize + align - 1) / align * align;
        // Was encrypted on disk; is code now. Let the analyzer treat it as such.
        sec[i].Characteristics |= IMAGE_SCN_MEM_READ;
    }

    const std::string path = vrlog::dump_path("image", 0, "exe");
    if (path_out && path_size) _snprintf_s(path_out, path_size, _TRUNCATE, "%s", path.c_str());
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return false;
    const size_t wrote = fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    VRLOG("[image] wrote %s: base=%p size=0x%zx sections=%u unreadable=0x%zx",
          path.c_str(), static_cast<const void*>(base), image_size,
          nt2->FileHeader.NumberOfSections, unreadable);
    return wrote == img.size();
}

bool command(const char* cmd, const char* args, char* reply, size_t n)
{
    (void)args;
    if (strcmp(cmd, "dumpimage") != 0) return false;
    char path[MAX_PATH] = {};
    if (dump(path, sizeof(path))) _snprintf_s(reply, n, _TRUNCATE, "dumpimage: wrote %s", path);
    else                          _snprintf_s(reply, n, _TRUNCATE, "dumpimage: failed (see log)");
    return true;
}

} // namespace imgdump
