// Decompile a hand-picked set of functions from the decrypted image and write
// them to one file: the camera object's constructor/destructor and their
// callers, its vtable's virtuals, the base TypeInfo constructor, and whatever
// references the "Render.*" / "Game.*" global-variable name strings.
//
//@category Frostbite
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class DecompileTargets extends GhidraScript {

    static final long CAMERA_VTABLE = 0x014755C8L;
    static final long[] FUNCS = { 0x00c42130L, 0x00c43970L, 0x00c439c0L,   // write the camera vtable
                                  0x004fe260L, 0x004293f0L,                // base TypeInfo ctor + register
                                  0x004f4c40L, 0x004f4600L };              // GetGlobalVariable("Module.Field", TypeInfo*) and its worker
    static final String[] STRINGS = { "Render.Renderer", "Render.EdgeModelLodScale", "Game.AutoAimEnabled",
                                      "ForceFov", "GameRenderSettings", "DefaultFOV", "InfantryFOVMultiplier" };

    DecompInterface dec;
    PrintWriter out;
    Set<Long> done = new HashSet<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();
        out = new PrintWriter(new File(outDir, "decompiled.txt"), StandardCharsets.UTF_8);
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        out.println("# ==== functions that write the camera vtable, and their callers ====");
        for (long f : FUNCS) {
            decomp(f, 400);
            Function fn = getFunctionAt(toAddr(f));
            if (fn == null) continue;
            int n = 0;
            for (Reference r : getReferencesTo(fn.getEntryPoint())) {
                Function caller = getFunctionContaining(r.getFromAddress());
                if (caller == null || n++ >= 6) continue;
                out.printf("#   caller of %08X: %s @%s%n", f, caller.getName(), caller.getEntryPoint());
                decomp(caller.getEntryPoint().getOffset(), 250);
            }
        }

        out.println("\n# ==== camera vtable virtuals ====");
        Address vt = toAddr(CAMERA_VTABLE);
        for (int i = 0; i < 24; i++) {
            long fp = currentProgram.getMemory().getInt(vt.add(4L * i)) & 0xFFFFFFFFL;
            if (fp < 0x00401000L || fp > 0x01400000L) break;
            out.printf("# vslot %d -> %08X%n", i, fp);
            decomp(fp, 120);
        }

        out.println("\n# ==== functions referencing setting-name strings ====");
        for (String s : STRINGS) {
            Address a = findStr(s);
            if (a == null) { out.printf("# \"%s\": not found%n", s); continue; }
            out.printf("# \"%s\" @%s%n", s, a);
            int n = 0;
            for (Reference r : getReferencesTo(a)) {
                Function fn = getFunctionContaining(r.getFromAddress());
                out.printf("#   ref from %s in %s%n", r.getFromAddress(), fn != null ? fn.getName() : "-");
                if (fn != null && n++ < 3) decomp(fn.getEntryPoint().getOffset(), 250);
            }
        }
        out.close();
        dec.dispose();
        println("DecompileTargets: done");
    }

    Address findStr(String s) throws Exception {
        byte[] needle = (s + "\0").getBytes(StandardCharsets.US_ASCII);
        return currentProgram.getMemory().findBytes(currentProgram.getMinAddress(), needle, null, true, monitor);
    }

    void decomp(long addr, int maxLines) {
        if (!done.add(addr)) { out.printf("## %08X (already shown)%n", addr); return; }
        Address a = toAddr(addr);
        Function f = getFunctionAt(a);
        if (f == null) f = getFunctionContaining(a);
        if (f == null) {
            try { f = createFunction(a, "FUN_" + Long.toHexString(addr)); } catch (Exception e) { }
        }
        out.printf("## %08X %s%n", addr, f != null ? f.getName() : "(no function)");
        if (f == null) return;
        DecompileResults res = dec.decompileFunction(f, 60, monitor);
        if (res == null || !res.decompileCompleted()) { out.println("  (decompile failed)"); return; }
        String[] lines = res.getDecompiledFunction().getC().split("\n");
        for (int i = 0; i < lines.length && i < maxLines; i++) out.println(lines[i]);
        if (lines.length > maxLines) out.printf("  ... (%d more lines)%n", lines.length - maxLines);
        out.println();
    }
}
