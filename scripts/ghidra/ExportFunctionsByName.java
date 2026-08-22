// Export decompiler output for functions by exact name from the current program.
// Run example:
//   analyzeHeadless /tmp ghidra-audio -import capture_audio_8ch -analyze \
//     -scriptPath scripts/ghidra \
//     -postScript ExportFunctionsByName.java /tmp/mz0380-ghidra-audio main

import java.io.File;
import java.io.FileWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class ExportFunctionsByName extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			throw new IllegalArgumentException(
				"usage: ExportFunctionsByName.java OUTDIR function [...]");
		}

		File outDir = new File(args[0]);
		if (!outDir.exists() && !outDir.mkdirs()) {
			throw new RuntimeException("failed to create " + outDir);
		}

		DecompInterface iface = new DecompInterface();
		iface.openProgram(currentProgram);
		SymbolTable symbols = currentProgram.getSymbolTable();

		File summary = new File(outDir, currentProgram.getName() + "_by_name_summary.txt");
		FileWriter sw = new FileWriter(summary);
		sw.write("program\tname\tentry\tstatus\n");

		for (int i = 1; i < args.length; i++) {
			String name = args[i];
			Function fn = null;
			SymbolIterator it = symbols.getSymbols(name);
			while (it.hasNext()) {
				Symbol sym = it.next();
				fn = getFunctionAt(sym.getAddress());
				if (fn == null) {
					fn = getFunctionContaining(sym.getAddress());
				}
				if (fn != null) {
					break;
				}
			}

			if (fn == null) {
				sw.write(currentProgram.getName() + "\t" + name + "\t-\tMISSING\n");
				println("missing " + name);
				continue;
			}

			DecompileResults res = iface.decompileFunction(fn, 120, monitor);
			File out = new File(outDir, currentProgram.getName() + "_" + name + ".c");
			FileWriter fw = new FileWriter(out);
			fw.write("// Program: " + currentProgram.getName() + "\n");
			fw.write("// Function: " + fn.getName() + "\n");
			fw.write("// Entry: " + fn.getEntryPoint().toString() + "\n\n");
			if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
				fw.write(res.getDecompiledFunction().getC());
				sw.write(currentProgram.getName() + "\t" + name + "\t" +
					fn.getEntryPoint() + "\tFOUND\n");
			} else {
				fw.write("// Decompile failed: " + res.getErrorMessage() + "\n");
				sw.write(currentProgram.getName() + "\t" + name + "\t" +
					fn.getEntryPoint() + "\tDECOMPILE_FAILED\n");
			}
			fw.close();
			println("exported " + name + " -> " + out);
		}

		sw.close();
		println("summary -> " + summary);
	}
}
