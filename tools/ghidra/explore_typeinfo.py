"""Exploratory: read the `typeinfo` and `fieldinf` PE sections of BFBC2Game.exe
raw, so the record layouts can be inferred from evidence before a parser
encodes them. Prints the first records of each section decoded as dwords with
string pointers resolved, and dumps one known class (ShotConfigData) end to end.
"""
import struct, sys

EXE = sys.argv[1] if len(sys.argv) > 1 else r'build\ghidra\BFBC2Game.exe'
d = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
BASE = struct.unpack_from('<I', d, pe + 24 + 28)[0]
secs = []
off = pe + 24 + opt
for i in range(nsec):
    name, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, off + i * 40)
    secs.append((name.rstrip(b'\0').decode(), va, vs, rp, rs))
print('image base %08X' % BASE)

def sec(n):
    for s in secs:
        if s[0] == n: return s
    raise KeyError(n)

def va2off(va):
    rva = va - BASE
    for _, sva, svs, srp, srs in secs:
        if sva <= rva < sva + max(svs, srs): return srp + (rva - sva)
    return None

def dw(va):
    o = va2off(va)
    return struct.unpack_from('<I', d, o)[0] if o is not None and o + 4 <= len(d) else None

def cstr(va, maxlen=96):
    o = va2off(va)
    if o is None: return None
    e = d.find(b'\0', o, o + maxlen)
    if e < 0: return None
    s = d[o:e]
    if not s or any(c < 32 or c > 126 for c in s): return None
    return s.decode()

def which(va):
    rva = va - BASE
    for n, sva, svs, srp, srs in secs:
        if sva <= rva < sva + max(svs, srs): return n
    return '?'

def show(va, n, label=''):
    print(f'--- {label} @{va:08X} ---')
    for i in range(n):
        a = va + i * 4
        v = dw(a)
        if v is None: print(f'  {a:08X}: ????'); continue
        s = cstr(v) if v >= BASE else None
        tag = f'"{s}"' if s else (which(v) if BASE <= v < BASE + 0x2000000 else '')
        print(f'  {a:08X}: {v:08X}  {tag}')

ti = sec('typeinfo'); fi = sec('fieldinf')
TI = BASE + ti[1]; FI = BASE + fi[1]
print(f'typeinfo va={TI:08X} size={ti[2]:X}   fieldinf va={FI:08X} size={fi[2]:X}')

show(TI, 40, 'start of typeinfo')
show(FI, 40, 'start of fieldinf')

# ShotConfigData: name string at 01452DA0 (from Ghidra). Find its record.
target = None
for a in range(TI, TI + ti[2], 4):
    v = dw(a)
    if v and cstr(v) == 'ShotConfigData': target = a; break
print('\nShotConfigData record at', f'{target:08X}' if target else None)
if target:
    show(target - 8, 16, 'ShotConfigData record (+neighbours)')
    fields = dw(target + 24)
    print(f'\ntrailing ptr -> {fields:08X} ({which(fields)})')
    show(fields, 48, 'fieldinf at trailing ptr')
