// Emits one C function pair per reference vector: gen_blocks <vectors> <out.c>
#include <fstream>

#include "recomp/decode.h"
#include "recomp/emit_c.h"
#include "vectors.h"

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    const auto vs = vec::load(argv[1]);
    std::ofstream out(argv[2]);
    out << "#include \"harness_mem.h\"\n" << recomp::emitPrelude();
    for (size_t k = 0; k < vs.size(); k++) {
        const std::vector<recomp::Insn> insns = vec::decodeVector(vs[k]);
        recomp::EmitOptions full;
        full.eliminateDeadFlags = false;
        recomp::EmitOptions dead;
        dead.liveOut = 0;
        out << recomp::emitBlockC(insns, "v" + std::to_string(k) + "_full", full);
        out << recomp::emitBlockC(insns, "v" + std::to_string(k) + "_dead", dead);
    }
    out << "typedef void (*rc_fn)(RcCtx*);\nconst rc_fn rc_vec_fns[][2] = {\n";
    for (size_t k = 0; k < vs.size(); k++)
        out << "    {v" << k << "_full, v" << k << "_dead},\n";
    out << "    {0, 0}};\nconst unsigned rc_vec_count = " << vs.size() << ";\n";
    return 0;
}
