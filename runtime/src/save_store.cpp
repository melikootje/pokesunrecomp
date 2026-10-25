#include "pokerecomp/save_store.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace pokerecomp {

namespace fs = std::filesystem;
using Err = SaveStore::Err;

namespace {
bool plainComponent(const std::string &c) {
    if (c.empty() || c == "." || c == ".." || c.size() > 255)
        return false;
    for (unsigned char ch : c)
        if (ch == 0 || ch == '\\' || ch == '/' || ch < 0x20)
            return false;
    return true;
}
} // namespace

Err SaveStore::archiveDir(const std::string &key, bool create, fs::path *out) const {
    if (!plainComponent(key) || key.find(':') != std::string::npos)
        return Err::Invalid;
    const fs::path dir = root_ / key;
    std::error_code ec;
    if (create)
        fs::create_directories(dir, ec);
    if (ec)
        return Err::Io;
    if (!fs::is_directory(dir, ec))
        return Err::NotFound;
    *out = dir;
    return Err::Ok;
}

Err SaveStore::resolve(const fs::path &archive, const std::string &guestPath, fs::path *out) {
    fs::path p = archive;
    size_t pos = 0;
    int depth = 0;
    while (pos <= guestPath.size()) {
        const size_t e = guestPath.find('/', pos);
        const std::string comp = guestPath.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!comp.empty() && comp != ".") {
            if (!plainComponent(comp) || comp.find(':') != std::string::npos || ++depth > 16)
                return Err::Invalid;
            p /= comp;
        }
        if (e == std::string::npos)
            break;
        pos = e + 1;
    }
    *out = p;
    return Err::Ok;
}

Err SaveStore::createFile(const fs::path &p, uint64_t size, uint64_t maxBytes) {
    if (size > maxBytes)
        return Err::TooBig;
    std::error_code ec;
    if (fs::exists(p, ec))
        return Err::Exists;
    if (!fs::is_directory(p.parent_path(), ec))
        return Err::NotFound;
    std::ofstream f(p, std::ios::binary);
    if (!f)
        return Err::Io;
    f.close();
    if (size) {
        fs::resize_file(p, size, ec);
        if (ec) {
            fs::remove(p, ec);
            return Err::Io;
        }
    }
    return Err::Ok;
}

Err SaveStore::deleteFile(const fs::path &p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return Err::NotFound;
    return fs::remove(p, ec) ? Err::Ok : Err::Io;
}

Err SaveStore::createDirectory(const fs::path &p) {
    std::error_code ec;
    if (fs::exists(p, ec))
        return Err::Exists;
    if (!fs::is_directory(p.parent_path(), ec))
        return Err::NotFound;
    return fs::create_directory(p, ec) ? Err::Ok : Err::Io;
}

Err SaveStore::deleteDirectory(const fs::path &p, bool recursive) {
    std::error_code ec;
    if (!fs::is_directory(p, ec))
        return Err::NotFound;
    if (recursive)
        return fs::remove_all(p, ec) != static_cast<std::uintmax_t>(-1) && !ec ? Err::Ok : Err::Io;
    if (!fs::is_empty(p, ec))
        return Err::NotEmpty;
    return fs::remove(p, ec) ? Err::Ok : Err::Io;
}

Err SaveStore::rename(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    if (!fs::exists(from, ec))
        return Err::NotFound;
    if (fs::exists(to, ec))
        return Err::Exists;
    fs::rename(from, to, ec);
    return ec ? Err::Io : Err::Ok;
}

Err SaveStore::list(const fs::path &dir, std::vector<Entry> *out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return Err::NotFound;
    out->clear();
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        Entry en;
        en.name = e.path().filename().string();
        en.isDirectory = e.is_directory(ec);
        en.size = en.isDirectory ? 0 : uint64_t(e.file_size(ec));
        out->push_back(en);
    }
    std::sort(out->begin(), out->end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    return ec ? Err::Io : Err::Ok;
}

bool SaveStore::isFile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool SaveStore::isDirectory(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

} // namespace pokerecomp
