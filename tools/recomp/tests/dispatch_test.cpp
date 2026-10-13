// Dispatcher behaviour on synthetic programs: loops, jump tables, interworking indirect calls, fallback and
// coverage, invalidation (self-modifying code) and SVC servicing.
#include <cstdio>
#include <cstring>
#include <vector>

#include "dispatch_programs.h"
#include "pokerecomp/dispatcher.h"

extern "C" {
typedef struct {
    void (*fn)(RcCtx *);
    uint32_t addr, bytes, insns, thumb;
} rc_block_t;
extern const rc_block_t rc_blocks[];
extern const unsigned rc_block_count;
}

using pokerecomp::Dispatcher;
using pokerecomp::StopReason;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

static void registerAll(Dispatcher &d, uint32_t skipAddr = 0xFFFFFFFF) {
    for (unsigned k = 0; k < rc_block_count; k++) {
        const rc_block_t &b = rc_blocks[k];
        if (b.addr == skipAddr)
            continue;
        pokerecomp::BlockInfo bi;
        bi.fn = b.fn;
        bi.addr = b.addr;
        bi.bytes = b.bytes;
        bi.insns = b.insns;
        bi.thumb = b.thumb != 0;
        d.registerBlock(bi);
    }
}

struct Guest {
    prog::Image img = prog::build();
    RcCtx c;
    Guest() { reset(); }
    void reset() {
        std::memset(&c, 0, sizeof c);
        c.mem = img.bytes.data();
        c.r[14] = prog::kHalt;
    }
    void start(uint32_t pc, bool thumb = false) {
        c.r[15] = pc;
        c.thumb = thumb ? 1 : 0;
    }
};

int main() {
    // 1. loop + conditional branch, fully recompiled
    {
        Guest g;
        Dispatcher d;
        registerAll(d);
        d.setHaltAddress(prog::kHalt);
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 10000) == StopReason::Halt);
        CHECK(g.c.r[0] == 55);
        CHECK(d.coverage().interpretedInsns == 0);
        CHECK(d.coverage().percentRecompiled() == 100.0);
        CHECK(d.coverage().blocksRun > 10); // the loop block ran 10 times
    }
    // 2. jump table: targets recovered statically (not listed as entries), default path included
    {
        const unsigned want[5] = {10, 11, 12, 13, 99};
        const unsigned in[5] = {0, 1, 2, 3, 7};
        for (int k = 0; k < 5; k++) {
            Guest g;
            Dispatcher d;
            registerAll(d);
            d.setHaltAddress(prog::kHalt);
            g.c.r[0] = in[k];
            g.start(prog::kTable);
            CHECK(d.run(g.c, 100) == StopReason::Halt);
            CHECK(g.c.r[1] == want[k]);
            CHECK(d.coverage().interpretedInsns == 0);
        }
    }
    // 3. virtual call through a vtable: ARM caller -> Thumb callee (interworking), then -> ARM callee
    {
        Guest g;
        Dispatcher d;
        registerAll(d);
        d.setHaltAddress(prog::kHalt);
        g.c.r[0] = prog::kObj;
        g.c.r[1] = 1;
        g.start(prog::kVcall);
        CHECK(d.run(g.c, 100) == StopReason::Halt);
        CHECK(g.c.r[1] == 6);  // adds r1, #5 in Thumb
        CHECK(g.c.thumb == 0); // returned to ARM state (lr written by blx has bit 0 clear)
        CHECK(d.coverage().interpretedInsns == 0);
        g.reset();
        g.img.w32(prog::kVtbl + 8, prog::kArmFn); // slot now points at the ARM implementation
        g.c.r[0] = prog::kObj;
        g.c.r[1] = 1;
        g.start(prog::kVcall);
        CHECK(d.run(g.c, 100) == StopReason::Halt);
        CHECK(g.c.r[1] == 2); // add r1, r1, r1
        CHECK(d.coverage().interpretedInsns == 0);
    }
    // 4. fallback: leave the loop body unrecompiled -> the interpreter runs it and the hits are logged
    {
        Guest g;
        Dispatcher d;
        registerAll(d, prog::kLoop + 8);
        d.setHaltAddress(prog::kHalt);
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 10000) == StopReason::Halt);
        CHECK(g.c.r[0] == 55); // same result as the fully recompiled run
        CHECK(d.coverage().interpretedInsns == 30);
        CHECK(d.coverage().recompiledInsns == 3);
        CHECK(d.coverage().percentRecompiled() < 10.0 && d.coverage().percentRecompiled() > 9.0);
        const auto top = d.topFallbacks(3);
        CHECK(top.size() == 3);
        for (const auto &t : top)
            CHECK(t.second == 10);
        CHECK(d.fallbackHits().count(prog::kLoop + 8) == 1);
        // nothing registered at all: still correct, 0% recompiled
        Dispatcher none;
        none.setHaltAddress(prog::kHalt);
        g.reset();
        g.start(prog::kLoop);
        CHECK(none.run(g.c, 10000) == StopReason::Halt);
        CHECK(g.c.r[0] == 55);
        CHECK(none.coverage().percentRecompiled() == 0.0);
    }
    // 5. self-modifying / reloaded code: stale blocks keep running until invalidated
    {
        Guest g;
        Dispatcher d;
        registerAll(d);
        d.setHaltAddress(prog::kHalt);
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 10000) == StopReason::Halt);
        CHECK(g.c.r[0] == 55);
        g.img.w32(prog::kLoop + 8, 0xE0400001u); // sub r0, r0, r1   (was add)
        g.reset();
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 10000) == StopReason::Halt);
        CHECK(g.c.r[0] == 55); // stale recompiled copy: this is why the HLE must call invalidate()
        d.invalidate(prog::kLoop + 8, 4);
        CHECK(d.invalidatedBlocks() == 1);
        g.reset();
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 10000) == StopReason::Halt);
        CHECK(int32_t(g.c.r[0]) == -55); // new code executed by the fallback interpreter
        CHECK(d.coverage().interpretedInsns > 0);
        // an unrelated range invalidates nothing
        d.invalidate(0x300000, 0x1000);
        CHECK(d.invalidatedBlocks() == 1);
    }
    // 6. SVC stops the run with the number pending; after servicing, execution continues
    {
        Guest g;
        Dispatcher d;
        registerAll(d);
        d.setHaltAddress(prog::kHalt);
        g.start(prog::kSvc);
        CHECK(d.run(g.c, 100) == StopReason::Svc);
        CHECK(g.c.svc == 0x32 && g.c.svc_pending == 1);
        CHECK(g.c.r[0] == 7);
        g.c.svc_pending = 0;
        CHECK(d.run(g.c, 100) == StopReason::Halt);
        CHECK(g.c.r[0] == 8);
    }
    // 7. step limit and undefined instruction trap (executed by the fallback interpreter)
    {
        Guest g;
        Dispatcher d;
        registerAll(d);
        g.start(prog::kLoop);
        CHECK(d.run(g.c, 1) == StopReason::StepLimit);
        g.img.w32(0x100500, 0xF7F0A000u); // undefined encoding
        g.start(0x100500);
        CHECK(d.run(g.c, 10) == StopReason::Trap);
        CHECK(g.c.trap_pc == 0x100500);
    }
    std::printf(g_fail ? "FAIL\n" : "dispatch_test ok\n");
    return g_fail ? 1 : 0;
}
