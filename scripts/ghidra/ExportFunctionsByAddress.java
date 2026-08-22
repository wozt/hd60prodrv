// Export decompiler output for functions containing absolute addresses.
// Run example:
//   analyzeHeadless /home/wozt ElgatoHD60pro -process e60MZ0380.X64.SYS \
//     -noanalysis -scriptPath scripts/ghidra \
//     -postScript ExportFunctionsByAddress.java /tmp/hd60pro-ghidra-windows \
//       send_command=140285074 preinit=140278bb0

import java.io.File;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ExportFunctionsByAddress extends GhidraScript {
	private static class Target {
		final String label;
		final String addressText;

		Target(String label, String addressText) {
			this.label = label;
			this.addressText = addressText;
		}
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			throw new IllegalArgumentException(
				"usage: ExportFunctionsByAddress.java OUTDIR label=address [...]");
		}

		File outDir = new File(args[0]);
		if (!outDir.exists() && !outDir.mkdirs()) {
			throw new RuntimeException("failed to create " + outDir);
		}

		List<Target> targets = new ArrayList<Target>();
		for (int i = 1; i < args.length; i++) {
			String arg = args[i];
			int eq = arg.indexOf('=');
			if (eq <= 0 || eq == arg.length() - 1) {
				throw new IllegalArgumentException("bad target: " + arg);
			}
			targets.add(new Target(arg.substring(0, eq), arg.substring(eq + 1)));
		}

		DecompInterface iface = new DecompInterface();
		iface.openProgram(currentProgram);

		File summary = new File(outDir, currentProgram.getName() + "_summary.txt");
		FileWriter sw = new FileWriter(summary);
		sw.write("program\tlabel\taddress\tfunction_entry\tfunction_name\tstatus\n");

		for (Target target : targets) {
			Address addr = currentProgram.getAddressFactory()
				.getDefaultAddressSpace()
				.getAddress(target.addressText);
			Function fn = getFunctionContaining(addr);
			if (fn == null) {
				sw.write(currentProgram.getName() + "\t" + target.label + "\t" +
					target.addressText + "\t-\t-\tMISSING\n");
				println("missing " + target.label + " at " + target.addressText);
				continue;
			}

			DecompileResults res = iface.decompileFunction(fn, 120, monitor);
			File out = new File(outDir, currentProgram.getName() + "_" + target.label + ".c");
			FileWriter fw = new FileWriter(out);
			fw.write("// Program: " + currentProgram.getName() + "\n");
			fw.write("// Label: " + target.label + "\n");
			fw.write("// Requested address: " + target.addressText + "\n");
			fw.write("// Function: " + fn.getName() + "\n");
			fw.write("// Entry: " + fn.getEntryPoint().toString() + "\n\n");
			if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
				fw.write(res.getDecompiledFunction().getC());
				sw.write(currentProgram.getName() + "\t" + target.label + "\t" +
					target.addressText + "\t" + fn.getEntryPoint() + "\t" +
					fn.getName() + "\tFOUND\n");
			} else {
				fw.write("// Decompile failed: " + res.getErrorMessage() + "\n");
				sw.write(currentProgram.getName() + "\t" + target.label + "\t" +
					target.addressText + "\t" + fn.getEntryPoint() + "\t" +
					fn.getName() + "\tDECOMPILE_FAILED\n");
			}
			fw.close();
			println("exported " + target.label + " -> " + out);
		}

		sw.close();
		println("summary -> " + summary);
	}
}
