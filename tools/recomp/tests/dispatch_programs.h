#pragma once
// Hand-assembled synthetic guest programs shared by the generator and the dispatcher test.
#include <cstdint>
#include <map>
#include <vector>

namespace prog {

constexpr uint32_t kImageSize = 0x400000;
constexpr uint32_t kHalt = 0x003FF000; // return address that stops the dispatcher
constexpr uint32_t kLoop = 0x100000, kTable = 0x100100, kVcall = 0x100200, kThumbFn = 0x100300,
                   kArmFn = 0x100340, kSvc = 0x100400;
constexpr uint32_t kObj = 0x200000, kVtbl = 0x200100;

// --- tiny ARM assembler (A32) ---
inline uint32_t movi(unsigned rd, unsigned imm) { return 0xE3A00000u | rd << 12 | imm; }
inline uint32_t addr_(unsigned rd, unsigned rn, unsigned rm) {
    return 0xE0800000u | rn << 16 | rd << 12 | rm;
}
inline uint32_t subsi(unsigned rd, unsigned rn, unsigned imm) {
    return 0xE2500000u | rn << 16 | rd << 12 | imm;
}
inline uint32_t subi(unsigned rd, unsigned rn, unsigned imm) {
    return 0xE2400000u | rn << 16 | rd << 12 | imm;
}
inline uint32_t cmpi(unsigned rn, unsigned imm) { return 0xE3500000u | rn << 16 | imm; }
inline uint32_t b(unsigned cond, uint32_t from, uint32_t to) {
    return cond << 28 | 0x0A000000u | (((to - (from + 8)) >> 2) & 0xFFFFFF);
}
inline uint32_t ldrls_pc(unsigned rm) { return 0x979FF100u | rm; } // ldrls pc, [pc, rm, lsl #2]
inline uint32_t ldri(unsigned rd, unsigned rn, unsigned off) {
    return 0xE5900000u | rn << 16 | rd << 12 | off;
}
inline uint32_t bx(unsigned rm) { return 0xE12FFF10u | rm; }
inline uint32_t blx(unsigned rm) { return 0xE12FFF30u | rm; }
inline uint32_t movr(unsigned rd, unsigned rm) { return 0xE1A00000u | rd << 12 | rm; }
inline uint32_t svc(unsigned n) { return 0xEF000000u | n; }
constexpr unsigned NE = 1, AL = 14;
// --- Thumb ---
inline uint16_t t_adds_imm8(unsigned rd, unsigned imm) { return uint16_t(0x3000 | rd << 8 | imm); }
inline uint16_t t_bx(unsigned rm) { return uint16_t(0x4700 | rm << 3); }

struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kImageSize, 0);
    void w32(uint32_t a, uint32_t v) {
        for (int i = 0; i < 4; i++)
            bytes[a + i] = uint8_t(v >> (8 * i));
    }
    void w16(uint32_t a, uint16_t v) {
        bytes[a] = uint8_t(v);
        bytes[a + 1] = uint8_t(v >> 8);
    }
};

inline Image build() {
    Image m;
    // Sum 1..10 in r0, then return through lr.
    uint32_t a = kLoop;
    m.w32(a, movi(0, 0));
    a += 4;
    m.w32(a, movi(1, 10));
    a += 4;
    const uint32_t loop = a;
    m.w32(a, addr_(0, 0, 1));
    a += 4;
    m.w32(a, subsi(1, 1, 1));
    a += 4;
    m.w32(a, b(NE, a, loop));
    a += 4;
    m.w32(a, bx(14));
    // Jump table: r1 = 10 + r0 for r0 in 0..3, else 99.
    a = kTable;
    const uint32_t def = kTable + 0x48;
    m.w32(a, cmpi(0, 3));
    a += 4;
    m.w32(a, ldrls_pc(0));
    a += 4;
    m.w32(a, b(AL, a, def));
    a += 4;
    for (unsigned k = 0; k < 4; k++)
        m.w32(a + 4 * k, kTable + 0x20 + 8 * k);
    for (unsigned k = 0; k < 4; k++) {
        m.w32(kTable + 0x20 + 8 * k, movi(1, 10 + k));
        m.w32(kTable + 0x24 + 8 * k, bx(14));
    }
    m.w32(def, movi(1, 99));
    m.w32(def + 4, bx(14));
    // Virtual call: r0 = object, vtable slot 2 holds the callee (Thumb: bit 0 set).
    a = kVcall;
    m.w32(a, ldri(2, 0, 0));
    a += 4;
    m.w32(a, ldri(3, 2, 8));
    a += 4;
    m.w32(a, movr(5, 14));
    a += 4;
    m.w32(a, blx(3));
    a += 4;
    m.w32(a, bx(5));
    m.w32(kObj, kVtbl);
    m.w32(kVtbl + 8, kThumbFn | 1);     // Thumb implementation
    m.w32(kVtbl + 12, kArmFn);          // ARM implementation (slot 3)
    m.w16(kThumbFn, t_adds_imm8(1, 5)); // adds r1, #5
    m.w16(kThumbFn + 2, t_bx(14));      // bx lr
    m.w32(kArmFn, addr_(1, 1, 1));      // add r1, r1, r1
    m.w32(kArmFn + 4, bx(14));
    // SVC then continue.
    a = kSvc;
    m.w32(a, movi(0, 7));
    a += 4;
    m.w32(a, svc(0x32));
    a += 4;
    m.w32(a, movi(0, 8));
    a += 4;
    m.w32(a, bx(14));
    return m;
}

} // namespace prog
