#include "romlib/romfs.h"

#include <cstring>
#include <set>

namespace romlib {
namespace {
constexpr uint32_t kNone = 0xFFFFFFFFu;

std::string utf16ToUtf8(const uint8_t *p, size_t bytes) {
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t c = rd16(p + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
            uint32_t lo = rd16(p + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}
} // namespace

Romfs Romfs::parse(const Source &region) {
    Bytes ivfc = region.readBytes(0, 0x60);
    if (std::memcmp(&ivfc[0], "IVFC", 4) != 0 || rd32(&ivfc[4]) != 0x10000)
        throw Error("romfs: bad IVFC header");
    uint32_t masterHash = rd32(&ivfc[8]);
    uint32_t l3Block = rd32(&ivfc[0x4C]);
    uint64_t l3Size = rd64(&ivfc[0x44]);
    if (l3Block < 9 || l3Block > 24)
        throw Error("romfs: bad level-3 block size");
    uint64_t align = uint64_t(1) << l3Block;
    uint64_t l3 = (0x60 + uint64_t(masterHash) + align - 1) / align * align;
    if (l3 > region.size() || l3Size > region.size() - l3)
        throw Error("romfs: level 3 exceeds region");
    if (l3Size < 0x28)
        throw Error("romfs: level 3 too small");

    Bytes hdr = region.readBytes(l3, 0x28);
    uint32_t dirOff = rd32(&hdr[0xC]), dirSize = rd32(&hdr[0x10]);
    uint32_t fileOff = rd32(&hdr[0x1C]), fileSize = rd32(&hdr[0x20]);
    uint32_t dataOff = rd32(&hdr[0x24]);
    if (uint64_t(dirOff) + dirSize > l3Size || uint64_t(fileOff) + fileSize > l3Size || dataOff > l3Size)
        throw Error("romfs: metadata out of range");
    Bytes dirs = region.readBytes(l3 + dirOff, dirSize);
    Bytes files = region.readBytes(l3 + fileOff, fileSize);

    Romfs r;
    r.region_ = &region;
    r.dataBase_ = l3 + dataOff;
    r.level3Off_ = l3;
    r.level3Size_ = l3Size;

    std::set<uint32_t> seenDir, seenFile;
    // Iterative walk: (dir entry offset, path prefix)
    std::vector<std::pair<uint32_t, std::string>> stack{{0, ""}};
    while (!stack.empty()) {
        auto [doff, prefix] = stack.back();
        stack.pop_back();
        if (doff + 0x18 > dirs.size() || !seenDir.insert(doff).second)
            throw Error("romfs: bad directory entry");
        uint32_t nameLen = rd32(&dirs[doff + 0x14]);
        if (uint64_t(doff) + 0x18 + nameLen > dirs.size())
            throw Error("romfs: bad directory name");
        std::string path = prefix;
        if (doff != 0) {
            path += "/" + utf16ToUtf8(&dirs[doff + 0x18], nameLen);
            r.dirs_.push_back(path);
        }
        for (uint32_t f = rd32(&dirs[doff + 0xC]); f != kNone;) {
            if (f + 0x20 > files.size() || !seenFile.insert(f).second)
                throw Error("romfs: bad file entry");
            uint32_t fl = rd32(&files[f + 0x1C]);
            if (uint64_t(f) + 0x20 + fl > files.size())
                throw Error("romfs: bad file name");
            RomfsFile rf{rd64(&files[f + 8]), rd64(&files[f + 0x10])};
            if (rf.offset > l3Size - dataOff || rf.size > l3Size - dataOff - rf.offset)
                throw Error("romfs: file data out of range");
            r.files_[path + "/" + utf16ToUtf8(&files[f + 0x20], fl)] = rf;
            f = rd32(&files[f + 4]);
        }
        for (uint32_t c = rd32(&dirs[doff + 8]); c != kNone;) {
            if (c + 0x18 > dirs.size())
                throw Error("romfs: bad child directory");
            stack.push_back({c, path});
            // sibling chain: walk without recursion into siblings' children here
            c = rd32(&dirs[c + 4]);
            if (seenDir.size() > dirs.size())
                throw Error("romfs: directory cycle");
        }
    }
    return r;
}

const RomfsFile *Romfs::find(const std::string &path) const {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second;
}

std::vector<std::string> Romfs::listFiles() const {
    std::vector<std::string> v;
    for (const auto &kv : files_)
        v.push_back(kv.first);
    return v;
}

uint64_t Romfs::fileSize(const std::string &path) const {
    const RomfsFile *f = find(path);
    if (!f)
        throw Error("romfs: no such file " + path);
    return f->size;
}

Bytes Romfs::read(const std::string &path, uint64_t off, size_t len) const {
    const RomfsFile *f = find(path);
    if (!f)
        throw Error("romfs: no such file " + path);
    if (off > f->size || len > f->size - off)
        throw Error("romfs: read past end of " + path);
    return region_->readBytes(dataBase_ + f->offset + off, len);
}

Bytes Romfs::readAll(const std::string &path) const { return read(path, 0, size_t(fileSize(path))); }

} // namespace romlib
