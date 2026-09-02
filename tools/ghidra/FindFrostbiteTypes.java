// Locate Frostbite's reflection registry in BFBC2Game.exe by walking backwards
// from the type-name strings it contains.
//
// The binary carries ~890 distinct "*Data" type names (ShotConfigData,
// CameraData, EntityData, ...). In Frostbite each is referenced by a
// TypeInfoData record (name pointer + flags + size ...), which is in turn
// referenced by a TypeInfo (vtable, data pointer, next-in-list, ...). Finding
// the pointer chains from the strings, and then the one offset at which
// TypeInfo records point at each other, yields the registry's linked list and
// its head - the root an in-process walker needs.
//
// Everything is a brute pointer scan over the image rather than a reliance on
// the analyzer having recognised these as references; a static pointer in
// .rdata is exactly the kind of thing auto-analysis can miss. Layout is NOT
// assumed from docs/bc2-engine.md - the report prints raw dwords around every
// hit so the layout can be read off the evidence, then encoded.
//
// Usage (headless):
//   analyzeHeadless <proj> bfbc2 -import BFBC2Game.exe -postScript FindFrostbiteTypes.java <outdir>
//
//@category Frostbite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class FindFrostbiteTypes extends GhidraScript {

    // Names used as positive controls. The *Data regex sweep below finds the
    // rest; these are the ones the VR work actually needs.
    static final String[] CONTROLS = {
        "ShotConfigData", "CameraData", "EntityData", "SoldierEntity", "SoldierEntityData",
        "ClientPlayer", "WeaponFiring", "WeaponFiringData", "SoldierWeaponData",
        "GameObjectData", "DataContainer", "TypeInfo", "ClassInfo", "RenderView",
        "ClientWeaponFiringEffects", "ClientSoldierWeapon", "AnimatedSoldier",
        "PlayerManager", "ClientControllableEntity"
    };

    Memory mem;
    List<MemoryBlock> blocks = new ArrayList<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();
        PrintWriter out = new PrintWriter(new File(outDir, "frostbite_types.txt"), StandardCharsets.UTF_8);

        mem = currentProgram.getMemory();
        for (MemoryBlock b : mem.getBlocks()) if (b.isInitialized() && b.isLoaded()) blocks.add(b);

        out.printf("# Frostbite type-name pointer chains in %s%n", currentProgram.getName());
        out.printf("# image base %s%n%n", currentProgram.getImageBase());

        // ---- 1. Find the strings ------------------------------------------------
        // Every NUL-terminated printable string that looks like an identifier
        // ending in "Data", plus the controls. Keyed by address.
        Map<Long, String> strings = new TreeMap<>();
        Set<String> wanted = new HashSet<>(Arrays.asList(CONTROLS));
        for (MemoryBlock b : blocks) {
            byte[] buf = readBlock(b);
            if (buf == null) continue;
            long base = b.getStart().getOffset();
            int i = 0;
            while (i < buf.length) {
                if (!isIdentStart(buf[i])) { i++; continue; }
                int j = i;
                while (j < buf.length && isIdentChar(buf[j])) j++;
                if (j < buf.length && buf[j] == 0 && j - i >= 4 && j - i <= 64) {
                    String s = new String(buf, i, j - i, StandardCharsets.US_ASCII);
                    if (wanted.contains(s) || (s.endsWith("Data") && Character.isUpperCase(s.charAt(0)))) {
                        strings.put(base + i, s);
                    }
                }
                i = j + 1;
            }
        }
        out.printf("# %d candidate type-name strings%n", strings.size());
        int controlsFound = 0;
        for (String c : CONTROLS) if (strings.containsValue(c)) controlsFound++;
        out.printf("# controls present: %d of %d%n%n", controlsFound, CONTROLS.length);

        // ---- 2. Pointers to those strings = TypeInfoData name fields ----------
        Map<Long, Long> ptrToString = new TreeMap<>();   // location of pointer -> string addr
        scanPointers(strings.keySet(), ptrToString);
        out.printf("# %d pointers to type-name strings found in the image%n%n", ptrToString.size());

        // ---- 3. Pointers to (near) those records = TypeInfo -> TypeInfoData ----
        // The name pointer may not be the first field of TypeInfoData, so accept
        // a pointer landing within 32 bytes before it.
        Set<Long> recordAnchors = new HashSet<>();
        for (long p : ptrToString.keySet()) for (int k = 0; k <= 32; k += 4) recordAnchors.add(p - k);
        Map<Long, Long> ptrToRecord = new TreeMap<>();   // location -> anchor hit
        scanPointers(recordAnchors, ptrToRecord);
        out.printf("# %d pointers into type-name records (TypeInfo candidates)%n%n", ptrToRecord.size());

        // ---- 4. Report, one block per control name first ------------------------
        out.println("## Controls");
        for (String c : CONTROLS) {
            Long saddr = null;
            for (Map.Entry<Long, String> e : strings.entrySet()) if (e.getValue().equals(c)) { saddr = e.getKey(); break; }
            if (saddr == null) { out.printf("%-28s NOT FOUND%n", c); continue; }
            out.printf("%-28s string @%08X%n", c, saddr);
            for (Map.Entry<Long, Long> e : ptrToString.entrySet()) {
                if (!e.getValue().equals(saddr)) continue;
                long p = e.getKey();
                out.printf("    name-ptr @%08X   record dump (-32..+48):%n", p);
                dumpDwords(out, p - 32, 20, p);
                for (Map.Entry<Long, Long> t : ptrToRecord.entrySet()) {
                    long anchor = t.getValue();
                    if (anchor < p - 32 || anchor > p) continue;
                    out.printf("    <- referenced @%08X (points to record+%d)   TypeInfo dump (-16..+48):%n",
                               t.getKey(), (int)(anchor - (p - 32)) - 32);
                    dumpDwords(out, t.getKey() - 16, 16, t.getKey());
                }
            }
            out.println();
        }

        // ---- 5. Infer the TypeInfo linked-list offset ---------------------------
        // Among all TypeInfo candidate locations, find the field offset (relative
        // to the location that holds the data pointer) at which the dword most
        // often equals ANOTHER TypeInfo candidate's location (adjusted by the
        // same offset). That is m_next.
        out.println("## TypeInfo link inference");
        Set<Long> tiLocs = new HashSet<>(ptrToRecord.keySet());
        int bestOff = 0, bestHits = 0;
        for (int off = -32; off <= 64; off += 4) {
            if (off == 0) continue;
            int hits = 0;
            for (long loc : tiLocs) {
                Long v = readDword(loc + off);
                if (v == null) continue;
                // v should be another TypeInfo's data-pointer location minus the
                // same offset, i.e. v + (dataPtrOffsetWithinTypeInfo) in tiLocs.
                // We do not know that inner offset, so test small candidates.
                for (int inner = 0; inner <= 16; inner += 4) if (tiLocs.contains(v + inner)) { hits++; break; }
            }
            out.printf("  offset %+3d from data-ptr slot: %d chain hits%n", off, hits);
            if (hits > bestHits) { bestHits = hits; bestOff = off; }
        }
        out.printf("%nbest link offset %+d with %d hits of %d candidates%n", bestOff, bestHits, tiLocs.size());

        // Head of the list: a candidate nobody links TO.
        if (bestHits > 0) {
            Set<Long> linkedTo = new HashSet<>();
            for (long loc : tiLocs) {
                Long v = readDword(loc + bestOff);
                if (v != null) for (int inner = 0; inner <= 16; inner += 4) if (tiLocs.contains(v + inner)) linkedTo.add(v + inner);
            }
            out.println("candidates not linked to by anyone (list head / or list tails):");
            int shown = 0;
            for (long loc : tiLocs) if (!linkedTo.contains(loc) && shown++ < 12) {
                Long rec = ptrToRecord.get(loc);
                out.printf("  @%08X -> record %08X%n", loc, rec);
            }
            // Statics that point at any TypeInfo candidate: the registry root.
            out.println("static pointers into TypeInfo candidates (registry root candidates):");
            Map<Long, Long> roots = new TreeMap<>();
            scanPointers(tiLocs, roots);
            int n = 0;
            for (Map.Entry<Long, Long> e : roots.entrySet()) {
                if (tiLocs.contains(e.getKey())) continue;   // a TypeInfo pointing at another is the chain, not a root
                if (n++ < 40) out.printf("  @%08X -> %08X%n", e.getKey(), e.getValue());
            }
            out.printf("  (%d total)%n", n);
        }

        // ---- 6. Full string list for the record --------------------------------
        out.println();
        out.println("## All type-name strings (addr name #name-ptrs)");
        for (Map.Entry<Long, String> e : strings.entrySet()) {
            int refs = 0;
            for (long v : ptrToString.values()) if (v == e.getKey()) refs++;
            out.printf("%08X %-40s %d%n", e.getKey(), e.getValue(), refs);
        }
        out.close();
        println("FindFrostbiteTypes: wrote " + new File(outDir, "frostbite_types.txt").getAbsolutePath());
    }

    // Every initialised dword in the image whose value is in `targets`.
    void scanPointers(Set<Long> targets, Map<Long, Long> out) throws Exception {
        for (MemoryBlock b : blocks) {
            byte[] buf = readBlock(b);
            if (buf == null) continue;
            long base = b.getStart().getOffset();
            for (int i = 0; i + 4 <= buf.length; i += 4) {
                long v = (buf[i] & 0xFFL) | ((buf[i+1] & 0xFFL) << 8) | ((buf[i+2] & 0xFFL) << 16) | ((buf[i+3] & 0xFFL) << 24);
                if (targets.contains(v)) out.put(base + i, v);
            }
        }
    }

    byte[] readBlock(MemoryBlock b) {
        try {
            long size = b.getSize();
            if (size > (64L << 20)) return null;
            byte[] buf = new byte[(int) size];
            b.getBytes(b.getStart(), buf);
            return buf;
        } catch (Exception e) { return null; }
    }

    Long readDword(long addr) {
        try {
            Address a = toAddr(addr);
            if (!mem.contains(a)) return null;
            return mem.getInt(a) & 0xFFFFFFFFL;
        } catch (Exception e) { return null; }
    }

    void dumpDwords(PrintWriter out, long start, int count, long mark) {
        for (int i = 0; i < count; i++) {
            long a = start + i * 4L;
            Long v = readDword(a);
            if (i % 4 == 0) out.printf("      %08X:", a);
            if (v == null) out.print(" ????????");
            else out.printf(" %08X%s", v, a == mark ? "*" : " ");
            if (i % 4 == 3) out.println();
        }
        if (count % 4 != 0) out.println();
    }

    static boolean isIdentStart(byte c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
    static boolean isIdentChar(byte c)  { return isIdentStart(c) || (c >= '0' && c <= '9') || c == ':'; }
}
