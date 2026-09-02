"""Read a few dwords from the running BFBC2Game.exe. Read-only.
    python tools/ghidra/peek.py <hexaddr> [count]
"""
import ctypes, ctypes.wintypes as W, struct, sys, subprocess
k32 = ctypes.windll.kernel32
def pid_of(name):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith('"'): return int(line.split('","')[1].strip('"'))
pid = pid_of('BFBC2Game.exe')
if not pid: sys.exit('not running')
h = k32.OpenProcess(0x10 | 0x400, False, pid)
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
addr = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 16
buf = ctypes.create_string_buffer(n * 4); got = ctypes.c_size_t(0)
if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n * 4, ctypes.byref(got)): sys.exit('read failed at %08X' % addr)
for i in range(got.value // 4):
    v = struct.unpack_from('<I', buf, i * 4)[0]; f = struct.unpack_from('<f', buf, i * 4)[0]
    tag = ''
    if 0x01406000 <= v < 0x014F1000: tag = '.rdata (vtable?)'
    elif 0x00401000 <= v < 0x01005000: tag = '.text'
    elif 0x010F1000 <= v < 0x017F1000: tag = '.data'
    elif 0x017F1000 <= v < 0x01849000: tag = 'typeinfo/fieldinf'
    print('  %08X: %08X  %14.4f  %s' % (addr + i * 4, v, f if abs(f) < 1e8 else float('nan'), tag))
