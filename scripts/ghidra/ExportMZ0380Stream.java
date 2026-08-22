// Export selected MZ0380 decompiler output from an existing Ghidra project.
// Run with:
//   analyzeHeadless /home/wozt/ghidra-projects MZ0380 -process LXV4L2D_MZ0380.ko \
//     -noanalysis -scriptPath scripts/ghidra -postScript ExportMZ0380Stream.java /tmp/mz0380-ghidra-stream

import java.io.File;
import java.io.FileWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class ExportMZ0380Stream extends GhidraScript {
	private static final String[] TARGETS = {
		"MZ0380_StartFirmware",
		"MZ0380_StopFirmware",
		"MZ0380_SendVendorCommand_P5",
		"MZ0380_SendVendorCommand",
		"MZ0380_WaitInterruptComplete",
		"vid_cap_start_streaming",
		"vid_cap_stop_streaming",
	};

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : "/tmp/mz0380-ghidra-stream");
		if (!outDir.exists() && !outDir.mkdirs()) {
			throw new RuntimeException("failed to create " + outDir);
		}

		DecompInterface iface = new DecompInterface();
		iface.openProgram(currentProgram);

		SymbolTable symbols = currentProgram.getSymbolTable();
		Set<String> found = new HashSet<String>();

		for (String name : TARGETS) {
			SymbolIterator it = symbols.getSymbols(name);
			while (it.hasNext()) {
				Symbol sym = it.next();
				Function fn = getFunctionAt(sym.getAddress());
				if (fn == null) {
					fn = getFunctionContaining(sym.getAddress());
				}
				if (fn == null) {
					continue;
				}

				DecompileResults res = iface.decompileFunction(fn, 120, monitor);
				File out = new File(outDir, name + ".c");
				FileWriter w = new FileWriter(out);
				w.write("// Function: " + name + "\n");
				w.write("// Entry: " + fn.getEntryPoint().toString() + "\n\n");
				if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
					w.write(res.getDecompiledFunction().getC());
				} else {
					w.write("// Decompile failed: " + res.getErrorMessage() + "\n");
				}
				w.close();
				println("exported " + name + " -> " + out);
				found.add(name);
				break;
			}
		}

		File summary = new File(outDir, "_summary.txt");
		FileWriter w = new FileWriter(summary);
		for (String name : TARGETS) {
			w.write(name + "\t" + (found.contains(name) ? "FOUND" : "MISSING") + "\n");
		}
		w.close();
		println("summary -> " + summary);
	}
}
