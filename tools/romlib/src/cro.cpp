#include "romlib/cro.h"

#include <cstring>

namespace romlib {
namespace {
constexpr uint32_t kHeaderSize = 0x138;

// Header fields after the 0x80-byte hash area: consecutive u32s starting at 0x80 (magic).
enum Hdr : uint32_t {
    Magic = 0x80,
    NameOffset = 0x84,
    NextCro = 0x88,
    PrevCro = 0x8C,
    FileSize = 0x90,
    BssSize = 0x94,
    FixedSize = 0x98,
    CodeOffset = 0xB0,
    CodeSize = 0xB4,
    DataOffset = 0xB8,
    DataSize = 0xBC,
    ModuleNameOffset = 0xC0,
    ModuleNameSize = 0xC4,
    SegTableOffset = 0xC8,
    SegNum = 0xCC,
    ExpNamedOffset = 0xD0,
    ExpNamedNum = 0xD4,
    ExpIndexedOffset = 0xD8,
    ExpIndexedNum = 0xDC,
    ExpStringsOffset = 0xE0,
    ExpStringsSize = 0xE4,
    ExpTreeOffset = 0xE8,
    ExpTreeNum = 0xEC,
    ImpModuleOffset = 0xF0,
    ImpModuleNum = 0xF4,
    ExtRelocOffset = 0xF8,
    ExtRelocNum = 0xFC,
    ImpNamedOffset = 0x100,
    ImpNamedNum = 0x104,
    ImpIndexedOffset = 0x108,
    ImpIndexedNum = 0x10C,
    ImpAnonOffset = 0x110,
    ImpAnonNum = 0x114,
    ImpStringsOffset = 0x118,
    ImpStringsSize = 0x11C,
    StaticAnonOffset = 0x120,
    StaticAnonNum = 0x124,
    IntRelocOffset = 0x128,
    IntRelocNum = 0x12C,
    StaticRelocOffset = 0x130,
    StaticRelocNum = 0x134,
};
} // namespace

std::string Cro::cstring(uint32_t off) const {
    std::string s;
    while (off < data.size() && data[off])
        s += char(data[off++]);
    return s;
}

Cro Cro::parse(const Bytes &file) {
    if (file.size() < kHeaderSize)
        throw Error("cro: file too small");
    Cro c;
    c.data = file;
    if (std::memcmp(&file[Magic], "CRO0", 4) == 0)
        c.isCrs = false;
    else if (std::memcmp(&file[Magic], "CRS0", 4) == 0)
        c.isCrs = true;
    else
        throw Error("cro: bad magic");
    auto F = [&](uint32_t o) { return rd32(&file[o]); };
    auto table = [&](uint32_t offField, uint32_t numField, uint32_t entry, const char *what) {
        const uint32_t off = F(offField), num = F(numField);
        if (num == 0)
            return std::pair<uint32_t, uint32_t>{0, 0};
        if (off > file.size() || uint64_t(num) * entry > file.size() - off)
            throw Error(std::string("cro: ") + what + " table exceeds file");
        return std::pair<uint32_t, uint32_t>{off, num};
    };
    c.nameOffset = F(NameOffset);
    c.fileSize = F(FileSize);
    c.bssSize = F(BssSize);
    c.fixedSize = F(FixedSize);
    c.codeOffset = F(CodeOffset);
    c.codeSize = F(CodeSize);
    c.dataOffset = F(DataOffset);
    c.dataSize = F(DataSize);
    c.moduleNameOffset = F(ModuleNameOffset);
    c.moduleNameSize = F(ModuleNameSize);
    if (c.fileSize > file.size())
        throw Error("cro: header file size exceeds data");

    {
        auto [off, num] = table(SegTableOffset, SegNum, 12, "segment");
        for (uint32_t i = 0; i < num; i++)
            c.segments.push_back(
                {rd32(&file[off + 12 * i]), rd32(&file[off + 12 * i + 4]), rd32(&file[off + 12 * i + 8])});
    }
    auto relocs = [&](uint32_t offField, uint32_t numField, std::vector<CroReloc> &out, const char *what) {
        auto [off, num] = table(offField, numField, 12, what);
        for (uint32_t i = 0; i < num; i++) {
            const uint8_t *p = &file[off + 12 * i];
            out.push_back({rd32(p), p[4], p[5], p[6], p[7], rd32(p + 8)});
        }
    };
    relocs(IntRelocOffset, IntRelocNum, c.internalRelocs, "internal relocation");
    relocs(ExtRelocOffset, ExtRelocNum, c.externalRelocs, "external relocation");
    relocs(StaticRelocOffset, StaticRelocNum, c.staticRelocs, "static relocation");
    {
        auto [off, num] = table(ImpModuleOffset, ImpModuleNum, 20, "import module");
        for (uint32_t i = 0; i < num; i++) {
            const uint8_t *p = &file[off + 20 * i];
            c.importModules.push_back({rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16)});
        }
    }
    auto pairs = [&](uint32_t offField, uint32_t numField, std::vector<CroSymbolPair> &out,
                     const char *what) {
        auto [off, num] = table(offField, numField, 8, what);
        for (uint32_t i = 0; i < num; i++)
            out.push_back({rd32(&file[off + 8 * i]), rd32(&file[off + 8 * i + 4])});
    };
    pairs(ImpNamedOffset, ImpNamedNum, c.namedImports, "named import");
    pairs(ImpIndexedOffset, ImpIndexedNum, c.indexedImports, "indexed import");
    pairs(ImpAnonOffset, ImpAnonNum, c.anonymousImports, "anonymous import");
    pairs(ExpNamedOffset, ExpNamedNum, c.namedExports, "named export");
    pairs(StaticAnonOffset, StaticAnonNum, c.staticAnonymous, "static anonymous");
    {
        auto [off, num] = table(ExpIndexedOffset, ExpIndexedNum, 4, "indexed export");
        for (uint32_t i = 0; i < num; i++)
            c.indexedExports.push_back(rd32(&file[off + 4 * i]));
    }
    return c;
}

} // namespace romlib
