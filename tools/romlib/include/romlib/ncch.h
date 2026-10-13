#pragma once
#include <array>
#include <map>
#include <memory>
#include <optional>

#include "romlib/common.h"

namespace romlib {

struct NcsdPartition {
    uint64_t offset = 0; // bytes
    uint64_t size = 0;   // bytes
    bool present() const { return size != 0; }
};

struct Ncsd {
    uint64_t mediaSize = 0; // bytes
    uint64_t mediaId = 0;
    uint32_t mediaUnit = 0x200;
    std::array<NcsdPartition, 8> partitions;
    static Ncsd parse(const Source &src);
};

struct CodeSegment {
    uint32_t address = 0, pages = 0, size = 0;
};

struct ExHeader {
    std::string appTitle;
    bool compressExefsCode = false;
    CodeSegment text, ro, data;
    uint32_t bssSize = 0;
    uint32_t stackSize = 0;
    uint64_t programId = 0;
    std::vector<std::string> services; // service access list
    std::vector<uint32_t> kernelCaps;  // raw non-zero, non-"unused" kernel capability descriptors
    static ExHeader parse(const Bytes &raw);
};

struct Ncch {
    uint64_t base = 0; // absolute offset of this NCCH within the source
    uint64_t contentSize = 0;
    uint64_t partitionId = 0;
    uint64_t programId = 0;
    std::string productCode;
    uint8_t flags[8] = {};
    bool noCrypto() const { return (flags[7] & 0x4) != 0; }
    bool hasExHeader = false;
    uint32_t exHeaderSize = 0;
    // offsets/sizes relative to `base`, in bytes (0 when absent)
    uint64_t plainOff = 0, plainSize = 0, logoOff = 0, logoSize = 0, exefsOff = 0, exefsSize = 0,
             romfsOff = 0, romfsSize = 0;

    // Throws Error if the magic is wrong or the content is encrypted (decrypted dumps only).
    static Ncch parse(const Source &src, uint64_t base);
    ExHeader exHeader(const Source &src) const;
};

} // namespace romlib
