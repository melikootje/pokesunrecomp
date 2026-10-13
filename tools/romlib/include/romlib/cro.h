#pragma once
// CRO0/CRS (static module) parsing. Offsets in a file are relative to its start; "tags" pack a segment index
// in the low 4 bits and an offset within the segment in the upper 28 bits.
#include "romlib/common.h"

namespace romlib {

struct CroSegment {
    uint32_t offset = 0, size = 0, type = 0; // type: 0 text, 1 rodata, 2 data, 3 bss
};

struct CroReloc {
    uint32_t target = 0; // segment tag of the patched word
    uint8_t type = 0;
    uint8_t aux1 = 0, aux2 = 0,
            aux3 = 0;   // meaning depends on the table (batch end/resolved, symbol segment, ...)
    uint32_t value = 0; // addend or symbol/batch offset
};

struct CroImportModule {
    uint32_t nameOffset = 0, indexedOffset = 0, indexedNum = 0, anonymousOffset = 0, anonymousNum = 0;
};

struct CroSymbolPair {
    uint32_t a = 0, b = 0; // (name offset | index | segment tag), relocation-batch offset
};

struct Cro {
    bool isCrs = false; // true for the static module ("CRS0" magic)
    Bytes data;         // the whole file
    uint32_t nameOffset = 0, fileSize = 0, bssSize = 0, fixedSize = 0;
    uint32_t codeOffset = 0, codeSize = 0, dataOffset = 0, dataSize = 0;
    uint32_t moduleNameOffset = 0, moduleNameSize = 0;
    std::vector<CroSegment> segments;
    std::vector<CroReloc> internalRelocs, externalRelocs, staticRelocs;
    std::vector<CroImportModule> importModules;
    std::vector<CroSymbolPair> namedImports, indexedImports, anonymousImports, namedExports, staticAnonymous;
    std::vector<uint32_t> indexedExports; // segment tags

    uint32_t u32(uint32_t off) const { return rd32(&data.at(off)); }
    std::string cstring(uint32_t off) const;
    std::string name() const { return cstring(moduleNameOffset); }
    static Cro parse(const Bytes &file); // throws Error on malformed input
};

} // namespace romlib
