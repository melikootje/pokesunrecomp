#pragma once
// Synthetic 3DS container fixtures. Everything here is generated from scratch; no game data.
#include <algorithm>
#include <cstring>
#include <map>

#include "romlib/common.h"

namespace fx {
inline uint32_t rd32le(const romlib::Bytes &b, size_t o) { return romlib::rd32(&b[o]); }
using romlib::Bytes;

inline void w32(Bytes &b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; i++)
        b[o + i] = uint8_t(v >> (8 * i));
}
inline void w64(Bytes &b, size_t o, uint64_t v) {
    w32(b, o, uint32_t(v));
    w32(b, o + 4, uint32_t(v >> 32));
}
inline void put(Bytes &b, size_t o, const void *p, size_t n) { std::memcpy(&b[o], p, n); }
inline void putStr(Bytes &b, size_t o, const std::string &s) { put(b, o, s.data(), s.size()); }
inline size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// Encoder for the backward LZ format (test-only; greedy). `prefix` bytes are stored raw.
inline Bytes blzCompress(const Bytes &raw, size_t prefix) {
    struct Tok {
        bool match;
        uint8_t lit;
        size_t len, disp;
    };
    std::vector<Tok> toks;
    size_t dst = raw.size();
    while (dst > prefix) {
        size_t bestLen = 0, bestDisp = 0;
        for (size_t disp = 3; disp <= 0x1002 && dst + disp <= raw.size(); disp++) {
            size_t len = 0;
            // decoder semantics: byte at dst-1-k copies from dst-1-k+disp
            while (len < 18 && len < dst - prefix && raw[dst - 1 - len] == raw[dst - 1 - len + disp])
                len++;
            if (len > bestLen) {
                bestLen = len;
                bestDisp = disp;
            }
        }
        if (bestLen >= 3) {
            toks.push_back({true, 0, bestLen, bestDisp});
            dst -= bestLen;
        } else {
            toks.push_back({false, raw[dst - 1], 0, 0});
            dst -= 1;
        }
    }
    Bytes readOrder; // bytes in the order the decoder reads them (descending addresses)
    for (size_t i = 0; i < toks.size(); i += 8) {
        uint8_t flags = 0;
        size_t n = std::min<size_t>(8, toks.size() - i);
        Bytes group;
        for (size_t k = 0; k < n; k++) {
            const Tok &t = toks[i + k];
            if (t.match) {
                flags |= uint8_t(0x80 >> k);
                size_t d = t.disp - 3;
                group.push_back(uint8_t(((t.len - 3) << 4) | (d >> 8)));
                group.push_back(uint8_t(d & 0xFF));
            } else
                group.push_back(t.lit);
        }
        readOrder.push_back(flags);
        readOrder.insert(readOrder.end(), group.begin(), group.end());
    }
    Bytes out(raw.begin(), raw.begin() + long(prefix));
    out.insert(out.end(), readOrder.rbegin(), readOrder.rend());
    size_t streamLen = readOrder.size();
    size_t size = prefix + streamLen + 8;
    out.resize(size);
    w32(out, size - 8, uint32_t(8u << 24 | (streamLen + 8)));
    w32(out, size - 4, uint32_t(raw.size() - size));
    return out;
}

struct RomfsFileIn {
    std::string path;
    Bytes data;
};

inline Bytes utf16(const std::string &s) {
    Bytes b;
    for (char c : s) {
        b.push_back(uint8_t(c));
        b.push_back(0);
    }
    return b;
}

// Builds a RomFS region (IVFC header + level 3).
inline Bytes buildRomfs(const std::vector<RomfsFileIn> &files) {
    // collect dirs
    std::vector<std::string> dirs{""};
    for (auto &f : files) {
        size_t p = 0;
        while ((p = f.path.find('/', p + 1)) != std::string::npos) {
            std::string d = f.path.substr(0, p);
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end())
                dirs.push_back(d);
        }
    }
    auto parentOf = [](const std::string &p) { return p.substr(0, p.rfind('/')); };
    auto baseOf = [](const std::string &p) { return p.substr(p.rfind('/') + 1); };
    std::vector<size_t> dOff(dirs.size());
    size_t dsz = 0;
    for (size_t i = 0; i < dirs.size(); i++) {
        dOff[i] = dsz;
        dsz += 0x18 + alignUp(i ? utf16(baseOf(dirs[i])).size() : 0, 4);
    }
    std::vector<size_t> fOff(files.size());
    size_t fsz = 0, dataSz = 0;
    std::vector<size_t> dataOff(files.size());
    for (size_t i = 0; i < files.size(); i++) {
        fOff[i] = fsz;
        fsz += 0x20 + alignUp(utf16(baseOf(files[i].path)).size(), 4);
        dataOff[i] = dataSz;
        dataSz += alignUp(files[i].data.size(), 16);
    }
    auto dirIdx = [&](const std::string &d) {
        return size_t(std::find(dirs.begin(), dirs.end(), d) - dirs.begin());
    };
    Bytes dm(dsz, 0), fm(fsz, 0);
    const uint32_t NONE = 0xFFFFFFFFu;
    for (size_t i = 0; i < dirs.size(); i++) {
        w32(dm, dOff[i], i ? uint32_t(dOff[dirIdx(parentOf(dirs[i]))]) : 0);
        w32(dm, dOff[i] + 4, NONE);
        w32(dm, dOff[i] + 8, NONE);
        w32(dm, dOff[i] + 0xC, NONE);
        w32(dm, dOff[i] + 0x10, NONE);
        Bytes nm = i ? utf16(baseOf(dirs[i])) : Bytes{};
        w32(dm, dOff[i] + 0x14, uint32_t(nm.size()));
        if (!nm.empty())
            put(dm, dOff[i] + 0x18, nm.data(), nm.size());
    }
    // sibling/child chains (prepend order is fine)
    for (size_t i = 1; i < dirs.size(); i++) {
        size_t par = dirIdx(parentOf(dirs[i]));
        w32(dm, dOff[i] + 4, rd32le(dm, dOff[par] + 8));
        w32(dm, dOff[par] + 8, uint32_t(dOff[i]));
    }
    for (size_t i = 0; i < files.size(); i++) {
        size_t par = dirIdx(parentOf(files[i].path));
        w32(fm, fOff[i], uint32_t(dOff[par]));
        w32(fm, fOff[i] + 4, rd32le(dm, dOff[par] + 0xC));
        w32(dm, dOff[par] + 0xC, uint32_t(fOff[i]));
        w64(fm, fOff[i] + 8, dataOff[i]);
        w64(fm, fOff[i] + 0x10, files[i].data.size());
        w32(fm, fOff[i] + 0x18, NONE);
        Bytes nm = utf16(baseOf(files[i].path));
        w32(fm, fOff[i] + 0x1C, uint32_t(nm.size()));
        put(fm, fOff[i] + 0x20, nm.data(), nm.size());
    }
    const size_t hashSz = 8;
    size_t hdr = 0x28;
    size_t dhOff = hdr, fhOff = dhOff + hashSz, dmOff = fhOff + hashSz, fmOff = dmOff + dsz,
           dataBase = alignUp(fmOff + fsz, 16);
    size_t l3Size = dataBase + dataSz;
    Bytes l3(l3Size, 0);
    w32(l3, 0, 0x28);
    w32(l3, 4, uint32_t(dhOff));
    w32(l3, 8, hashSz);
    w32(l3, 0xC, uint32_t(dmOff));
    w32(l3, 0x10, uint32_t(dsz));
    w32(l3, 0x14, uint32_t(fhOff));
    w32(l3, 0x18, hashSz);
    w32(l3, 0x1C, uint32_t(fmOff));
    w32(l3, 0x20, uint32_t(fsz));
    w32(l3, 0x24, uint32_t(dataBase));
    put(l3, dmOff, dm.data(), dsz);
    put(l3, fmOff, fm.data(), fsz);
    for (size_t i = 0; i < files.size(); i++)
        if (!files[i].data.empty())
            put(l3, dataBase + dataOff[i], files[i].data.data(), files[i].data.size());

    Bytes r(0x1000 + l3Size, 0);
    putStr(r, 0, "IVFC");
    w32(r, 4, 0x10000);
    w32(r, 8, 0x20); // master hash size
    w32(r, 0x4C, 12);
    w64(r, 0x44, l3Size);
    put(r, 0x1000, l3.data(), l3Size);
    return r;
}

struct NcchSpec {
    uint64_t programId = 0x0004000000ABCD00ull;
    std::string productCode = "CTR-P-TEST";
    Bytes code; // decompressed .code
    bool compress = true;
    std::vector<std::string> services{"srv:", "fs:USER", "gsp::Gpu"};
    std::vector<RomfsFileIn> romfs;
    bool encrypted = false;
    bool withRomfs = true;
};

inline Bytes buildNcch(const NcchSpec &s) {
    Bytes exh(0x800, 0);
    putStr(exh, 0, "TESTAPP");
    exh[0xD] = s.compress ? 1 : 0;
    w32(exh, 0x10, 0x00100000);
    w32(exh, 0x14, uint32_t(alignUp(s.code.size(), 0x1000) / 0x1000));
    w32(exh, 0x18, uint32_t(s.code.size()));
    w32(exh, 0x1C, 0x4000);
    w32(exh, 0x20, 0x00200000);
    w32(exh, 0x24, 1);
    w32(exh, 0x28, 0x100);
    w32(exh, 0x30, 0x00300000);
    w32(exh, 0x34, 1);
    w32(exh, 0x38, 0x100);
    w32(exh, 0x3C, 0x2000);
    w64(exh, 0x200, s.programId);
    for (size_t i = 0; i < s.services.size(); i++)
        putStr(exh, 0x250 + i * 8, s.services[i]);
    for (int i = 0; i < 28; i++)
        w32(exh, 0x370 + i * 4, 0xFFFFFFFFu);
    w32(exh, 0x370, 0xF0000000u | 0x20);

    Bytes codeStored = s.compress ? blzCompress(s.code, 0) : s.code;
    Bytes icon(0x40, 0x11);
    size_t codeOff = 0, iconOff = alignUp(codeStored.size(), 0x200);
    size_t exefsLen = 0x200 + alignUp(iconOff + icon.size(), 0x200);
    Bytes exefs(exefsLen, 0);
    putStr(exefs, 0, ".code");
    w32(exefs, 8, uint32_t(codeOff));
    w32(exefs, 12, uint32_t(codeStored.size()));
    putStr(exefs, 16, "icon");
    w32(exefs, 24, uint32_t(iconOff));
    w32(exefs, 28, uint32_t(icon.size()));
    put(exefs, 0x200 + codeOff, codeStored.data(), codeStored.size());
    put(exefs, 0x200 + iconOff, icon.data(), icon.size());

    Bytes romfs = s.withRomfs ? buildRomfs(s.romfs) : Bytes{};
    size_t exhOff = 0x200, exefsOff = alignUp(exhOff + 0x800, 0x200),
           romfsOff = alignUp(exefsOff + exefs.size(), 0x200);
    size_t total = alignUp(romfsOff + romfs.size(), 0x200);
    Bytes n(total, 0);
    putStr(n, 0x100, "NCCH");
    w32(n, 0x104, uint32_t(total / 0x200));
    w64(n, 0x108, s.programId);
    w64(n, 0x118, s.programId);
    putStr(n, 0x150, s.productCode);
    w32(n, 0x180, 0x400);
    n[0x188 + 7] = s.encrypted ? 0x1 : 0x4;
    w32(n, 0x1A0, uint32_t(exefsOff / 0x200));
    w32(n, 0x1A4, uint32_t(exefs.size() / 0x200));
    if (s.withRomfs) {
        w32(n, 0x1B0, uint32_t(romfsOff / 0x200));
        w32(n, 0x1B4, uint32_t(alignUp(romfs.size(), 0x200) / 0x200));
    }
    put(n, exhOff, exh.data(), exh.size());
    put(n, exefsOff, exefs.data(), exefs.size());
    if (!romfs.empty())
        put(n, romfsOff, romfs.data(), romfs.size());
    return n;
}

// Wraps an NCCH in a minimal NCSD (CCI) with partition 0 at 0x4000.
inline Bytes buildCci(const Bytes &ncch) {
    size_t off = 0x4000;
    Bytes c(off + ncch.size(), 0);
    putStr(c, 0x100, "NCSD");
    w32(c, 0x104, uint32_t(c.size() / 0x200));
    w64(c, 0x108, 0x0004000000ABCD00ull);
    w32(c, 0x120, uint32_t(off / 0x200));
    w32(c, 0x124, uint32_t(ncch.size() / 0x200));
    put(c, off, ncch.data(), ncch.size());
    return c;
}
} // namespace fx
