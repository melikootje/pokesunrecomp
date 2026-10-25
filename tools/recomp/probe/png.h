// Minimal PNG writer (uncompressed deflate blocks) for local screenshots. No dependencies.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace probe {

inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline bool writePng(const std::string &path, unsigned w, unsigned h, const std::vector<uint8_t> &rgba) {
    std::vector<uint8_t> raw; // filter byte 0 + RGB rows
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (unsigned y = 0; y < h; y++) {
        raw.push_back(0);
        for (unsigned x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
                raw.push_back(rgba[(size_t(y) * w + x) * 4 + c]);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[pos + i]) % 65521;
            b = (b + a) % 65521;
        }
        pos += n;
    }
    const uint32_t ad = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8)
        z.push_back(uint8_t(ad >> s));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto be32 = [&](std::vector<uint8_t> &v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8)
            v.push_back(uint8_t(x >> s));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(out, uint32_t(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, crc32(td.data(), td.size()));
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

} // namespace probe
