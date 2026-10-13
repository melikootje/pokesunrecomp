#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace romlib {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Bytes = std::vector<uint8_t>;

inline uint32_t rd32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint64_t rd64(const uint8_t *p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

// Random-access source: a file on disk (never read eagerly) or an in-memory buffer (tests).
class Source {
  public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    // Reads exactly len bytes at off or throws Error.
    virtual void read(uint64_t off, void *dst, size_t len) const = 0;

    Bytes readBytes(uint64_t off, size_t len) const {
        Bytes b(len);
        if (len)
            read(off, b.data(), len);
        return b;
    }
};

class MemorySource : public Source {
  public:
    explicit MemorySource(Bytes data) : data_(std::move(data)) {}
    uint64_t size() const override { return data_.size(); }
    void read(uint64_t off, void *dst, size_t len) const override;

  private:
    Bytes data_;
};

class FileSource : public Source {
  public:
    explicit FileSource(const std::string &path);
    ~FileSource() override;
    uint64_t size() const override { return size_; }
    void read(uint64_t off, void *dst, size_t len) const override;

  private:
    std::FILE *f_;
    uint64_t size_;
};

// A window [base, base+len) of another source.
class SubSource : public Source {
  public:
    SubSource(const Source &parent, uint64_t base, uint64_t len);
    uint64_t size() const override { return len_; }
    void read(uint64_t off, void *dst, size_t len) const override;

  private:
    const Source &parent_;
    uint64_t base_, len_;
};

} // namespace romlib
