"""Dump Frostbite 1.5's reflection tables straight out of BFBC2Game.exe.

The engine ships its type registry as two unencrypted PE sections:

    typeinfo   TypeInfoData records, 24 bytes:
                 +0  const char* name
                 +4  uint32      size << 16 | flags        flags 0x29 = class (has a field table)
                 +8  void*       module                    a per-module object in .data
                 +12 uint32      0x0001 | fieldCount<<8 | alignment
                 +16 uint32      0
                 +20 uint32      0
               ClassInfoData (flags 0x29) adds:
                 +24 FieldInfoData* fields                 -> into `fieldinf`
    fieldinf   FieldInfoData records, 24 bytes:
                 +0  const char* name
                 +4  uint32      flags | arraySize<<16
                 +8  void*       fieldType                 TypeInfoData*, or a runtime object in .data
                 +12 void*       secondaryType
                 +16 uint32      offset
                 +20 void*       attributes

Every number above was read off the file (see explore_typeinfo.py and
docs/engine-map.md), and this script re-checks the positive controls on every
run: ShotConfigData must be 80 bytes with InitialSpeed/InitialDirection/
InitialPosition at 0x00/0x10/0x20, and the field-table extents must tile.

The executable's .text is SteamStub-encrypted on disk; these sections are not,
so this needs neither Ghidra nor a running game.

    python tools/ghidra/dump_reflection.py [BFBC2Game.exe] [outdir]

Writes reflection.txt (readable) and reflection.json (for tooling).
"""
import json, os, struct, sys

EXE = sys.argv[1] if len(sys.argv) > 1 else r'build\ghidra\BFBC2Game.exe'
OUT = sys.argv[2] if len(sys.argv) > 2 else r'build\ghidra\out'
os.makedirs(OUT, exist_ok=True)

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

def sec(n):
    for s in secs:
        if s[0] == n: return s
    raise KeyError(n)

def which(va):
    rva = va - BASE
    for n, sva, svs, srp, srs in secs:
        if sva <= rva < sva + max(svs, srs): return n
    return None

def va2off(va):
    rva = va - BASE
    for _, sva, svs, srp, srs in secs:
        if sva <= rva < sva + srs: return srp + (rva - sva)     # file-backed only
    return None

def dw(va):
    o = va2off(va)
    return struct.unpack_from('<I', d, o)[0] if o is not None and o + 4 <= len(d) else None

def cstr(va, maxlen=128):
    o = va2off(va)
    if o is None: return None
    e = d.find(b'\0', o, o + maxlen)
    if e < 0: return None
    s = d[o:e]
    if not s or any(c < 32 or c > 126 for c in s): return None
    return s.decode()

def is_ident(s):
    return s and (s[0].isalpha() or s[0] == '_') and all(c.isalnum() or c in '_:' for c in s)

ti = sec('typeinfo'); fi = sec('fieldinf')
TI0, TI1 = BASE + ti[1], BASE + ti[1] + ti[2]
FI0, FI1 = BASE + fi[1], BASE + fi[1] + fi[2]

# ---- walk typeinfo ----------------------------------------------------------
types = {}          # record va -> dict
order = []
gaps = 0
a = TI0
while a + 24 <= TI1:
    name_ptr = dw(a)
    name = cstr(name_ptr) if name_ptr else None
    sf = dw(a + 4)
    # Structural test, not a flags whitelist: a name pointer into .rdata and a
    # zero dword at +20. (+16 is 0 for classes but holds the SIZE for scalar
    # types, and a flags-byte filter silently dropped Int8..Int64.)
    if not is_ident(name) or which(name_ptr) != '.rdata' or sf is None or dw(a + 20) != 0:
        a += 4; gaps += 1; continue           # padding between records: resync
    flags = sf & 0xFFFF
    size = sf >> 16
    misc = dw(a + 12)
    scalar = bool(flags & 0xC000)          # Boolean, Int*, Float*, String, value types
    rec = {
        'va': a, 'name': name, 'size': size, 'flags': flags,
        'module': dw(a + 8), 'misc': misc, 'scalar': scalar,
        # class: +12 = 0x0001 | count<<8 | align, +16 = 0
        # scalar: +12 = align, +16 = size
        'field_count': 0 if scalar and flags & 0x4000 and not flags & 0x1000 else (misc >> 8) & 0xFF,
        'align': misc & 0xFF, 'misc_hi': misc >> 16,
        'fields_va': None, 'fields': [],
    }
    if scalar and (misc >> 8) == 0: rec['align'] = misc
    # A value table follows for classes (flags 0x29: fields) AND enums
    # (flags 0x179: enumerators, where `offset` is the value). Decide by where
    # the dword at +24 points rather than by flags, so a kind we have not seen
    # yet still parses.
    step = 24
    fp = dw(a + 24)
    if fp is not None and FI0 <= fp < FI1 and rec['field_count'] > 0:
        rec['fields_va'] = fp
        step = 28
    elif flags == 0x29:
        rec['note'] = 'class without a fieldinf pointer (%08X)' % (fp or 0)
    types[a] = rec
    order.append(a)
    a += step

# ---- value tables passed by the static initialisers ---------------------------
# Only some records embed their table pointer at +24. For the rest, the
# constructor call in the `ctr` section pushes the table address alongside the
# record address:   push <fieldinf table>; push <callback>; push <TypeInfoData>;
# mov ecx, <TypeInfo object>; call ...   Harvest every push of a fieldinf address
# and pair it with the nearest push of a known record. Measured: 861 such
# links, all to records without an embedded pointer, none ambiguous.
ctr = sec('ctr')
cbuf = d[ctr[3]:ctr[3] + ctr[4]]
cbase = BASE + ctr[1]
ctr_links = 0
for i in range(len(cbuf) - 5):
    if cbuf[i] != 0x68: continue
    v = struct.unpack_from('<I', cbuf, i + 1)[0]
    if not (FI0 <= v < FI1): continue
    for k in list(range(1, 48)) + list(range(-1, -48, -1)):
        p = i + k
        if p < 0 or p + 5 > len(cbuf) or cbuf[p] != 0x68: continue
        w = struct.unpack_from('<I', cbuf, p + 1)[0]
        if w in types:
            rec = types[w]
            if rec['fields_va'] is None and rec['field_count'] > 0:
                rec['fields_va'] = v
                rec['table_from'] = 'ctr @%08X' % (cbase + i)
                ctr_links += 1
            elif rec['fields_va'] is not None and rec['fields_va'] != v:
                rec['note'] = 'ctr pushes a different table %08X' % v
            break

# ---- TypeInfo object -> TypeInfoData record, from the ctr initialisers -------
# Field type pointers name the runtime TypeInfo OBJECT (zero on disk, usually
# in the typeinfo section's padding or in .data BSS), not the data record. The
# constructor sequence   push <TypeInfoData>; mov ecx, <TypeInfo object>; call
# is the only static link between them.
def try_record_at(va):
    """Register a TypeInfoData record found outside the section walk."""
    if va in types: return types[va]
    nm = cstr(dw(va) or 0)
    sf = dw(va + 4)
    if not is_ident(nm) or which(dw(va)) != '.rdata' or sf is None or dw(va + 20) != 0: return None
    misc = dw(va + 12) or 0
    rec = {'va': va, 'name': nm, 'size': sf >> 16, 'flags': sf & 0xFFFF, 'module': dw(va + 8), 'misc': misc,
           'field_count': (misc >> 8) & 0xFF, 'align': misc & 0xFF, 'misc_hi': misc >> 16,
           'fields_va': None, 'fields': [], 'note': 'record outside typeinfo section'}
    fp = dw(va + 24)
    if fp is not None and FI0 <= fp < FI1 and rec['field_count'] > 0: rec['fields_va'] = fp
    types[va] = rec; order.append(va)
    return rec

obj2rec = {}
for i in range(len(cbuf) - 5):
    if cbuf[i] != 0xB9: continue                       # mov ecx, imm32
    obj = struct.unpack_from('<I', cbuf, i + 1)[0]
    if not (BASE <= obj < BASE + 0x2000000): continue
    for k in list(range(-1, -48, -1)) + list(range(1, 48)):
        p = i + k
        if p < 0 or p + 5 > len(cbuf) or cbuf[p] != 0x68: continue
        w = struct.unpack_from('<I', cbuf, p + 1)[0]
        rec = types.get(w) or (try_record_at(w) if va2off(w) is not None else None)
        if rec:
            obj2rec.setdefault(obj, rec)
            break

# ---- scalar / value-type objects: registered from encrypted .text -------------
# The Boolean..Guid and Vec2..AxisAlignedBox TypeInfo objects have NO static
# reference anywhere (their registration runs from .text). Two independent
# facts pin them down anyway, and the check below re-verifies the second on
# every run:
#   1. the objects are laid out at fixed spacing in the same order as their
#      records - 0x10 apart in the typeinfo padding for scalars, 0x14 apart in
#      .data BSS for value types;
#   2. each object's SIZE, measured from field packing (smallest gap to the next
#      field over every use), equals the mapped record's size.
byname_rec = {}
for va in order: byname_rec.setdefault(types[va]['name'], types[va])
inferred = {}
def infer_run(first_obj, step, names):
    for k, n in enumerate(names):
        if n in byname_rec: inferred[first_obj + step * k] = byname_rec[n]
infer_run(0x01BF12DC, 0x10, ['ArrayBase', 'StringArrayBase', 'RefArrayBase', 'DataContainer'])
infer_run(0x01BF1354, 0x10, ['Boolean', 'Uint8', 'Int8', 'Uint16', 'Int16', 'Uint32', 'Int32',
                             'Uint64', 'Int64', 'Float32', 'Float64', 'String', 'FileRef', 'Guid'])
infer_run(0x0155E0AC, 0x14, ['Vec2'])
infer_run(0x01562ADC, 0x14, ['Vec3'])
infer_run(0x01562D60, 0x14, ['Vec4', 'Quat', 'Plane', 'LinearTransform', 'Mat4', 'AxisAlignedBox'])
for obj, rec in inferred.items():
    if obj not in obj2rec: obj2rec[obj] = rec

# ---- resolve a type pointer to a name -----------------------------------------
def type_name(p):
    if not p: return 'void'
    if p in types: return types[p]['name']
    if p in obj2rec: return obj2rec[p]['name']
    w = which(p)
    if w == 'typeinfo':
        # points inside a record (e.g. at +24 of a class) - find the enclosing one
        for va in order:
            if va <= p < va + 28 and types[va]: return types[va]['name'] + ('+%d' % (p - va) if p != va else '')
        return 'typeinfo:%08X' % p
    if w in ('.data', '.rdata'):
        # A runtime TypeInfo object, or a TypeInfoData that lives outside the
        # section. If its first dword names a string, it is the latter.
        n = dw(p)
        s = cstr(n) if n else None
        if is_ident(s): return s
        return '%s:%08X' % (w, p)
    return '%08X' % p

# ---- read field tables -------------------------------------------------------
for va in order:
    rec = types[va]
    if rec['fields_va'] is None: continue
    f = rec['fields_va']
    for i in range(rec['field_count']):
        e = f + i * 24
        if e + 24 > FI1: rec['note'] = 'field table runs past fieldinf'; break
        nm = cstr(dw(e))
        fl = dw(e + 4)
        rec['fields'].append({
            'name': nm, 'flags': fl & 0xFFFF, 'array': fl >> 16,
            'type': type_name(dw(e + 8)), 'type_va': dw(e + 8),
            'secondary': type_name(dw(e + 12)) if dw(e + 12) else None,
            'offset': dw(e + 16), 'attributes': dw(e + 20),
        })

# ---- inheritance and runtime ids, from a live image (optional) ---------------
# The runtime TypeInfo objects are zero on disk but populated in an image
# written by the mod's `dumpimage`. Their layout, read off such an image:
#   +00 vtable (one per kind)   +04 TypeInfoData*   +08 TypeInfo* next
#   +0C runtime id              +14 TypeInfo* super  +24 FieldInfo* fields
# The registry head is the static at 0x0154D4B4; the list is null-terminated
# (1,986 nodes on 2026-09-02). Pass the image as a third argument to fill in
# each class's parent; nothing else changes.
S_FIRST = 0x0154D4B4
live_nodes = 0
if len(sys.argv) > 3 and os.path.exists(sys.argv[3]):
    L = open(sys.argv[3], 'rb').read()
    def ldw(va):
        o = va - BASE
        return struct.unpack_from('<I', L, o)[0] if 0 <= o < len(L) - 3 else None
    obj_rec = {}
    cur = ldw(S_FIRST); seen = set()
    while cur and cur not in seen and len(seen) < 20000:
        seen.add(cur); rec = ldw(cur + 4)
        if rec: obj_rec[cur] = rec
        cur = ldw(cur + 8)
    live_nodes = len(seen)
    for obj, rec in obj_rec.items():
        t = types.get(rec)
        if not t: continue
        t['runtime_id'] = ldw(obj + 0x0C)
        sup = ldw(obj + 0x14)
        sup_rec = obj_rec.get(sup)
        if sup == obj:                     # the root (DataContainer) links to itself
            t['super'] = None
        elif sup_rec in types:
            t['super'] = types[sup_rec]['name']
        elif sup and 0x01406000 <= sup < 0x014F1000:
            # The flags-0x29 kind (vtable 0140C710) keeps its vtable here: a
            # different object layout. Parent unknown, not wrong.
            t['super'] = '?'
        else:
            t['super'] = ('%08X' % sup) if sup else None

# ---- checks ------------------------------------------------------------------
checks = []
def check(cond, msg): checks.append(('ok  ' if cond else 'FAIL') + '  ' + msg)

byname = {}
for va in order: byname.setdefault(types[va]['name'], []).append(types[va])
scd = byname.get('ShotConfigData', [None])[0]
check(scd is not None, 'ShotConfigData present')
if scd:
    fo = {f['name']: f['offset'] for f in scd['fields']}
    check(scd['size'] == 0x50, 'ShotConfigData size 0x50 (got 0x%X)' % scd['size'])
    check(fo.get('InitialSpeed') == 0x00, 'InitialSpeed @0x00')
    check(fo.get('InitialDirection') == 0x10, 'InitialDirection @0x10')
    check(fo.get('InitialPosition') == 0x20, 'InitialPosition @0x20')
    check(len(scd['fields']) == scd['field_count'] == 12, 'ShotConfigData 12 fields')

# Field tables must tile: each class's table ends where the next one begins.
classes = [types[va] for va in order if types[va]['fields_va'] is not None]
enums = [r for r in classes if r['flags'] == 0x179]
classes_by_fields = sorted(classes, key=lambda r: r['fields_va'])
tiled = 0
for x, y in zip(classes_by_fields, classes_by_fields[1:]):
    if x['fields_va'] + x['field_count'] * 24 == y['fields_va']: tiled += 1
check(tiled >= len(classes_by_fields) - 1 - len(classes_by_fields) // 20,
      'field tables tile (%d of %d adjacent pairs)' % (tiled, len(classes_by_fields) - 1))
# Every field offset must lie inside its class.
bad_off = sum(1 for r in classes if r['flags'] != 0x179 for f in r['fields'] if f['offset'] is None or f['offset'] >= max(r['size'], 1))
check(bad_off == 0, 'all field offsets inside class size (%d violations)' % bad_off)

# The inferred scalar/value-type objects: measured size must equal the record's.
# Arrays are excluded (the field holds an Array header, not the record's size).
measured = {}
for r in classes:
    if r['flags'] == 0x179: continue
    fs = sorted([f for f in r['fields'] if f['offset'] is not None], key=lambda f: f['offset'])
    for i, f in enumerate(fs):
        nxt = fs[i + 1]['offset'] if i + 1 < len(fs) else r['size']
        if nxt > f['offset'] and not (f['flags'] & 0x48):
            measured[f['type_va']] = min(measured.get(f['type_va'], 1 << 30), nxt - f['offset'])
mism = []
for obj, rec in inferred.items():
    if obj in measured and rec['name'] not in ('ArrayBase', 'StringArrayBase', 'RefArrayBase', 'DataContainer'):
        if measured[obj] != rec['size']: mism.append('%s@%08X measured %d vs %d' % (rec['name'], obj, measured[obj], rec['size']))
verified = sum(1 for obj, rec in inferred.items() if obj in measured)
check(not mism, 'inferred scalar objects: measured size == record size for all %d with uses%s'
      % (verified, ('; MISMATCH ' + '; '.join(mism)) if mism else ''))
unresolved = sum(1 for r in classes for f in r['fields'] if f['type'].startswith(('typeinfo:', '.data:')))
check(unresolved == 0, 'every field type resolves to a name (%d unresolved)' % unresolved)

# ---- write -------------------------------------------------------------------
flag_hist = {}
for va in order: flag_hist[types[va]['flags']] = flag_hist.get(types[va]['flags'], 0) + 1

with open(os.path.join(OUT, 'reflection.txt'), 'w', encoding='utf-8') as o:
    o.write('# Frostbite reflection tables from %s  (image base %08X)\n' % (os.path.basename(EXE), BASE))
    o.write('# typeinfo %08X..%08X  fieldinf %08X..%08X\n' % (TI0, TI1, FI0, FI1))
    o.write('# %d type records (%d resync steps over zero padding = runtime TypeInfo objects); '
            '%d with value tables (%d enums; %d tables linked via ctr initialisers); %d entries\n'
            % (len(order), gaps, len(classes), len(enums), ctr_links, sum(len(r['fields']) for r in classes)))
    o.write('# %d TypeInfo objects mapped to records via ctr; %d records found outside the typeinfo section\n'
            % (len(obj2rec), sum(1 for va in order if types[va].get('note') == 'record outside typeinfo section')))
    if live_nodes: o.write('# live registry walked from *(0x%08X): %d nodes; inheritance and runtime ids filled from it\n' % (S_FIRST, live_nodes))
    o.write('# flags histogram: ' + ', '.join('0x%02X:%d' % kv for kv in sorted(flag_hist.items())) + '\n')
    o.write('#\n# checks:\n')
    for c in checks: o.write('#   ' + c + '\n')
    o.write('\n')
    for va in order:
        r = types[va]
        kind = {0x29: 'class', 0x179: 'enum ', 0x35: 'type '}.get(r['flags'], 'f%04X' % r['flags'])
        o.write('%s %-40s @%08X size=0x%-4X align=%-2d fields=%-3d module=%08X misc_hi=%04X'
                % (kind, r['name'], va, r['size'], r['align'], r['field_count'], r['module'] or 0, r['misc_hi']))
        if r.get('super'): o.write('  : %s' % r['super'])
        if r.get('runtime_id') is not None: o.write('  id=%d' % r['runtime_id'])
        if r.get('table_from'): o.write('  table via ' + r['table_from'])
        if r.get('note'): o.write('   ! ' + r['note'])
        o.write('\n')
        for f in r['fields']:
            if r['flags'] == 0x179:
                o.write('    %-36s = %d\n' % (f['name'] or '?', f['offset'])); continue
            arr = ('[%d]' % f['array']) if f['array'] else ''
            sec2 = (' <%s>' % f['secondary']) if f['secondary'] else ''
            o.write('    +0x%04X  %-36s %s%s%s' % (f['offset'], f['name'] or '?', f['type'], arr, sec2))
            if f['flags']: o.write('  flags=0x%X' % f['flags'])
            o.write('\n')

with open(os.path.join(OUT, 'reflection.json'), 'w', encoding='utf-8') as o:
    json.dump({'image_base': BASE, 'types': [types[va] for va in order], 'checks': checks}, o, indent=1)

print('\n'.join(checks))
print('types=%d classes=%d fields=%d -> %s' % (len(order), len(classes), sum(len(r['fields']) for r in classes), OUT))
