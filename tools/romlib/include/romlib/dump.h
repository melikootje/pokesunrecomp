#pragma once
#include "romlib/exefs.h"
#include "romlib/romfs.h"

namespace romlib {

// A decrypted cartridge dump (CCI/NCSD) or a bare NCCH/CXI. Owns nothing but the source reference.
class Dump {
  public:
    explicit Dump(const Source &src);
    const Ncch &app() const { return app_; }
    const Source &source() const { return *src_; }
    // Region sources are windows onto the original source: no data is copied.
    SubSource exefsRegion() const;
    SubSource romfsRegion() const;
    Bytes exHeaderRaw() const;

  private:
    const Source *src_;
    Ncch app_;
};

// Applies a user-supplied update NCCH as an overlay: update ExeFS code replaces the base code and
// RomFS files fall back to the base when absent from the update.
struct Overlay {
    const Romfs *base = nullptr;
    const Romfs *update = nullptr;
    bool exists(const std::string &p) const;
    Bytes readAll(const std::string &p) const;
};

} // namespace romlib
