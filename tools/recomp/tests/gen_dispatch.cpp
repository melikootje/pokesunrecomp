// Discovers blocks in the synthetic programs and emits C for them: gen_dispatch <out.c>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "dispatch_programs.h"
#include "recomp/blocks.h"

struct MemImage : recomp::ImageReader {
    prog::Image img;
    bool read(uint32_t a, void *d, size_t n) const override {
        if (uint64_t(a) + n > img.bytes.size())
            return false;
        std::memcpy(d, &img.bytes[a], n);
        return true;
    }
};

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    MemImage m;
    m.img = prog::build();
    using recomp::Entry;
    // kVcall's indirect callees are *not* listed for the loop/table programs; the vtable callees are listed
    // here to model "address taken" entries recovered from vtables.
    const std::vector<Entry> entries = {{prog::kLoop, false},   {prog::kTable, false}, {prog::kVcall, false},
                                        {prog::kThumbFn, true}, {prog::kArmFn, false}, {prog::kSvc, false}};
    recomp::DiscoverStats st;
    const auto blocks = recomp::discoverBlocks(m, entries, {}, &st);
    std::ofstream out(argv[1]);
    out << recomp::emitBlocksC(blocks);
    std::fprintf(stderr, "blocks=%zu insns=%zu jumpTables=%zu targets=%zu indirect=%zu\n", st.blocks,
                 st.insns, st.jumpTables, st.jumpTableTargets, st.unresolvedIndirect);
    return 0;
}
