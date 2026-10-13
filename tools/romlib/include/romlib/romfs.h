#pragma once
#include <map>
#include <memory>

#include "romlib/common.h"

namespace romlib {

struct RomfsFile {
    uint64_t offset = 0; // within level-3 file data
    uint64_t size = 0;
};

// RomFS (IVFC level 3). Only metadata tables are loaded; file data is read lazily.
class Romfs {
  public:
    static Romfs parse(const Source &region);
    bool exists(const std::string &path) const { return files_.count(path) != 0; }
    const RomfsFile *find(const std::string &path) const;
    std::vector<std::string> listFiles() const;
    std::vector<std::string> listDirs() const { return dirs_; }
    uint64_t fileSize(const std::string &path) const;
    // Reads len bytes from `path` at `off` (clamped checks: out-of-range throws).
    Bytes read(const std::string &path, uint64_t off, size_t len) const;
    Bytes readAll(const std::string &path) const;

  private:
    const Source *region_ = nullptr;
    uint64_t dataBase_ = 0; // region-relative offset of level3 file data
    std::map<std::string, RomfsFile> files_;
    std::vector<std::string> dirs_;
};

} // namespace romlib
