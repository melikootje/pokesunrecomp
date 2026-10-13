#pragma once
#include "romlib/ncch.h"

namespace romlib {

struct ExefsEntry {
    std::string name;
    uint32_t offset = 0; // relative to the end of the 0x200-byte header
    uint32_t size = 0;
};

class Exefs {
  public:
    // `region` must be the ExeFS region of an NCCH.
    static Exefs parse(const Source &region);
    const std::vector<ExefsEntry> &entries() const { return entries_; }
    const ExefsEntry *find(const std::string &name) const;
    Bytes read(const std::string &name) const;
    // Returns the (decompressed when requested) ".code" image.
    Bytes code(bool compressed) const;

  private:
    explicit Exefs(const Source &r) : region_(&r) {}
    const Source *region_;
    std::vector<ExefsEntry> entries_;
};

} // namespace romlib
