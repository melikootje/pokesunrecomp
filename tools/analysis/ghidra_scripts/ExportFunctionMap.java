// Exports a derived function map (address, size, ARM/Thumb mode, thunk flag) for the open program.
// Output contains only addresses/sizes/flags -- never bytes -- per docs/legal-policy.md.
// Usage: analyzeHeadless <proj> <name> -import code.elf -postScript ExportFunctionMap.java <out.tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.PrintWriter;

public class ExportFunctionMap extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("expected output path");
        }
        Register tmode = currentProgram.getLanguage().getRegister("TMode");
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            out.println("address\tsize\tmode\tthunk");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                Address a = f.getEntryPoint();
                RegisterValue rv = tmode == null ? null : currentProgram.getProgramContext().getRegisterValue(tmode, a);
                boolean thumb = rv != null && rv.hasValue() && rv.getUnsignedValue().intValue() == 1;
                out.printf("0x%x\t%d\t%s\t%d%n", a.getOffset(), f.getBody().getNumAddresses(),
                        thumb ? "thumb" : "arm", f.isThunk() ? 1 : 0);
            }
        }
    }
}
