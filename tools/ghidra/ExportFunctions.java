// Export every function as "rva,size,name" so the draw census's call-site RVAs
// (bfbc2vr_draws_*.txt, which log the image base precisely for this) can be
// resolved to the engine's draw-submission functions.
//
// Also lists cross-references to a few addresses the mod already knows about,
// so the code that CONSTRUCTS the FOV object (writes its vtable) can be found
// - that is the path to a fixed pointer to it instead of a per-launch scan.
//
//@category Frostbite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportFunctions extends GhidraScript {

    // From docs/console.md: the camera object's vtable (live vertical FOV at
    // +0x50), measured and poke-verified across three launches.
    static final long[] KNOWN = { 0x014755C8L };

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();

        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter out = new PrintWriter(new File(outDir, "functions.csv"), StandardCharsets.UTF_8)) {
            out.printf("# image base %08X ; rva,size,name%n", base);
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            int n = 0;
            while (it.hasNext()) {
                Function f = it.next();
                long rva = f.getEntryPoint().getOffset() - base;
                long size = f.getBody().getNumAddresses();
                out.printf("%08X,%d,%s%n", rva, size, f.getName());
                n++;
            }
            println("ExportFunctions: " + n + " functions");
        }

        try (PrintWriter out = new PrintWriter(new File(outDir, "known_xrefs.txt"), StandardCharsets.UTF_8)) {
            for (long k : KNOWN) {
                Address a = toAddr(k);
                out.printf("## xrefs to %08X%n", k);
                int n = 0;
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.printf("  %s  %-10s in %s%n", r.getFromAddress(), r.getReferenceType(),
                               f != null ? f.getName() + " (rva " + Long.toHexString(f.getEntryPoint().getOffset() - base) + ")" : "-");
                    n++;
                }
                out.printf("  (%d)%n%n", n);
            }
        }
    }
}
