#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "harness_mem.h"
#include "pokerecomp/cpu.h"
#include "recomp/decode.h"
#include "recomp/interp.h"
#include "vectors.h"

extern "C" {
uint8_t rc_test_window[0x10000];
unsigned rc_test_faults = 0;
uint32_t rc_test_last_fault = 0;
uint8_t rc_test_scratch[16];
uint8_t rc_test_code[0x1000];
typedef void (*rc_fn)(RcCtx *);
extern const rc_fn rc_vec_fns[][2];
extern const unsigned rc_vec_count;
}

using namespace recomp;
using vec::decodeVector;

namespace {

void resetWindow() {
    for (uint32_t i = 0; i < vec::kWinSize; i++)
        rc_test_window[i] = vec::memf(vec::kWin + i);
    rc_test_faults = 0;
}

// Places the vector's instruction bytes at the code address so PC-relative loads see them.
void loadCode(const vec::Vector &v) {
    std::memset(rc_test_code, 0, sizeof rc_test_code);
    for (size_t j = 0; j < v.words.size() && 4 * j + 4 <= sizeof rc_test_code; j++) {
        if (v.thumbMode) {
            rc_test_code[2 * j] = uint8_t(v.words[j]);
            rc_test_code[2 * j + 1] = uint8_t(v.words[j] >> 8);
        } else {
            for (int b = 0; b < 4; b++)
                rc_test_code[4 * j + b] = uint8_t(v.words[j] >> (8 * b));
        }
    }
}

class WinMem : public Memory {
  public:
    uint8_t r8(uint32_t a) override { return rc_t_ld8(a); }
    uint16_t r16(uint32_t a) override { return rc_t_ld16(a); }
    uint32_t r32(uint32_t a) override { return rc_t_ld32(a); }
    void w8(uint32_t a, uint8_t v) override { rc_t_st8(a, v); }
    void w16(uint32_t a, uint16_t v) override { rc_t_st16(a, v); }
    void w32(uint32_t a, uint32_t v) override { rc_t_st32(a, v); }
};

void load(RcCtx &c, const vec::Vector &v) {
    std::memset(&c, 0, sizeof c);
    for (int i = 0; i < 15; i++)
        c.r[i] = v.init[i];
    c.n = (v.flags >> 3) & 1;
    c.z = (v.flags >> 2) & 1;
    c.c = (v.flags >> 1) & 1;
    c.v = v.flags & 1;
    c.tls = v.tls;
    c.q = (v.qge >> 4) & 1;
    c.ge = v.qge & 0xF;
    c.thumb = v.thumbMode ? 1 : 0;
    if (v.vfp) {
        for (int i = 0; i < 32; i++)
            c.s[i] = v.sInit[i];
        c.fpscr = v.fpscrInit;
    }
}

struct Result {
    std::string why;
    bool ok() const { return why.empty(); }
};

Result check(const RcCtx &c, const vec::Vector &v, bool flags) {
    char b[160];
    for (int i = 0; i < 15; i++)
        if (c.r[i] != v.final_[i]) {
            std::snprintf(b, sizeof b, "r%d got %08x want %08x", i, c.r[i], v.final_[i]);
            return {b};
        }
    if (c.r[15] != v.pc) {
        std::snprintf(b, sizeof b, "pc got %08x want %08x", c.r[15], v.pc);
        return {b};
    }
    if (c.thumb != v.thumb)
        return {"thumb"};
    if (flags) {
        const uint32_t nzcv = (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v;
        if (nzcv != v.nzcv) {
            std::snprintf(b, sizeof b, "nzcv got %x want %x", nzcv, v.nzcv);
            return {b};
        }
    }
    if (((c.q << 4) | c.ge) != v.finalQge) {
        std::snprintf(b, sizeof b, "q/ge got %x want %x", (c.q << 4) | c.ge, v.finalQge);
        return {b};
    }
    if (v.vfp) {
        for (int i = 0; i < 32; i++)
            if (c.s[i] != v.sFinal[i]) {
                std::snprintf(b, sizeof b, "s%d got %08x want %08x", i, c.s[i], v.sFinal[i]);
                return {b};
            }
        if (c.fpscr != v.fpscrFinal) {
            std::snprintf(b, sizeof b, "fpscr got %08x want %08x", c.fpscr, v.fpscrFinal);
            return {b};
        }
    }
    if (c.trap)
        return {"trap"};
    if (rc_test_faults) {
        std::snprintf(b, sizeof b, "memory fault at %08x", rc_test_last_fault);
        return {b};
    }
    std::map<uint32_t, uint8_t> got;
    for (uint32_t i = 0; i < vec::kWinSize; i++)
        if (rc_test_window[i] != vec::memf(vec::kWin + i))
            got[vec::kWin + i] = rc_test_window[i];
    std::map<uint32_t, uint8_t> want(v.diffs.begin(), v.diffs.end());
    if (got != want)
        return {"memory contents differ"};
    return {};
}

} // namespace

int main(int argc, char **argv) {
    const bool interpOnly =
        argc == 3 && std::string(argv[2]) == "--interp-only"; // debugging aid for ad-hoc vector files
    if (argc != 2 && !interpOnly) {
        std::fprintf(stderr, "usage: recomp_tests vectors\n");
        return 2;
    }
    const auto vs = vec::load(argv[1]);
    if (vs.empty()) {
        std::fprintf(stderr, "no vectors\n");
        return 1;
    }
    if (!interpOnly && vs.size() != rc_vec_count) {
        std::fprintf(stderr, "generated block count mismatch\n");
        return 1;
    }
    unsigned failInterp = 0, failFull = 0, failDead = 0;
    std::map<std::string, unsigned> failByOp;
    for (size_t k = 0; k < vs.size(); k++) {
        const vec::Vector &v = vs[k];
        // reference interpreter
        {
            RcCtx c;
            load(c, v);
            resetWindow();
            WinMem mem;
            loadCode(v);
            for (const Insn &in : decodeVector(v))
                step(c, mem, in);
            Result r = check(c, v, true);
            if (!r.ok()) {
                if (failInterp < 10) {
                    std::printf("interp vector %zu: %s%s\n", k, r.why.c_str(),
                                v.vfp ? (" [fpscr_init=" + std::to_string(v.fpscrInit) + "]").c_str() : "");
                    if (v.vfp && getenv("RC_DUMP_S")) {
                        RcCtx dc;
                        load(dc, v);
                        resetWindow();
                        WinMem dm;
                        loadCode(v);
                        for (const Insn &in : decodeVector(v))
                            step(dc, dm, in);
                        for (int q = 0; q < 32; q++)
                            if (v.sInit[q] != v.sFinal[q] || dc.s[q] != v.sFinal[q])
                                std::printf("      s%-2d init %08x want %08x got %08x\n", q, v.sInit[q],
                                            v.sFinal[q], dc.s[q]);
                    }
                    if (v.vfp)
                        std::printf("    init fpscr=%08x len=%u stride=%u rmode=%u fz=%u dn=%u\n",
                                    v.fpscrInit, (v.fpscrInit >> 16) & 7, (v.fpscrInit >> 20) & 3,
                                    (v.fpscrInit >> 22) & 3, (v.fpscrInit >> 24) & 1,
                                    (v.fpscrInit >> 25) & 1);
                    for (const Insn &in : decodeVector(v))
                        std::printf("    %s\n", disasm(in).c_str());
                }
                for (const Insn &di : decodeVector(v)) {
                    std::string key = opName(di.op);
                    if (di.op == Op::Vfp)
                        key += ":" + std::to_string(di.sub) + (di.dbl ? "d" : "s");
                    failByOp[key]++;
                }
                failInterp++;
            }
        }
        for (int variant = 0; variant < (interpOnly ? 0 : 2); variant++) {
            RcCtx c;
            load(c, v);
            resetWindow();
            loadCode(v);
            rc_vec_fns[k][variant](&c);
            Result r = check(c, v, variant == 0);
            if (!r.ok()) {
                unsigned &f = variant == 0 ? failFull : failDead;
                if (f < 10)
                    std::printf("emitted %s vector %zu: %s\n", variant == 0 ? "full" : "dead-flag", k,
                                r.why.c_str());
                f++;
            }
        }
    }
    std::printf("vectors=%zu interp_fail=%u emitted_full_fail=%u emitted_dead_flag_fail=%u\n", vs.size(),
                failInterp, failFull, failDead);
    for (auto &kv : failByOp)
        std::printf("  involved %s: %u\n", kv.first.c_str(), kv.second);
    return (failInterp || failFull || failDead) ? 1 : 0;
}
