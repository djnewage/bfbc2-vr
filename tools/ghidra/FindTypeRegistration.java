// Harvest every Frostbite type registration and decompile the constructor.
//
// FindFrostbiteTypes showed that each TypeInfoData record is referenced from
// code as a static-initialiser sequence:
//
//     push  <TypeInfoData>       68 imm32
//     mov   ecx, <TypeInfo obj>  B9 imm32       ; the object, zero-filled in .data until startup
//     call  <TypeInfo::TypeInfo> E8 rel32
//
// The objects are only populated at runtime, so the registry's linked list
// cannot be read from the file. The constructor CAN: it is the code that does
// `this->m_next = s_firstTypeInfo; s_firstTypeInfo = this;`. This script
// collects every (record, object, ctor) triple, groups by ctor, decodes the
// records' fields, and decompiles the constructor(s) so the link offset and
// the static head can be read off C rather than guessed.
//
//@category Frostbite
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.app.script.GhidraScript;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class FindTypeRegistration extends GhidraScript {

    Memory mem;
    List<MemoryBlock> blocks = new ArrayList<>();
    Map<MemoryBlock, byte[]> bytesOf = new HashMap<>();

    static class Reg {
        long record, object, ctor, site;
        String name; long sizeFlags, module, misc, extra; boolean hasExtra;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();
        PrintWriter out = new PrintWriter(new File(outDir, "type_registration.txt"), StandardCharsets.UTF_8);

        mem = currentProgram.getMemory();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || !b.isLoaded()) continue;
            blocks.add(b);
            bytesOf.put(b, readBlock(b));
        }

        // 1. Type-name strings -> record addresses (record begins with the name pointer).
        Map<Long, String> strings = new HashMap<>();
        for (MemoryBlock b : blocks) {
            byte[] buf = bytesOf.get(b); if (buf == null) continue;
            long base = b.getStart().getOffset();
            int i = 0;
            while (i < buf.length) {
                if (!isIdentStart(buf[i])) { i++; continue; }
                int j = i; while (j < buf.length && isIdentChar(buf[j])) j++;
                if (j < buf.length && buf[j] == 0 && j - i >= 3 && j - i <= 64) {
                    String s = new String(buf, i, j - i, StandardCharsets.US_ASCII);
                    if (Character.isUpperCase(s.charAt(0))) strings.put(base + i, s);
                }
                i = j + 1;
            }
        }
        // Records: any dword in a non-executable block that points at one of those strings
        // AND whose following dword looks like size<<16|flags with a small flags byte.
        Map<Long, String> records = new TreeMap<>();
        for (MemoryBlock b : blocks) {
            if (b.isExecute()) continue;
            byte[] buf = bytesOf.get(b); if (buf == null) continue;
            long base = b.getStart().getOffset();
            for (int i = 0; i + 8 <= buf.length; i += 4) {
                long v = dword(buf, i);
                String name = strings.get(v);
                if (name == null) continue;
                long sf = dword(buf, i + 4);
                if ((sf & 0xFFFF) > 0xFF) continue;           // flags byte small
                records.put(base + i, name);
            }
        }
        out.printf("# %d TypeInfoData record candidates (name ptr + plausible size/flags)%n", records.size());

        // 2. Code: push imm32 to a record, then mov ecx, imm32 and call rel32 nearby.
        List<Reg> regs = new ArrayList<>();
        for (MemoryBlock b : blocks) {
            if (!b.isExecute()) continue;
            byte[] buf = bytesOf.get(b); if (buf == null) continue;
            long base = b.getStart().getOffset();
            for (int i = 0; i + 5 <= buf.length; i++) {
                if ((buf[i] & 0xFF) != 0x68) continue;
                long imm = dword(buf, i + 1);
                String name = records.get(imm);
                if (name == null) continue;
                Reg r = new Reg();
                r.site = base + i; r.record = imm; r.name = name;
                // mov ecx, imm32 within the next 12 bytes or previous 8
                for (int k = -8; k <= 12; k++) {
                    int p = i + k;
                    if (p < 0 || p + 5 > buf.length || k == 0) continue;
                    if ((buf[p] & 0xFF) == 0xB9) { r.object = dword(buf, p + 1); break; }
                }
                // call rel32 within the next 20 bytes
                for (int k = 5; k <= 20; k++) {
                    int p = i + k;
                    if (p + 5 > buf.length) continue;
                    if ((buf[p] & 0xFF) == 0xE8) {
                        long rel = (int) dword(buf, p + 1);
                        r.ctor = base + p + 5 + rel;
                        break;
                    }
                }
                decodeRecord(r);
                regs.add(r);
            }
        }
        out.printf("# %d registration sites found in code%n%n", regs.size());

        // 3. Group by constructor.
        Map<Long, Integer> byCtor = new TreeMap<>();
        for (Reg r : regs) byCtor.merge(r.ctor, 1, Integer::sum);
        out.println("## Constructors called (address: registrations)");
        for (Map.Entry<Long, Integer> e : byCtor.entrySet()) out.printf("  %08X: %d%n", e.getKey(), e.getValue());
        out.println();

        // 4. Record-layout evidence: flags byte -> does an extra pointer follow the 24 bytes?
        out.println("## Record layout by flags byte (count, of which followed by a pointer into .data)");
        Map<Long, int[]> byFlags = new TreeMap<>();
        for (Reg r : regs) {
            int[] c = byFlags.computeIfAbsent(r.sizeFlags & 0xFF, k -> new int[2]);
            c[0]++; if (r.hasExtra) c[1]++;
        }
        for (Map.Entry<Long, int[]> e : byFlags.entrySet())
            out.printf("  flags 0x%02X: %d records, %d with trailing pointer%n", e.getKey(), e.getValue()[0], e.getValue()[1]);
        out.println();

        // 5. Every registration, decoded.
        out.println("## Registrations (site  record  object  ctor  size  flags  module  misc  extra  name)");
        regs.sort((a, b) -> a.name.compareTo(b.name));
        for (Reg r : regs) {
            out.printf("%08X  %08X  %08X  %08X  %5d  0x%02X  %08X  %08X  %s  %s%n",
                       r.site, r.record, r.object, r.ctor, (r.sizeFlags >> 16) & 0xFFFF, r.sizeFlags & 0xFF,
                       r.module, r.misc, r.hasExtra ? String.format("%08X", r.extra) : "   -    ", r.name);
        }
        out.println();

        // 6. Decompile each constructor (at most 4).
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        // Rank by registration count; a misdecoded rel32 lands outside the image
        // and shows up as a one-off, so require the target to be mapped.
        List<Map.Entry<Long, Integer>> ranked = new ArrayList<>(byCtor.entrySet());
        ranked.sort((x, y) -> y.getValue() - x.getValue());
        int shown = 0;
        for (Map.Entry<Long, Integer> e : ranked) {
            long ctor = e.getKey();
            if (ctor == 0 || !mem.contains(toAddr(ctor)) || shown++ >= 4) continue;
            Address a = toAddr(ctor);
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, "TypeInfo_ctor_" + Long.toHexString(ctor));
            out.printf("## Decompiled constructor %08X (%s), %d registrations%n", ctor, f != null ? f.getName() : "?", byCtor.get(ctor));
            if (f == null) { out.println("  (could not create function)"); continue; }
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
            else out.println("  (decompile failed: " + (res != null ? res.getErrorMessage() : "null") + ")");
            out.println();
        }
        dec.dispose();
        out.close();
        println("FindTypeRegistration: " + regs.size() + " registrations, " + byCtor.size() + " ctors");
    }

    void decodeRecord(Reg r) {
        r.sizeFlags = readDword(r.record + 4);
        r.module    = readDword(r.record + 8);
        r.misc      = readDword(r.record + 12);
        long after  = readDword(r.record + 24);
        // A trailing pointer lands in a data block and is not itself a string
        // pointer (the next record's name) - i.e. it points into .data, not .rdata.
        r.hasExtra = false;
        if (after != 0) {
            Address a = toAddr(after);
            MemoryBlock b = mem.getBlock(a);
            if (b != null && b.isWrite() && !b.isExecute()) { r.hasExtra = true; r.extra = after; }
        }
    }

    long readDword(long addr) {
        try { Address a = toAddr(addr); return mem.contains(a) ? (mem.getInt(a) & 0xFFFFFFFFL) : 0; }
        catch (Exception e) { return 0; }
    }
    static long dword(byte[] b, int i) {
        return (b[i] & 0xFFL) | ((b[i+1] & 0xFFL) << 8) | ((b[i+2] & 0xFFL) << 16) | ((b[i+3] & 0xFFL) << 24);
    }
    byte[] readBlock(MemoryBlock b) {
        try { if (b.getSize() > (64L << 20)) return null; byte[] buf = new byte[(int) b.getSize()]; b.getBytes(b.getStart(), buf); return buf; }
        catch (Exception e) { return null; }
    }
    static boolean isIdentStart(byte c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
    static boolean isIdentChar(byte c)  { return isIdentStart(c) || (c >= '0' && c <= '9') || c == ':'; }
}
