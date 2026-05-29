// Ghidra headless post-script: export a GhidraBoy-loaded Game Boy program as
// three JSON files — functions, symbols, and decompiled C — for feeding better
// names back into the gb-recompiled code generator.
//
// Run via analyzeHeadless (see run_ghidra.py). Script args:
//   args[0] = output directory (default ".")
//   args[1] = max functions to decompile, 0 = all (default 0; decompiling
//             every function is slow, so pass a small number for a quick run)
//
// Bank mapping: GhidraBoy loads ROM bank 0 into block "rom"/"rom0" at
// 0x0000-0x3FFF and each higher bank into an overlay block "rom<N>" based at
// 0x4000. The block name therefore encodes the ROM bank, which is exactly the
// (bank, addr) form gb-recompiled names functions by.
//
//@category GameBoy
//@keybinding
//@menupath
//@toolbar

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;

public class ExportGbJson extends GhidraScript {

    private static String esc(String s) {
        if (s == null) return "";
        StringBuilder b = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
                    else b.append(c);
            }
        }
        return b.toString();
    }

    // -1 means the address is not in a ROM block (hardware regs, WRAM, etc.).
    private int bankOf(Address a) {
        MemoryBlock blk = currentProgram.getMemory().getBlock(a);
        if (blk == null) return -1;
        String n = blk.getName();
        if (n.equals("rom") || n.equals("rom0")) return 0;
        if (n.startsWith("rom")) {
            try { return Integer.parseInt(n.substring(3)); } catch (NumberFormatException e) { return -1; }
        }
        return -1;
    }

    private static int gbAddr(Address a) {
        return (int) (a.getOffset() & 0xFFFF);
    }

    // Ghidra's placeholder names we don't want to propagate as "real" names.
    private static boolean isAutoName(String name, SourceType src) {
        if (src == SourceType.DEFAULT) return true;
        if (name == null) return true;
        return name.startsWith("FUN_") || name.startsWith("SUB_") ||
               name.startsWith("LAB_") || name.startsWith("DAT_") ||
               name.startsWith("UNK_") || name.startsWith("EXT_") ||
               name.startsWith("caseD_") || name.startsWith("switchD_");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length > 0 && !args[0].isEmpty()) ? args[0] : ".";
        int maxDecomp = 0;
        if (args.length > 1) {
            try { maxDecomp = Integer.parseInt(args[1]); } catch (NumberFormatException ignored) {}
        }
        File dir = new File(outDir);
        dir.mkdirs();

        println("[export] program: " + currentProgram.getName());
        println("[export] output dir: " + dir.getAbsolutePath());

        // ---- functions.json ----
        int funcCount = 0;
        try (BufferedWriter w = newWriter(new File(dir, "functions.json"))) {
            w.write("[\n");
            boolean first = true;
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            for (Function f : it) {
                Address ep = f.getEntryPoint();
                int bank = bankOf(ep);
                if (bank < 0) continue; // only ROM-resident functions
                Symbol sym = f.getSymbol();
                SourceType src = sym != null ? sym.getSource() : SourceType.DEFAULT;
                String name = f.getName();
                if (!first) w.write(",\n");
                first = false;
                w.write(String.format(
                    "  {\"bank\": %d, \"addr\": \"0x%04x\", \"name\": \"%s\", \"auto\": %s, \"source\": \"%s\"}",
                    bank, gbAddr(ep), esc(name), isAutoName(name, src) ? "true" : "false",
                    src != null ? src.toString() : "DEFAULT"));
                funcCount++;
            }
            w.write("\n]\n");
        }
        println("[export] functions: " + funcCount);

        // ---- symbols.json ----
        int symCount = 0;
        try (BufferedWriter w = newWriter(new File(dir, "symbols.json"))) {
            w.write("[\n");
            boolean first = true;
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(false); // non-dynamic
            for (Symbol s : it) {
                Address a = s.getAddress();
                if (a == null || !a.isMemoryAddress()) continue;
                int bank = bankOf(a);
                if (bank < 0) continue;
                String name = s.getName();
                SourceType src = s.getSource();
                if (isAutoName(name, src)) continue; // keep only meaningful symbols
                if (!first) w.write(",\n");
                first = false;
                w.write(String.format(
                    "  {\"bank\": %d, \"addr\": \"0x%04x\", \"name\": \"%s\", \"type\": \"%s\", \"source\": \"%s\"}",
                    bank, gbAddr(a), esc(name),
                    s.getSymbolType() != null ? s.getSymbolType().toString() : "",
                    src != null ? src.toString() : "DEFAULT"));
                symCount++;
            }
            w.write("\n]\n");
        }
        println("[export] symbols: " + symCount);

        // ---- decompiled.json ----
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        int decompCount = 0;
        try (BufferedWriter w = newWriter(new File(dir, "decompiled.json"))) {
            w.write("[\n");
            boolean first = true;
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            for (Function f : it) {
                if (monitor.isCancelled()) break;
                if (maxDecomp > 0 && decompCount >= maxDecomp) break;
                Address ep = f.getEntryPoint();
                int bank = bankOf(ep);
                if (bank < 0) continue;
                String c = "";
                try {
                    DecompileResults res = di.decompileFunction(f, 30, monitor);
                    if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                        c = res.getDecompiledFunction().getC();
                    }
                } catch (Exception e) {
                    c = "/* decompile failed: " + e.getMessage() + " */";
                }
                if (!first) w.write(",\n");
                first = false;
                w.write(String.format(
                    "  {\"bank\": %d, \"addr\": \"0x%04x\", \"name\": \"%s\", \"c\": \"%s\"}",
                    bank, gbAddr(ep), esc(f.getName()), esc(c)));
                decompCount++;
            }
            w.write("\n]\n");
        } finally {
            di.dispose();
        }
        println("[export] decompiled: " + decompCount + (maxDecomp > 0 ? " (capped at " + maxDecomp + ")" : ""));
        println("[export] done.");
    }

    private static BufferedWriter newWriter(File f) throws Exception {
        return new BufferedWriter(new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8));
    }
}
