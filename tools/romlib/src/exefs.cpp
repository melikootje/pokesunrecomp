#include "romlib/exefs.h"

#include "romlib/blz.h"

namespace romlib {

Exefs Exefs::parse(const Source &region) {
    if (region.size() < 0x200)
        throw Error("exefs: truncated header");
    Exefs e(region);
    Bytes h = region.readBytes(0, 0x200);
    for (int i = 0; i < 10; i++) {
        const uint8_t *p = &h[i * 16];
        if (p[0] == 0)
            continue;
        size_t n = 0;
        while (n < 8 && p[n])
            n++;
        ExefsEntry en;
        en.name.assign(reinterpret_cast<const char *>(p), n);
        en.offset = rd32(p + 8);
        en.size = rd32(p + 12);
        if (uint64_t(en.offset) + en.size + 0x200 > region.size())
            throw Error("exefs: entry '" + en.name + "' exceeds region");
        e.entries_.push_back(en);
    }
    return e;
}

const ExefsEntry *Exefs::find(const std::string &name) const {
    for (const auto &e : entries_)
        if (e.name == name)
            return &e;
    return nullptr;
}

Bytes Exefs::read(const std::string &name) const {
    const ExefsEntry *e = find(name);
    if (!e)
        throw Error("exefs: no entry '" + name + "'");
    return region_->readBytes(0x200 + uint64_t(e->offset), e->size);
}

Bytes Exefs::code(bool compressed) const {
    Bytes raw = read(".code");
    return compressed ? blzDecompress(raw) : raw;
}

} // namespace romlib
