// Dump 32-bit words from memory with the function each one points to (vtable slots, tables).
// Args: <outFile> <hexAddr> <count> [<hexAddr> <count> ...]
// @category RON
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ReadWords extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter out = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i + 1 < a.length; i += 2) {
				Address p = toAddr(a[i]);
				int n = Integer.parseInt(a[i + 1]);
				out.println("== " + a[i]);
				for (int k = 0; k < n; k++) {
					long v = getInt(p) & 0xffffffffL;
					Function f = getFunctionAt(toAddr(v));
					out.printf("%3d +0x%03x  %08x  %s%n", k, k * 4, v, f != null ? f.getName(true) : "");
					p = p.add(4);
				}
			}
		}
	}
}
