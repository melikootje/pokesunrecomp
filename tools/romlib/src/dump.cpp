#include "romlib/dump.h"

#include <cstring>

namespace romlib {
namespace {
uint64_t locateApp(const Source &src) {
    if (src.size() < 0x200)
        throw Error("input too small to be a 3DS dump");
    Bytes magic = src.readBytes(0x100, 4);
    if (std::memcmp(magic.data(), "NCSD", 4) == 0) {
        Ncsd n = Ncsd::parse(src);
        if (!n.partitions[0].present())
            throw Error("ncsd: no application partition");
        return n.partitions[0].offset;
    }
    if (std::memcmp(magic.data(), "NCCH", 4) == 0)
        return 0;
    throw Error("unrecognized container (expected NCSD/CCI or NCCH/CXI)");
}
} // namespace

Dump::Dump(const Source &src) : src_(&src), app_(Ncch::parse(src, locateApp(src))) {}

SubSource Dump::exefsRegion() const {
    if (!app_.exefsSize)
        throw Error("ncch: no ExeFS");
    return SubSource(*src_, app_.base + app_.exefsOff, app_.exefsSize);
}
SubSource Dump::romfsRegion() const {
    if (!app_.romfsSize)
        throw Error("ncch: no RomFS");
    return SubSource(*src_, app_.base + app_.romfsOff, app_.romfsSize);
}
Bytes Dump::exHeaderRaw() const {
    if (!app_.hasExHeader)
        throw Error("ncch: no extended header");
    return src_->readBytes(app_.base + 0x200, 0x400);
}

bool Overlay::exists(const std::string &p) const {
    return (update && update->exists(p)) || (base && base->exists(p));
}
Bytes Overlay::readAll(const std::string &p) const {
    if (update && update->exists(p))
        return update->readAll(p);
    if (base && base->exists(p))
        return base->readAll(p);
    throw Error("overlay: no such file " + p);
}

} // namespace romlib
