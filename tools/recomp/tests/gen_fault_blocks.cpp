// Emits blocks that use the default guest-memory accessors (host base + guest address): gen_fault_blocks
// <out.c>
#include <fstream>

#include "recomp/decode.h"
#include "recomp/emit_c.h"

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    using namespace recomp;
    std::ofstream out(argv[1]);
    out << emitPrelude();
    // r0 = *(u32*)(r1 + 4); r0 += 1; *(u32*)(r2) = r0;   (ldr r0,[r1,#4] / add r0,r0,#1 / str r0,[r2])
    const uint32_t words[] = {0xE5910004, 0xE2800001, 0xE5820000};
    std::vector<Insn> insns;
    for (int k = 0; k < 3; k++)
        insns.push_back(decodeArm(words[k], 0x00100000 + 4 * uint32_t(k)));
    out << emitBlockC(insns, "blk_loadadd_store");
    return 0;
}
