"""Find the live GameRenderSettings instance in a running BFBC2Game.exe.

Read-only: OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION), walk the
committed readable regions, and test every 4-aligned address against the shape
the reflection tables give the class (docs/recon/reflection-bfbc2.txt):

    +0x0C Float32 ViewDistance          +0x10 Float32 NearPlane
    +0x14 Float32 EdgeModelScreenAreaScale
    +0x18 Float32 ForceFov              +0x1C Uint32  SkipMipmapCount
    +0x5C Int32   Dx9AdapterIndex       +0x64 Uint32  MultisampleCount
    +0x6C enum    Renderer              +0x70..+0xA3  Booleans (with a few
                                                      Float32 at 0x30/0x38/0x3C)
    size 0xA4

Singleplayer only. Nothing is written.
"""
import ctypes, ctypes.wintypes as W, struct, sys

k32 = ctypes.windll.kernel32
PROCESS_VM_READ = 0x10; PROCESS_QUERY_INFORMATION = 0x400
MEM_COMMIT = 0x1000; PAGE_GUARD = 0x100; PAGE_NOACCESS = 0x01
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}

class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', W.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]

def pid_of(name):
    import subprocess
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith('"'):
            return int(line.split('","')[1].strip('"'))
    return None

pid = pid_of('BFBC2Game.exe')
if not pid: sys.exit('BFBC2Game.exe is not running')
h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
if not h: sys.exit('OpenProcess failed (%d)' % k32.GetLastError())

k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

def f32(b, o): return struct.unpack_from('<f', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]

BOOL_OFFS = [o for o in range(0x70, 0xA4) if o not in (0x70, 0x71, 0x72, 0x73)]  # 0x70-0x73 are bools too; keep all
BOOL_OFFS = list(range(0x70, 0xA3))   # 0xA3 is padding, not a field
FLOATS_IN_BOOL_REGION = set()   # none per reflection: 0x30,0x38,0x3C are below 0x70

LOOSE = '--loose' in sys.argv
NOVT = '--novt' in sys.argv
RDATA = (0x01406000, 0x014F1000)   # .rdata VA range (RVA 0x1006000 + image base 0x400000)

def looks_like(b, o):
    if o + 0xA4 > len(b): return False
    # DataContainer base: vtable pointer at +0 (into .rdata), then refcount.
    vt = u32(b, o)
    if NOVT is False and not (RDATA[0] <= vt < RDATA[1]): return False
    if u32(b, o + 4) > 100000: return False
    # A running game has these on.
    on = sum(b[o+k] for k in BOOL_OFFS)
    if on < (2 if LOOSE else 6): return False
    vd, near, edge, ffov = f32(b, o+0x0C), f32(b, o+0x10), f32(b, o+0x14), f32(b, o+0x18)
    if not LOOSE and not (0.0 <= vd <= 1e7): return False
    if not LOOSE and not (0.0 <= near <= 10.0): return False
    if not (0.0 <= edge <= 1000.0): return False
    if not (0.0 <= ffov <= 180.0): return False
    if u32(b, o+0x1C) > 16: return False                    # SkipMipmapCount
    if not (-1 <= i32(b, o+0x5C) <= 8): return False        # Dx9AdapterIndex
    if u32(b, o+0x64) not in (0, 1, 2, 4, 8): return False   # MultisampleCount
    if u32(b, o+0x6C) > 8: return False                     # Renderer enum
    bad = sum(1 for k in BOOL_OFFS if b[o+k] > 1)
    return bad == 0

hits = []
addr = 0
mbi = MBI()
while addr < 0x7FFF0000:
    if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)): break
    base, size, prot = mbi.BaseAddress or 0, mbi.RegionSize, mbi.Protect
    if mbi.State == MEM_COMMIT and (prot & 0xFF) in READABLE and not (prot & PAGE_GUARD) and size <= (256 << 20):
        buf = ctypes.create_string_buffer(size); got = ctypes.c_size_t(0)
        if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(got)) and got.value >= 0xA4:
            b = buf.raw[:got.value]
            for o in range(0, len(b) - 0xA4, 4):
                if looks_like(b, o): hits.append((base + o, b[o:o+0xA4]))
    addr = base + size

print('pid %d: %d candidate(s)' % (pid, len(hits)))
for a, b in hits[:40]:
    print('  %08X  vtable=%08X ViewDistance=%.2f NearPlane=%.4f EdgeScale=%.3f ForceFov=%.2f SkipMip=%d AdapterIdx=%d MSAA=%d Renderer=%d Fullscreen=%d ShadowsEnable=%d LockView=%d'
          % (a, u32(b,0), f32(b,0x0C), f32(b,0x10), f32(b,0x14), f32(b,0x18), u32(b,0x1C), i32(b,0x5C), u32(b,0x64), u32(b,0x6C), b[0x86], b[0x80], b[0x81]))
k32.CloseHandle(h)
