#pragma once
// Parser for the Unicorn-generated reference vectors (see gen_vectors.py).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vec {

constexpr uint32_t kCode = 0x00100000, kWin = 0x20000000, kWinSize = 0x10000;

inline uint8_t memf(uint32_t a) { return uint8_t((a * 2654435761u) >> 24); }

struct Vector {
    std::vector<uint32_t> words;
    uint32_t init[15];
    uint32_t flags = 0, tls = 0, qge = 0, finalQge = 0; // qge = (q << 4) | ge
    uint32_t final_[15];
    uint32_t pc = 0, nzcv = 0, thumb = 0;
    std::vector<std::pair<uint32_t, uint8_t>> diffs;
};

inline std::vector<Vector> load(const std::string &path) {
    std::ifstream f(path);
    std::vector<Vector> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] != 'V')
            continue;
        std::istringstream in(line);
        std::string tok;
        in >> tok;
        Vector v;
        unsigned n;
        in >> std::dec >> n;
        in >> std::hex;
        for (unsigned k = 0; k < n; k++) {
            uint32_t w;
            in >> w;
            v.words.push_back(w);
        }
        in >> tok; // |
        for (auto &r : v.init)
            in >> r;
        in >> v.flags >> v.tls >> v.qge;
        in >> tok; // |
        for (auto &r : v.final_)
            in >> r;
        in >> v.pc >> v.nzcv >> std::dec >> v.thumb >> std::hex >> v.finalQge;
        in >> tok; // |
        unsigned nd;
        in >> std::dec >> nd;
        in >> std::hex;
        for (unsigned k = 0; k < nd; k++) {
            std::string d;
            in >> d;
            const size_t c = d.find(':');
            v.diffs.push_back({uint32_t(std::stoul(d.substr(0, c), nullptr, 16)),
                               uint8_t(std::stoul(d.substr(c + 1), nullptr, 16))});
        }
        out.push_back(v);
    }
    return out;
}

} // namespace vec
