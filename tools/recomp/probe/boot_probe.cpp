// boot_probe: runs a locally generated recompilation of a user-supplied code image from its entry point on
// the HLE kernel, logging SVCs, IPC, faults and dispatcher coverage. Local analysis only; the generated code
// it links is derived from game data and must not be committed.
//
//   boot_probe <code.bin> [maxInsns] [lenient]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/kernel.h"

extern "C" {
typedef struct {
    void (*fn)(RcCtx *);
    uint32_t addr, bytes, insns, thumb;
} rc_block_t;
extern const rc_block_t rc_blocks[];
extern const unsigned rc_block_count;
}

using namespace pokerecomp;

namespace {
constexpr uint32_t kText = 0x00100000, kImageEnd = 0x006D5000;
constexpr uint32_t kStackTop = 0x10000000,
                   kStackSize = 0x100000; // main thread stack (3DS: top of the app stack region)
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: boot_probe <code.bin> [maxInsns] [lenient]\n");
        return 2;
    }
    const uint64_t maxInsns = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000000000ull;
    const bool lenient = argc > 3 && std::string(argv[3]) == "lenient";
    std::vector<uint8_t> code;
    {
        std::ifstream f(argv[1], std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(f), {});
    }
    GuestMemory gm;
    if (!gm.ok())
        return 1;
    const int rw = GuestMemory::Read | GuestMemory::Write;
    const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
    gm.map(kText, (kImageEnd - kText + pg - 1) & ~(pg - 1), rw);
    gm.map(kStackTop - kStackSize, kStackSize, rw);
    std::memcpy(gm.base() + kText, code.data(), code.size());

    Dispatcher disp;
    for (unsigned k = 0; k < rc_block_count; k++) {
        BlockInfo b;
        b.fn = rc_blocks[k].fn;
        b.addr = rc_blocks[k].addr;
        b.bytes = rc_blocks[k].bytes;
        b.insns = rc_blocks[k].insns;
        b.thumb = rc_blocks[k].thumb != 0;
        disp.registerBlock(b);
    }
    Kernel kernel(gm, disp);
    kernel.setUnimplementedPolicy(lenient ? UnimplementedPolicy::ReturnSuccess
                                          : UnimplementedPolicy::ReturnError);
    kernel.setLog([](const std::string &s) { std::printf("[kernel] %s\n", s.c_str()); });
    kernel.configure(KernelConfig{});
    kernel.createMainThread(kText, kStackTop, 0);
    std::printf("registered %zu blocks; policy for unimplemented IPC: %s\n", disp.blockCount(),
                lenient ? "success" : "error");

    const RunResult rr = kernel.run(maxInsns);
    static const char *names[] = {
        "all threads exited", "deadlock", "instruction limit", "process exit", "break", "trap", "fault"};
    std::printf("result: %s\n", names[int(rr)]);
    if (rr == RunResult::Fault)
        std::printf("  guest fault at %08x\n", kernel.lastFault().address);
    if (rr == RunResult::Break)
        std::printf("  svcBreak reason %u\n", kernel.breakReason());

    for (uint32_t id = 1; id <= 4; id++)
        if (const RcCtx *c = kernel.threadContext(id))
            std::printf("thread %u: pc=%08x lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", id, c->r[15],
                        c->r[14], c->r[13], c->r[0], c->r[1], c->r[2], c->r[3]);
    // trace summary: SVC histogram and the sequence of IPC requests
    std::map<std::string, unsigned> svcHist;
    std::vector<std::string> ipc;
    for (const auto &t : kernel.trace()) {
        if (t.rfind("svc ", 0) == 0)
            svcHist[t.substr(4, 4)]++;
        else if (t.rfind("ipc ", 0) == 0 || t.rfind("srv ", 0) == 0)
            ipc.push_back(t);
    }
    std::printf("svcs=%" PRIu64 " ipc=%" PRIu64 " thread switches=%" PRIu64 " virtual ticks=%" PRIu64 "\n",
                kernel.stats().svcs, kernel.stats().ipcRequests, kernel.stats().switches, kernel.ticks());
    for (auto &kv : svcHist)
        std::printf("  svc %s x%u\n", kv.first.c_str(), kv.second);
    std::printf("first IPC requests:\n");
    for (size_t k = 0; k < ipc.size() && k < 40; k++)
        std::printf("  %s\n", ipc[k].c_str());
    const auto un = kernel.unimplementedCommands();
    std::printf("unimplemented service commands: %zu\n", un.size());
    for (auto &u : un)
        std::printf("  %s %08x\n", u.first.c_str(), u.second);
    // Fallback run starts (pcs whose predecessor was not also interpreted) become extra block entries on the
    // next translate run: the "log hits to improve coverage" loop.
    {
        const auto &hits = disp.fallbackHits();
        std::ofstream o("fallback_entries.txt");
        size_t n = 0;
        for (const auto &kv : hits) {
            const uint32_t pc = kv.first & ~1u;
            const bool thumb = kv.first & 1;
            const bool prev =
                hits.count((pc - 4) | (thumb ? 1u : 0u)) || (thumb && hits.count((pc - 2) | 1u));
            if (!prev) {
                o << std::hex << pc << " " << (thumb ? 1 : 0) << "\n";
                n++;
            }
        }
        std::printf("wrote %zu fallback entry points to fallback_entries.txt\n", n);
    }
    const Coverage &cv = disp.coverage();
    std::printf("coverage: %" PRIu64 " recompiled, %" PRIu64 " interpreted insns (%.2f%%), %" PRIu64
                " blocks run\n",
                cv.recompiledInsns, cv.interpretedInsns, cv.percentRecompiled(), cv.blocksRun);
    for (const auto &t : disp.topFallbacks(10))
        std::printf("  fallback pc %08x%s x%" PRIu64 "\n", t.first & ~1u, (t.first & 1) ? " (thumb)" : "",
                    t.second);
    return 0;
}
