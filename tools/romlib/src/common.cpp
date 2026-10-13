#include "romlib/common.h"

#include <cstring>

namespace romlib {

void MemorySource::read(uint64_t off, void *dst, size_t len) const {
    if (off > data_.size() || len > data_.size() - off)
        throw Error("read past end of buffer");
    if (len)
        std::memcpy(dst, data_.data() + off, len);
}

FileSource::FileSource(const std::string &path) : f_(std::fopen(path.c_str(), "rb")), size_(0) {
    if (!f_)
        throw Error("cannot open " + path);
    if (fseeko(f_, 0, SEEK_END) != 0) {
        std::fclose(f_);
        throw Error("cannot seek " + path);
    }
    off_t end = ftello(f_);
    if (end < 0) {
        std::fclose(f_);
        throw Error("cannot size " + path);
    }
    size_ = uint64_t(end);
}

FileSource::~FileSource() {
    if (f_)
        std::fclose(f_);
}

void FileSource::read(uint64_t off, void *dst, size_t len) const {
    if (off > size_ || len > size_ - off)
        throw Error("read past end of file");
    if (len == 0)
        return;
    if (fseeko(f_, off_t(off), SEEK_SET) != 0 || std::fread(dst, 1, len, f_) != len)
        throw Error("file read failed");
}

SubSource::SubSource(const Source &parent, uint64_t base, uint64_t len)
    : parent_(parent), base_(base), len_(len) {
    if (base > parent.size() || len > parent.size() - base)
        throw Error("region exceeds source");
}

void SubSource::read(uint64_t off, void *dst, size_t len) const {
    if (off > len_ || len > len_ - off)
        throw Error("read past end of region");
    parent_.read(base_ + off, dst, len);
}

} // namespace romlib
