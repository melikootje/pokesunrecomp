#pragma once
// Host-backed storage for the guest's save data, extdata and SD card archives. Every guest path is confined
// to the archive's directory under `root`: absolute paths, "..", NUL and backslashes are rejected.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pokerecomp {

class SaveStore {
  public:
    enum class Err { Ok, NotFound, Exists, Invalid, NotEmpty, Io, TooBig };

    struct Entry {
        std::string name; // UTF-8
        bool isDirectory = false;
        uint64_t size = 0;
    };

    explicit SaveStore(std::filesystem::path root, uint64_t maxFileBytes = 256ull << 20)
        : root_(std::move(root)), maxFile_(maxFileBytes) {}

    // Directory that holds one archive; `key` is a short identifier such as "extdata_01_0000000000001b50".
    // Returns Invalid for keys that are not plain identifiers.
    Err archiveDir(const std::string &key, bool create, std::filesystem::path *out) const;
    // Maps a guest path ("/a/b.bin") inside an archive to a host path, or Invalid.
    static Err resolve(const std::filesystem::path &archive, const std::string &guestPath,
                       std::filesystem::path *out);

    static Err createFile(const std::filesystem::path &p, uint64_t size, uint64_t maxBytes);
    static Err deleteFile(const std::filesystem::path &p);
    static Err createDirectory(const std::filesystem::path &p);
    static Err deleteDirectory(const std::filesystem::path &p, bool recursive);
    static Err rename(const std::filesystem::path &from, const std::filesystem::path &to);
    static Err list(const std::filesystem::path &dir, std::vector<Entry> *out);
    static bool isFile(const std::filesystem::path &p);
    static bool isDirectory(const std::filesystem::path &p);
    uint64_t maxFileBytes() const { return maxFile_; }

  private:
    std::filesystem::path root_;
    uint64_t maxFile_;
};

} // namespace pokerecomp
