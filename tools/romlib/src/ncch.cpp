#include "romlib/ncch.h"

#include <cstring>

namespace romlib {
namespace {
constexpr uint64_t kUnit = 0x200;

std::string cstr(const uint8_t *p, size_t max) {
    size_t n = 0;
    while (n < max && p[n])
        n++;
    return std::string(reinterpret_cast<const char *>(p), n);
}
} // namespace

Ncsd Ncsd::parse(const Source &src) {
    if (src.size() < 0x200)
        throw Error("ncsd: file too small");
    Bytes h = src.readBytes(0, 0x200);
    if (std::memcmp(&h[0x100], "NCSD", 4) != 0)
        throw Error("ncsd: bad magic");
    Ncsd n;
    uint8_t unitShift = h[0x188 + 6];
    if (unitShift > 8)
        throw Error("ncsd: bad media unit");
    n.mediaUnit = uint32_t(0x200u << unitShift);
    n.mediaSize = uint64_t(rd32(&h[0x104])) * n.mediaUnit;
    n.mediaId = rd64(&h[0x108]);
    for (int i = 0; i < 8; i++) {
        uint64_t off = uint64_t(rd32(&h[0x120 + i * 8])) * n.mediaUnit;
        uint64_t sz = uint64_t(rd32(&h[0x124 + i * 8])) * n.mediaUnit;
        if (sz != 0 && (off > src.size() || sz > src.size() - off))
            throw Error("ncsd: partition " + std::to_string(i) + " exceeds file (truncated dump?)");
        n.partitions[i] = {off, sz};
    }
    return n;
}

ExHeader ExHeader::parse(const Bytes &raw) {
    if (raw.size() < 0x400)
        throw Error("exheader: too small");
    ExHeader e;
    e.appTitle = cstr(&raw[0], 8);
    e.compressExefsCode = (raw[0xD] & 1) != 0;
    auto seg = [&](size_t o) { return CodeSegment{rd32(&raw[o]), rd32(&raw[o + 4]), rd32(&raw[o + 8])}; };
    e.text = seg(0x10);
    e.stackSize = rd32(&raw[0x1C]);
    e.ro = seg(0x20);
    e.data = seg(0x30);
    e.bssSize = rd32(&raw[0x3C]);
    e.programId = rd64(&raw[0x200]);
    for (int i = 0; i < 34; i++) {
        std::string s = cstr(&raw[0x250 + i * 8], 8);
        if (!s.empty())
            e.services.push_back(s);
    }
    for (int i = 0; i < 28; i++) {
        uint32_t d = rd32(&raw[0x370 + i * 4]);
        if (d != 0xFFFFFFFFu)
            e.kernelCaps.push_back(d);
    }
    return e;
}

Ncch Ncch::parse(const Source &src, uint64_t base) {
    if (base > src.size() || src.size() - base < 0x200)
        throw Error("ncch: truncated header");
    Bytes h = src.readBytes(base, 0x200);
    if (std::memcmp(&h[0x100], "NCCH", 4) != 0)
        throw Error("ncch: bad magic");
    Ncch n;
    n.base = base;
    n.contentSize = uint64_t(rd32(&h[0x104])) * kUnit;
    n.partitionId = rd64(&h[0x108]);
    n.programId = rd64(&h[0x118]);
    n.productCode = cstr(&h[0x150], 16);
    std::memcpy(n.flags, &h[0x188], 8);
    n.exHeaderSize = rd32(&h[0x180]);
    n.hasExHeader = n.exHeaderSize != 0;
    n.plainOff = uint64_t(rd32(&h[0x190])) * kUnit;
    n.plainSize = uint64_t(rd32(&h[0x194])) * kUnit;
    n.logoOff = uint64_t(rd32(&h[0x198])) * kUnit;
    n.logoSize = uint64_t(rd32(&h[0x19C])) * kUnit;
    n.exefsOff = uint64_t(rd32(&h[0x1A0])) * kUnit;
    n.exefsSize = uint64_t(rd32(&h[0x1A4])) * kUnit;
    n.romfsOff = uint64_t(rd32(&h[0x1B0])) * kUnit;
    n.romfsSize = uint64_t(rd32(&h[0x1B4])) * kUnit;

    if (!n.noCrypto())
        throw Error("ncch: content is encrypted. Only decrypted dumps are supported; see "
                    "docs/byo-dump.md for how to dump a decrypted copy from your own console");
    auto fits = [&](uint64_t off, uint64_t sz, const char *what) {
        if (sz && (off + sz < off || base + off + sz > src.size()))
            throw Error(std::string("ncch: ") + what + " region exceeds file");
    };
    fits(n.exefsOff, n.exefsSize, "exefs");
    fits(n.romfsOff, n.romfsSize, "romfs");
    fits(n.plainOff, n.plainSize, "plain");
    fits(n.logoOff, n.logoSize, "logo");
    return n;
}

ExHeader Ncch::exHeader(const Source &src) const {
    if (!hasExHeader)
        throw Error("ncch: no extended header");
    return ExHeader::parse(src.readBytes(base + 0x200, 0x400));
}

} // namespace romlib
