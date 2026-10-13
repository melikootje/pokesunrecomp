// recomp_stats <code.bin> <funcmap.tsv>: decoder coverage over the ARM-mode functions of a local image.
// Prints only counts and encoding-class masks (no instruction bytes).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "recomp/decode.h"

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), {});
    std::ifstream m(argv[2]);
    std::string line;
    std::getline(m, line);
    const uint32_t base = 0x100000;
    std::map<std::string, unsigned> ops;
    std::map<uint32_t, unsigned> undefClasses;
    unsigned total = 0, undef = 0;
    while (std::getline(m, line)) {
        std::istringstream in(line);
        std::string a, s, mode, th;
        in >> a >> s >> mode >> th;
        if (mode != "arm")
            continue;
        const uint32_t addr = uint32_t(std::stoul(a, nullptr, 16)), size = uint32_t(std::stoul(s));
        for (uint32_t o = addr; o + 4 <= addr + size && o - base + 4 <= code.size(); o += 4) {
            const uint32_t w = uint32_t(code[o - base]) | code[o - base + 1] << 8 | code[o - base + 2] << 16 |
                               uint32_t(code[o - base + 3]) << 24;
            const auto i = recomp::decodeArm(w, o);
            ops[recomp::opName(i.op)]++;
            total++;
            if (i.op == recomp::Op::Undef) {
                undef++;
                undefClasses[w & 0x0FF000F0]++; // bits 27-20 and 7-4 only
            }
        }
    }
    std::printf("words=%u undefined=%u (%.2f%%)\n", total, undef, 100.0 * undef / total);
    std::vector<std::pair<unsigned, uint32_t>> v;
    for (auto &kv : undefClasses)
        v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    unsigned vfp = 0, other = 0;
    for (auto &e : v)
        ((e.second >> 24) >= 0x0C ? vfp : other) += e.first;
    std::printf("coprocessor/VFP-range undefined=%u, other undefined=%u\n", vfp, other);
    unsigned shown = 0;
    for (auto &e : v)
        if ((e.second >> 24) < 0x0C && shown++ < 30)
            std::printf("  other undef class %08x: %u\n", e.second, e.first);
    return 0;
}
