// boot_probe: runs a locally generated recompilation of a user-supplied code image from its entry point with
// a minimal fake kernel, logging SVCs, faults and dispatcher coverage. Local analysis only; the generated
// code it links is derived from game data and must not be committed.
//
//   boot_probe <code.bin> [maxSvcs] [maxSteps]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"

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
constexpr uint32_t kStackTop = 0x08100000, kStackSize = 0x100000;
constexpr uint32_t kHeapBase = 0x08200000, kHeapSize = 0x04000000;
constexpr uint32_t kTls = 0x1FF82000;
constexpr uint32_t kHalt = 0x00FF0000;
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: boot_probe <code.bin> [maxSvcs] [maxSteps]\n");
        return 2;
    }
    const unsigned maxSvcs = argc > 2 ? unsigned(std::atoi(argv[2])) : 50;
    const uint64_t maxSteps = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200000000ull;
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
    auto up = [&](uint32_t v) { return (v + pg - 1) & ~(pg - 1); };
    gm.map(kText, up(kImageEnd - kText), rw);
    gm.map(kStackTop - kStackSize, kStackSize, rw);
    gm.map(kHeapBase, kHeapSize, rw);
    gm.map(kTls & ~(pg - 1), pg, rw);
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
    disp.setHaltAddress(kHalt);
    std::printf("registered %zu blocks\n", disp.blockCount());

    RcCtx c;
    std::memset(&c, 0, sizeof c);
    c.mem = gm.base();
    c.tls = kTls;
    c.r[13] = kStackTop;
    c.r[14] = kHalt;
    c.r[15] = kText; // the image's entry point
    uint32_t heapNext = kHeapBase;
    unsigned svcs = 0, handles = 0;
    std::map<uint32_t, unsigned> pcSamples;
    uint64_t steps = 0;
    for (;;) {
        StopReason why = StopReason::StepLimit;
        GuestFault fault;
        const uint32_t pcBefore = c.r[15];
        const bool ok = gm.runGuarded([&] { why = disp.run(c, 1000000); }, &fault);
        steps += 1000000;
        pcSamples[c.r[15]]++;
        if (!ok) {
            std::printf("FAULT: guest access to %08x (block entered near pc %08x)\n", fault.address,
                        pcBefore);
            break;
        }
        if (why == StopReason::Halt) {
            std::printf("HALT: control reached the exit address\n");
            break;
        }
        if (why == StopReason::Trap) {
            std::printf("TRAP at pc %08x\n", c.trap_pc);
            break;
        }
        if (why == StopReason::Svc) {
            svcs++;
            std::printf("SVC %#04x r0=%08x r1=%08x r2=%08x r3=%08x (pc now %08x, %s)\n", c.svc, c.r[0],
                        c.r[1], c.r[2], c.r[3], c.r[15], c.thumb ? "thumb" : "arm");
            c.svc_pending = 0;
            // Throwaway fake kernel: raw SVC ABIs from the public 3dbrew documentation (result in r0, new
            // handle in r1). It only exists to see how far the recompiled game gets; the real HLE is issue
            // #9/#10.
            auto newHandle = [&] { return 0xC0000000u + (++handles); };
            switch (c.svc) {
            case 0x01: // ControlMemory(op, addr0, addr1, size, perm) -> addr in r1
                c.r[0] = 0;
                c.r[1] = heapNext;
                heapNext += (c.r[3] + 0xFFF) & ~0xFFFu;
                break;
            case 0x02:
            case 0x03:
                break;
            case 0x08:
            case 0x13:
            case 0x15:
            case 0x17:
            case 0x1A:
            case 0x1E:
            case 0x21:
            case 0x27:
            case 0x2D:
            case 0x38:
                c.r[0] = 0;
                c.r[1] = newHandle();
                break;
            case 0x0A:
            case 0x14:
            case 0x18:
            case 0x19:
            case 0x22:
            case 0x23:
            case 0x24:
            case 0x1F:
            case 0x20:
                c.r[0] = 0;
                break;
            case 0x0B:
            case 0x0C:
                c.r[0] = 0;
                c.r[1] = 0x30;
                break;
            case 0x25: // WaitSynchronizationN
                c.r[0] = 0;
                c.r[1] = 0;
                break;
            case 0x28: // GetSystemTick
                c.r[0] = uint32_t(steps);
                c.r[1] = 0;
                break;
            case 0x2B:
            case 0x35:
            case 0x37: // GetProcessInfo / GetProcessId / GetThreadId
                c.r[0] = 0;
                c.r[1] = 1;
                c.r[2] = 0;
                break;
            case 0x32: { // SendSyncRequest: pretend every service replies "success" with no data
                uint32_t *cmd = reinterpret_cast<uint32_t *>(gm.base() + kTls + 0x80);
                std::printf("  IPC header %08x -> handle %08x\n", cmd[0], c.r[0]);
                cmd[1] = 0;
                c.r[0] = 0;
                break;
            }
            case 0x3A: // GetResourceLimitCurrentValues(values*, handle, names*, count)
                for (uint32_t k = 0; k < c.r[3]; k++) {
                    uint64_t v = 0;
                    std::memcpy(gm.base() + c.r[0] + 8 * k, &v, 8);
                }
                c.r[0] = 0;
                break;
            case 0x3C: // Break
                std::printf("  guest called svcBreak(%u)\n", c.r[0]);
                svcs = maxSvcs;
                break;
            case 0x3D: // OutputDebugString(ptr, len)
                std::printf("  debug: %.*s\n", int(c.r[1]), reinterpret_cast<char *>(gm.base() + c.r[0]));
                c.r[0] = 0;
                break;
            default:
                std::printf("  unhandled SVC; stopping\n");
                svcs = maxSvcs;
                break;
            }
            if (svcs >= maxSvcs)
                break;
        }
        if (steps >= maxSteps) {
            std::printf("step limit\n");
            break;
        }
    }
    {
        std::vector<std::pair<unsigned, uint32_t>> top;
        for (auto &kv : pcSamples)
            top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        std::printf("pc samples (after each 1M-step chunk):");
        for (size_t k = 0; k < top.size() && k < 6; k++)
            std::printf(" %08x x%u", top[k].second, top[k].first);
        std::printf("\n");
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
