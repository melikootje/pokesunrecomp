// PICA vertex shader interpreter on hand-assembled programs (no game data).
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "pokerecomp/pica_shader.h"

using namespace pokerecomp::pica;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace {
constexpr uint32_t V(uint32_t n) { return n; }
constexpr uint32_t R(uint32_t n) { return 0x10 + n; }
constexpr uint32_t C(uint32_t n) { return 0x20 + n; }
constexpr uint32_t O(uint32_t n) { return n; }
constexpr uint32_t sel(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return x << 6 | y << 4 | z << 2 | w;
}
constexpr uint32_t kId = sel(0, 1, 2, 3);

// operand descriptor: write mask (bit3 = x ... bit0 = w), per-source negate and swizzle
constexpr uint32_t desc(uint32_t mask, uint32_t s1 = kId, bool n1 = false, uint32_t s2 = kId, bool n2 = false,
                        uint32_t s3 = kId, bool n3 = false) {
    return mask | uint32_t(n1) << 4 | s1 << 5 | uint32_t(n2) << 13 | s2 << 14 | uint32_t(n3) << 22 | s3 << 23;
}
constexpr uint32_t f1(uint32_t op, uint32_t dst, uint32_t s1, uint32_t s2, uint32_t d, uint32_t idx = 0) {
    return op << 26 | dst << 21 | idx << 19 | s1 << 12 | s2 << 7 | d;
}
constexpr uint32_t f1i(uint32_t op, uint32_t dst, uint32_t s1, uint32_t s2, uint32_t d, uint32_t idx = 0) {
    return op << 26 | dst << 21 | idx << 19 | s1 << 14 | s2 << 7 | d; // inverted: src2 is the 7-bit field
}
constexpr uint32_t f1u(uint32_t op, uint32_t dst, uint32_t s1, uint32_t d, uint32_t idx = 0) {
    return op << 26 | dst << 21 | idx << 19 | s1 << 12 | d;
}
constexpr uint32_t fcmp(uint32_t cx, uint32_t cy, uint32_t s1, uint32_t s2, uint32_t d) {
    return (0x2Eu << 26) | cx << 24 | cy << 21 | s1 << 12 | s2 << 7 | d;
}
constexpr uint32_t f2(uint32_t op, uint32_t dst, uint32_t num, uint32_t condop = 0, uint32_t refx = 0,
                      uint32_t refy = 0) {
    return op << 26 | refx << 25 | refy << 24 | condop << 22 | dst << 10 | num;
}
constexpr uint32_t f3(uint32_t op, uint32_t dst, uint32_t num, uint32_t id) {
    return op << 26 | id << 22 | dst << 10 | num;
}
constexpr uint32_t fmad(uint32_t dst, uint32_t s1, uint32_t s2, uint32_t s3, uint32_t d, uint32_t idx = 0) {
    return 7u << 29 | dst << 24 | idx << 22 | s1 << 17 | s2 << 10 | s3 << 5 | d;
}
constexpr uint32_t kEnd = 0x22u << 26, kNop = 0x21u << 26;
enum {
    ADD = 0,
    DP3 = 1,
    DP4 = 2,
    DPH = 3,
    DST = 4,
    EX2 = 5,
    LG2 = 6,
    MUL = 8,
    SGE = 9,
    SLT = 0xA,
    FLR = 0xB,
    MAX = 0xC,
    MIN = 0xD,
    RCP = 0xE,
    RSQ = 0xF,
    MOVA = 0x12,
    MOV = 0x13,
    SLTI = 0x1B,
    DPHI = 0x18,
    BREAKC = 0x23,
    CALL = 0x24,
    CALLU = 0x26,
    IFU = 0x27,
    IFC = 0x28,
    LOOP = 0x29,
    JMPC = 0x2C,
    JMPU = 0x2D
};

struct Prog {
    ShaderUnit u;
    std::vector<uint32_t> d; // descriptor table in use
    Prog() {
        u.opdesc[0] = desc(0xF);
        for (auto &c : u.code)
            c = kEnd;
    }
    void put(uint32_t at, uint32_t w) { u.code[at] = w; }
    bool run(const Vec4 *in, Vec4 *out) { return u.run(in, out); }
};
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
bool eq(const Vec4 &a, float x, float y, float z, float w) {
    return near(a[0], x) && near(a[1], y) && near(a[2], z) && near(a[3], w);
}
} // namespace

int main() {
    Vec4 in[16], out[16];
    in[0] = {{1, 2, 3, 4}};
    in[1] = {{10, 20, 30, 40}};

    { // MOV / swizzle / negate / write mask
        Prog p;
        p.u.opdesc[1] = desc(0xF, sel(3, 2, 1, 0), true); // -v.wzyx
        p.u.opdesc[2] = desc(0x5, sel(0, 0, 0, 0));       // .y and .w written with v.xxxx
        p.put(0, f1u(MOV, O(0), V(0), 0));
        p.put(1, f1u(MOV, O(1), V(0), 1));
        p.put(2, f1u(MOV, O(2), V(1), 2));
        p.put(3, kEnd);
        for (auto &o : out)
            o = {{-1, -1, -1, -1}};
        CHECK(p.run(in, out));
        CHECK(eq(out[0], 1, 2, 3, 4));
        CHECK(eq(out[1], -4, -3, -2, -1));
        CHECK(eq(out[2], -1, 10, -1, 10)); // mask 0x5 = y and w
    }
    { // 4x4 matrix * vector with four DP4s writing one component each
        Prog p;
        p.u.opdesc[1] = desc(0x8);
        p.u.opdesc[2] = desc(0x4);
        p.u.opdesc[3] = desc(0x2);
        p.u.opdesc[4] = desc(0x1);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                p.u.fconst[r][c] = float(r * 4 + c + 1);
        // Constants can only be the wide (7-bit) first source of the regular format; DP4 is commutative.
        for (int r = 0; r < 4; r++)
            p.put(r, f1(DP4, O(0), C(r), V(0), 1 + r));
        p.put(4, kEnd);
        CHECK(p.run(in, out));
        CHECK(
            eq(out[0], 1 * 1 + 2 * 2 + 3 * 3 + 4 * 4, 5 + 12 + 21 + 32, 9 + 20 + 33 + 48, 13 + 28 + 45 + 64));
    }
    { // ADD MUL MAD SLT SGE MIN MAX FLR DP3 DPH
        Prog p;
        in[2] = {{1.5f, -2.5f, 3.25f, 0}};
        p.put(0, f1(ADD, R(0), C(0), V(1), 0)); // c0 + v1
        p.u.fconst[0] = {{1, 1, 1, 1}};
        p.put(1, f1(MUL, R(1), V(0), V(1), 0));
        p.put(2, fmad(R(2), V(0), V(1), V(1), 0)); // v0*v1 + v1  (MAD: src1 src2 src3)
        p.put(3, f1(SLT, R(3), V(0), V(1), 0));
        p.put(4, f1(SGE, R(4), V(0), V(1), 0));
        p.put(5, f1(MIN, R(5), V(0), V(1), 0));
        p.put(6, f1(MAX, R(6), V(0), V(1), 0));
        p.put(7, f1u(FLR, R(7), V(2), 0));
        p.put(8, f1(DP3, R(8), V(0), V(1), 0));
        p.put(9, f1(DPH, R(9), V(0), V(1), 0));
        for (int i = 0; i < 10; i++)
            p.put(10 + i, f1u(MOV, O(i), R(i), 0));
        p.put(20, f1i(DPHI, O(10), V(0), C(2), 0)); // inverted format: the constant is the second operand
        p.put(21, kEnd);
        p.u.fconst[2] = {{1, 2, 3, 5}};
        CHECK(p.run(in, out));
        CHECK(eq(out[10], 1 + 4 + 9 + 5, 19, 19, 19)); // v0.xyz . c2.xyz + c2.w
        CHECK(eq(out[0], 11, 21, 31, 41));
        CHECK(eq(out[1], 10, 40, 90, 160));
        CHECK(eq(out[2], 20, 60, 120, 200));
        CHECK(eq(out[3], 1, 1, 1, 1));
        CHECK(eq(out[4], 0, 0, 0, 0));
        CHECK(eq(out[5], 1, 2, 3, 4));
        CHECK(eq(out[6], 10, 20, 30, 40));
        CHECK(eq(out[7], 1, -3, 3, 0));
        CHECK(eq(out[8], 10 + 40 + 90, 140, 140, 140));
        CHECK(eq(out[9], 10 + 40 + 90 + 40, 180, 180, 180)); // DPH: src1.w counts as 1.0
    }
    { // scalar functions broadcast
        Prog p;
        in[3] = {{4, 0, 0, 0}};
        p.put(0, f1u(RCP, O(0), V(3), 0));
        p.put(1, f1u(RSQ, O(1), V(3), 0));
        p.put(2, f1u(EX2, O(2), V(3), 0));
        p.put(3, f1u(LG2, O(3), V(3), 0));
        p.put(4, kEnd);
        CHECK(p.run(in, out));
        CHECK(eq(out[0], 0.25f, 0.25f, 0.25f, 0.25f));
        CHECK(eq(out[1], 0.5f, 0.5f, 0.5f, 0.5f));
        CHECK(eq(out[2], 16, 16, 16, 16));
        CHECK(eq(out[3], 2, 2, 2, 2));
    }
    { // 0 * inf = 0 (PICA), but inf stays inf elsewhere
        Prog p;
        in[4] = {{0, 1, 0, 2}};
        in[5] = {{INFINITY, INFINITY, 2, 2}};
        p.put(0, f1(MUL, O(0), V(4), V(5), 0));
        p.put(1, kEnd);
        CHECK(p.run(in, out));
        CHECK(out[0][0] == 0 && std::isinf(out[0][1]) && out[0][2] == 0 && out[0][3] == 4);
    }
    { // relative addressing through a0.x, a0.y and aL, and out-of-range reads
        Prog p;
        for (int i = 0; i < 8; i++)
            p.u.fconst[i] = {{float(i), 0, 0, 0}};
        in[6] = {{2, 5, 0, 0}};
        p.u.opdesc[1] = desc(0xC); // MOVA writes a0.x and a0.y
        p.put(0, f1u(MOVA, O(15), V(6), 1));
        p.put(1, f1u(MOV, O(0), C(1), 0, 1)); // c[1 + a0.x] = c3
        p.put(2, f1u(MOV, O(1), C(1), 0, 2)); // c[1 + a0.y] = c6
        p.put(3, kEnd);
        CHECK(p.run(in, out));
        CHECK(out[0][0] == 3 && out[1][0] == 6);
        Prog q;
        in[7] = {{100, 0, 0, 0}};
        q.u.opdesc[1] = desc(0xC);
        q.put(0, f1u(MOVA, O(15), V(7), 1));
        q.put(1, f1u(MOV, O(0), C(0), 0, 1)); // 0x20 + 100 > 95 constants: reads (1,1,1,1)
        q.put(2, kEnd);
        CHECK(q.run(in, out) && eq(out[0], 1, 1, 1, 1));
    }
    { // IFU: then and else paths
        for (int b = 0; b < 2; b++) {
            Prog p;
            p.u.bconst[3] = b;
            p.u.fconst[0] = {{1, 0, 0, 0}};
            p.u.fconst[1] = {{2, 0, 0, 0}};
            p.put(0, f3(IFU, 3, 1, 3)); // if b3: [1,3) else: [3,4)
            p.put(1, f1u(MOV, O(0), C(0), 0));
            p.put(2, kNop);
            p.put(3, f1u(MOV, O(0), C(1), 0));
            p.put(4, f1u(MOV, O(1), C(0), 0));
            p.put(5, kEnd);
            // layout: IFU dst=3 num=1: then = pc+1..dst-1 = {1,2}; else = {3}; join at dst+num = 4
            CHECK(p.run(in, out));
            CHECK(out[0][0] == (b ? 1.0f : 2.0f));
            CHECK(out[1][0] == 1.0f); // both paths continue after the join
        }
    }
    { // CALL with a return, nested via a second CALLU
        Prog p;
        p.u.bconst[0] = true;
        p.u.fconst[0] = {{1, 1, 1, 1}};
        p.put(0, f2(CALL, 10, 2));                 // call [10, 12)
        p.put(1, f1(ADD, R(0), R(0), C(0), 0, 0)); // r0 += c0 (src2 const needs the inverted form below)
        p.put(1, f1u(MOV, O(1), R(0), 0));
        p.put(2, kEnd);
        p.put(10, f1(ADD, R(0), V(0), V(1), 0));
        p.put(11, f1u(MOV, O(0), R(0), 0));
        CHECK(p.run(in, out));
        CHECK(eq(out[0], 11, 22, 33, 44) && eq(out[1], 11, 22, 33, 44));
    }
    { // LOOP: x+1 iterations, aL from y by z; accumulate c[aL]
        Prog p;
        p.u.iconst[0][0] = 3; // 4 iterations
        p.u.iconst[0][1] = 2; // aL starts at 2
        p.u.iconst[0][2] = 1;
        for (int i = 0; i < 8; i++)
            p.u.fconst[i] = {{float(1 << i), 0, 0, 0}};
        p.u.opdesc[1] = desc(0x8);
        p.put(0, f1u(MOV, R(1), V(7), 0));    // r1 = 0 (v7 is zero)
        p.put(1, f3(LOOP, 3, 0, 0));          // body [2, 3]
        p.put(2, f1u(MOV, R(2), C(0), 0, 3)); // r2 = c[0 + aL]
        p.put(3, f1(ADD, R(1), R(1), R(2), 0));
        p.put(4, f1u(MOV, O(0), R(1), 0));
        p.put(5, kEnd);
        for (auto &i : in)
            i = {};
        CHECK(p.run(in, out));
        CHECK(out[0][0] == float((1 << 2) + (1 << 3) + (1 << 4) + (1 << 5)));
    }
    { // CMP + IFC, JMPU, BREAKC
        Prog p;
        in[8] = {{1, 5, 0, 0}};
        in[9] = {{2, 5, 0, 0}};
        p.u.fconst[0] = {{7, 7, 7, 7}};
        p.put(0, fcmp(2 /*LT*/, 0 /*EQ*/, V(8), V(9), 0)); // cmp.x = 1<2 true, cmp.y = 5==5 true
        p.put(1, f2(IFC, 4, 1, 1 /*AND*/, 1, 1));          // if (x==1 && y==1): [2,4) else [4,5)
        p.put(2, f1u(MOV, O(0), C(0), 0));
        p.put(3, kNop);
        p.put(4, f1u(MOV, O(1), C(0), 0));
        p.put(5, kEnd);
        for (auto &o : out)
            o = {};
        CHECK(p.run(in, out));
        CHECK(out[0][0] == 7 && out[1][0] == 0);

        Prog j;
        j.u.bconst[1] = true;
        j.put(0, f3(JMPU, 3, 0, 1)); // jump when b1 set (bit0 of num clear)
        j.put(1, f1u(MOV, O(0), V(8), 0));
        j.put(2, kEnd);
        j.put(3, f1u(MOV, O(0), V(9), 0));
        j.put(4, kEnd);
        CHECK(j.run(in, out) && out[0][0] == 2);
        j.u.bconst[1] = false;
        CHECK(j.run(in, out) && out[0][0] == 1);

        Prog b; // BREAKC leaves the loop early: r0 counts iterations until cmp is true
        b.u.iconst[0][0] = 9;
        b.u.iconst[0][2] = 1;
        b.u.fconst[0] = {{1, 0, 0, 0}};
        in[10] = {{3, 0, 0, 0}};
        b.put(0, f1u(MOV, R(0), V(11), 0));
        b.put(1, f3(LOOP, 4, 0, 0));                 // body [2,4]
        b.put(2, f1(ADD, R(0), R(0), V(12), 0));     // r0 += v12 (=1)
        b.put(3, fcmp(4 /*GT*/, 7, R(0), V(10), 0)); // cmp.x = r0 > 3
        b.put(4, f2(BREAKC, 0, 0, 2 /*x only*/, 1, 0));
        b.put(5, f1u(MOV, O(0), R(0), 0));
        b.put(6, kEnd);
        in[11] = {};
        in[12] = {{1, 0, 0, 0}};
        CHECK(b.run(in, out) && out[0][0] == 4);
    }
    { // malformed programs fail instead of running away
        Prog p;
        p.put(0, 0x1Cu << 26); // undocumented opcode
        CHECK(!p.run(in, out));
        Prog q;
        q.put(0, f3(JMPU, 0, 1, 0)); // jump to itself while b0 is clear (bit0 = 1 inverts)
        CHECK(!q.run(in, out));
    }
    std::printf(g_fail ? "FAIL\n" : "pica_shader ok\n");
    return g_fail ? 1 : 0;
}
