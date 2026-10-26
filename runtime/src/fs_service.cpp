// fs:USER: RomFS (read-only, from the user's dump) plus host-backed writable archives (save data, extdata, SD
// card). Command layouts come from the SDK's own client stubs in the game binary (0x16bf10-0x16cbb4) and are
// listed in docs/save-data.md together with what is verified.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "pokerecomp/save_store.h"
#include "pokerecomp/services.h"

namespace pokerecomp {
namespace {

namespace fs = std::filesystem;
using Err = SaveStore::Err;

constexpr uint32_t kArchiveRomFs = 0x00000003; // RomFS archive id (SelfNCCH is 0x2345678E)
constexpr uint32_t kArchiveSaveData = 4, kArchiveExtSaveData = 6, kArchiveSharedExt = 7,
                   kArchiveSystemSave = 8, kArchiveSdmc = 9, kArchiveSdmcWriteOnly = 10;

// Result codes: description | module(FS = 17) << 10 | summary << 21 | level(Status = 25) << 27.
constexpr uint32_t fsResult(uint32_t desc, uint32_t summary) {
    return desc | 17u << 10 | summary << 21 | 25u << 27;
}
constexpr uint32_t kNotFound = fsResult(100, 4);        // 0xC8804464, also what RomFS lookups return
constexpr uint32_t kExtDataNotFound = fsResult(120, 4); // 0xC8804478
constexpr uint32_t kFileNotFound = fsResult(112, 4);
constexpr uint32_t kPathNotFound = fsResult(113, 4);
constexpr uint32_t kAlreadyExists = fsResult(190, 13); // summary NothingHappened
constexpr uint32_t kDirNotEmpty = fsResult(240, 9);
constexpr uint32_t kNotFormatted = fsResult(340, 8);
constexpr uint32_t kInvalidPath = fsResult(702, 6);
constexpr uint32_t kFsError = fsResult(0x3FF, 1);

uint32_t errToResult(Err e, bool isDir = false) {
    switch (e) {
    case Err::Ok:
        return kresult::Success;
    case Err::NotFound:
        return isDir ? kPathNotFound : kFileNotFound;
    case Err::Exists:
        return kAlreadyExists;
    case Err::NotEmpty:
        return kDirNotEmpty;
    case Err::Invalid:
        return kInvalidPath;
    default:
        return kFsError;
    }
}

// Reads a path from a request's static-buffer translate parameter (UTF-16 or ASCII), as UTF-8.
std::string readPath(Kernel &k, uint32_t pathType, uint32_t size, uint32_t ptr) {
    std::string out;
    if (size > 0x400)
        return out;
    const uint8_t *p = k.guestPtr(ptr);
    auto put = [&](uint32_t c) {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    };
    if (pathType == 4) { // UTF-16
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            const uint16_t c = uint16_t(p[i] | p[i + 1] << 8);
            if (c == 0)
                break;
            put(c);
        }
    } else {
        for (uint32_t i = 0; i < size && p[i]; i++)
            out += char(p[i]);
    }
    return out;
}

// An open RomFS file (its own session).
class RomFile : public Service {
  public:
    RomFile(Kernel &k, std::shared_ptr<FsBackend> b, std::string path, bool raw = false)
        : k_(k), b_(std::move(b)), path_(std::move(path)), raw_(raw) {}
    const char *name() const override { return "fs:File"; }
    void handle(IpcRequest &r) override {
        switch (r.commandId()) {
        case 0x0802: { // Read(offset u64, size, buffer)
            const uint64_t off = uint64_t(r.cmd[1]) | uint64_t(r.cmd[2]) << 32;
            const uint32_t size = r.cmd[3], desc = r.cmd[4], ptr = r.cmd[5];
            const int64_t n = raw_ ? b_->rawRomfsRead(off, size, k_.guestPtr(ptr))
                                   : b_->romfsRead(path_, off, size, k_.guestPtr(ptr));
            if (n < 0)
                std::fprintf(stderr, "[fs:File %s] read of %u bytes at %llu failed\n", path_.c_str(), size,
                             (unsigned long long)off);
            r.reply(2, 2, n < 0 ? kNotFound : kresult::Success);
            r.cmd[2] = n < 0 ? 0 : uint32_t(n);
            r.cmd[3] = desc;
            r.cmd[4] = ptr;
            break;
        }
        case 0x0804: { // GetSize
            uint64_t sz = 0;
            if (raw_)
                b_->rawRomfsSize(&sz);
            else
                b_->romfsFileSize(path_, &sz);
            r.reply(3, 0, kresult::Success);
            r.cmd[2] = uint32_t(sz);
            r.cmd[3] = uint32_t(sz >> 32);
            break;
        }
        case 0x080C: { // OpenLinkFile: another session on the same file
            const Handle h = k_.createSession(std::make_unique<RomFile>(k_, b_, path_, raw_));
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0x10;
            r.cmd[3] = h;
            break;
        }
        case 0x0808: // Close
        case 0x0809: // Flush
        case 0x080A: // SetPriority
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080B: // GetPriority
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        default:
            std::fprintf(stderr, "[fs:File %s] unimplemented command %08x\n", path_.c_str(), r.header());
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    Kernel &k_;
    std::shared_ptr<FsBackend> b_;
    std::string path_;
    bool raw_;
};

// An open file of a writable archive.
class HostFile : public Service {
  public:
    HostFile(Kernel &k, fs::path path, bool writable, uint64_t maxBytes)
        : k_(k), path_(std::move(path)), writable_(writable), max_(maxBytes) {
        f_ = std::fopen(path_.string().c_str(), writable ? "r+b" : "rb");
    }
    ~HostFile() override {
        if (f_)
            std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    const char *name() const override { return "fs:File"; }
    void handle(IpcRequest &r) override {
        switch (r.commandId()) {
        case 0x0802: { // Read(offset u64, size, buffer)
            const uint64_t off = uint64_t(r.cmd[1]) | uint64_t(r.cmd[2]) << 32;
            const uint32_t size = r.cmd[3], desc = r.cmd[4], ptr = r.cmd[5];
            size_t n = 0;
            if (f_ && std::fseek(f_, long(off), SEEK_SET) == 0)
                n = std::fread(k_.guestPtr(ptr), 1, size, f_);
            r.reply(2, 2, f_ ? kresult::Success : kFsError);
            r.cmd[2] = uint32_t(n);
            r.cmd[3] = desc;
            r.cmd[4] = ptr;
            break;
        }
        case 0x0803: { // Write(offset u64, size, write option, buffer)
            const uint64_t off = uint64_t(r.cmd[1]) | uint64_t(r.cmd[2]) << 32;
            const uint32_t size = r.cmd[3], option = r.cmd[4], desc = r.cmd[5], ptr = r.cmd[6];
            size_t n = 0;
            uint32_t res = kresult::Success;
            if (!writable_ || !f_) {
                res = fsResult(0x2D2, 6); // not writable
            } else if (off + size > max_) {
                res = fsResult(0x2D1, 6); // write beyond the size limit
            } else if (std::fseek(f_, long(off), SEEK_SET) == 0) {
                n = std::fwrite(k_.guestPtr(ptr), 1, size, f_);
                if (option & 1)
                    std::fflush(f_);
            }
            r.reply(2, 2, res);
            r.cmd[2] = uint32_t(n);
            r.cmd[3] = desc;
            r.cmd[4] = ptr;
            break;
        }
        case 0x0804: { // GetSize
            uint64_t sz = 0;
            std::error_code ec;
            sz = uint64_t(fs::file_size(path_, ec));
            if (f_)
                std::fflush(f_);
            r.reply(3, 0, kresult::Success);
            r.cmd[2] = uint32_t(sz);
            r.cmd[3] = uint32_t(sz >> 32);
            break;
        }
        case 0x0805: { // SetSize(size u64)
            const uint64_t sz = uint64_t(r.cmd[1]) | uint64_t(r.cmd[2]) << 32;
            std::error_code ec;
            if (!writable_ || sz > max_) {
                r.reply(1, 0, fsResult(0x2D2, 6));
                break;
            }
            if (f_)
                std::fflush(f_);
            fs::resize_file(path_, sz, ec);
            r.reply(1, 0, ec ? kFsError : kresult::Success);
            break;
        }
        case 0x080C: { // OpenLinkFile: another session on the same file
            if (f_)
                std::fflush(f_);
            auto link = std::make_unique<HostFile>(k_, path_, writable_, max_);
            if (!link->ok()) {
                r.reply(1, 0, kFsError);
                break;
            }
            const Handle h = k_.createSession(std::move(link));
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0x10;
            r.cmd[3] = h;
            break;
        }
        case 0x0808: // Close
        case 0x0809: // Flush
            if (f_)
                std::fflush(f_);
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080A:
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080B:
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        default:
            std::fprintf(stderr, "[fs:File %s] unimplemented command %08x\n", path_.string().c_str(),
                         r.header());
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    Kernel &k_;
    fs::path path_;
    bool writable_;
    uint64_t max_;
    std::FILE *f_ = nullptr;
};

// An open directory of a writable archive: entries are returned in 0x228-byte records.
class HostDir : public Service {
  public:
    HostDir(Kernel &k, std::vector<SaveStore::Entry> entries) : k_(k), entries_(std::move(entries)) {}
    const char *name() const override { return "fs:Directory"; }
    void handle(IpcRequest &r) override {
        switch (r.commandId()) {
        case 0x0801: { // Read(entry count, buffer)
            const uint32_t count = r.cmd[1], desc = r.cmd[2], ptr = r.cmd[3];
            uint32_t n = 0;
            for (; n < count && next_ < entries_.size(); n++, next_++)
                encode(entries_[next_], k_.guestPtr(ptr + n * 0x228));
            r.reply(2, 2, kresult::Success);
            r.cmd[2] = n;
            r.cmd[3] = desc;
            r.cmd[4] = ptr;
            break;
        }
        case 0x0802: // Close
            r.reply(1, 0, kresult::Success);
            break;
        default:
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    // FS_DirectoryEntry: UTF-16 name[0x106], 8.3 name[9] + extension[4], 6 flag bytes, u64 size (0x228
    // bytes).
    static void encode(const SaveStore::Entry &e, uint8_t *out) {
        std::memset(out, 0, 0x228);
        size_t i = 0;
        for (unsigned char c : e.name) {
            if (i >= 0x105)
                break;
            const uint16_t w = c;
            std::memcpy(out + 2 * i, &w, 2);
            i++;
        }
        const std::string base = e.name.substr(0, e.name.find('.'));
        for (size_t k = 0; k < 8 && k < base.size(); k++)
            out[0x20C + k] = uint8_t(std::toupper(static_cast<unsigned char>(base[k])));
        const size_t dot = e.name.rfind('.');
        for (size_t k = 0; dot != std::string::npos && k < 3 && dot + 1 + k < e.name.size(); k++)
            out[0x215 + k] = uint8_t(std::toupper(static_cast<unsigned char>(e.name[dot + 1 + k])));
        out[0x219] = 1;
        out[0x21B] = e.isDirectory ? 1 : 0;
        std::memcpy(out + 0x220, &e.size, 8);
    }
    Kernel &k_;
    std::vector<SaveStore::Entry> entries_;
    size_t next_ = 0;
};

class FsService : public Service {
  public:
    FsService(Kernel &k, std::shared_ptr<FsBackend> b, std::shared_ptr<SaveStore> saves)
        : k_(k), b_(std::move(b)), saves_(std::move(saves)) {}
    const char *name() const override { return "fs:USER"; }

    // Failed requests are logged (once per command and result) so a silently failing load shows up in the
    // log.
    void handle(IpcRequest &r) override {
        const uint32_t header = r.header();
        dispatch(r);
        if (int32_t(r.cmd[1]) < 0 && failed_.insert({header, r.cmd[1]}).second)
            std::fprintf(stderr, "[fs:USER] command %08x failed with %08x\n", header, r.cmd[1]);
    }

    void dispatch(IpcRequest &r) {
        switch (r.header()) {
        case 0x08010002: // Initialize(process id)
        case 0x08010042:
        case 0x08610042: // InitializeWithSdkVersion(version, process id)
        case 0x08620040: // SetPriority(priority)
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080C00C2:
            openArchive(r);
            break;
        case 0x080E0080: // CloseArchive(handle u64)
        case 0x08160042:
            archives_.erase(r.cmd[1]);
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080D0144: // ControlArchive(handle u64, action, input size, output size): commit etc. are
                         // no-ops
            r.reply(1, 0, kresult::Success);
            break;
        case 0x08120080: { // GetFreeBytes(handle u64)
            r.reply(3, 0, kresult::Success);
            r.cmd[2] = 0x10000000;
            r.cmd[3] = 0;
            break;
        }
        case 0x080201C2: // OpenFile(transaction, archive u64, path type, path size, flags, attributes, path)
            openFile(r);
            break;
        case 0x08030204: // OpenFileDirectly
            openFileDirectly(r);
            break;
        case 0x08080202: // CreateFile(transaction, archive u64, path type, path size, attributes, size u64,
                         // path)
            fileOp(r, Op::CreateFile);
            break;
        case 0x08040142: // DeleteFile
            fileOp(r, Op::DeleteFile);
            break;
        case 0x08090182: // CreateDirectory
            fileOp(r, Op::CreateDir);
            break;
        case 0x08060142: // DeleteDirectory
            fileOp(r, Op::DeleteDir);
            break;
        case 0x08070142: // DeleteDirectoryRecursively
            fileOp(r, Op::DeleteDirRecursive);
            break;
        case 0x08050244: // RenameFile
        case 0x080A0244: // RenameDirectory
            renameOp(r);
            break;
        case 0x080B0102: // OpenDirectory(archive u64, path type, path size, path)
            openDirectory(r);
            break;
        case 0x084500C2: // GetFormatInfo(archive id, path type, path size, path)
            getFormatInfo(r);
            break;
        case 0x084C0242: // FormatSaveData(id, path type, path size, size, dirs, files, dir buckets, file
                         // buckets, dup)
            formatSaveData(r);
            break;
        case 0x08510242: // CreateExtSaveData(media, id low, id high, ..., dirs, files, size limit, icon size)
            createExtSaveData(r);
            break;
        case 0x08170000: // IsSdmcDetected -> true (the game treats a missing SD card as fatal)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 1;
            break;
        case 0x08630000: // GetPriority -> 0
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        default:
            if (seen_.insert(r.header()).second) {
                std::fprintf(stderr, "[fs:USER] unimplemented command %08x:", r.header());
                for (unsigned i = 1; i <= r.normalParams() + r.translateParams() && i < 12; i++)
                    std::fprintf(stderr, " %08x", r.cmd[i]);
                std::fprintf(stderr, "\n");
            }
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    struct Archive {
        uint32_t id = 0;
        bool rom = false;
        fs::path dir;
    };
    enum class Op { CreateFile, DeleteFile, CreateDir, DeleteDir, DeleteDirRecursive };

    // Key of the host directory that backs an archive, from its id and binary path; empty when unsupported.
    std::string archiveKey(uint32_t id, const uint8_t *path, uint32_t size) const {
        char b[80];
        auto w = [&](unsigned i) {
            uint32_t v = 0;
            if (path && size >= 4 * (i + 1))
                std::memcpy(&v, path + 4 * i, 4);
            return v;
        };
        switch (id) {
        case kArchiveSaveData:
            return "savedata";
        case kArchiveExtSaveData:
            std::snprintf(b, sizeof b, "extdata_%02x_%08x%08x", w(0) & 0xFF, w(2), w(1));
            return b;
        case kArchiveSharedExt:
            std::snprintf(b, sizeof b, "sharedext_%08x%08x", w(1), w(0));
            return b;
        case kArchiveSystemSave:
            std::snprintf(b, sizeof b, "system_%08x%08x", w(1), w(0));
            return b;
        case kArchiveSdmc:
        case kArchiveSdmcWriteOnly:
            return "sdmc";
        default:
            return "";
        }
    }
    fs::path metaPath(const std::string &key) const {
        fs::path d;
        saves_->archiveDir("meta", true, &d);
        return d / (key + ".format");
    }

    void openArchive(IpcRequest &r) {
        const uint32_t id = r.cmd[1], type = r.cmd[2], size = r.cmd[3], ptr = r.cmd[5];
        if (id == kArchiveRomFs) {
            const uint32_t h = ++nextArchive_;
            archives_[h] = Archive{id, true, {}};
            r.reply(3, 0, kresult::Success);
            r.cmd[2] = h;
            r.cmd[3] = 0;
            return;
        }
        const std::string key = saves_ ? archiveKey(id, k_.guestPtr(ptr), type == 2 ? size : 0) : "";
        if (key.empty()) {
            std::fprintf(stderr, "[fs:USER] OpenArchive id %08x not supported\n", id);
            r.reply(1, 0, kNotFound);
            return;
        }
        fs::path dir;
        // Save data must have been formatted; extdata must have been created; the SD card always exists.
        const bool autoCreate = id == kArchiveSdmc || id == kArchiveSdmcWriteOnly;
        const Err e = saves_->archiveDir(key, autoCreate, &dir);
        if (e != Err::Ok || (id == kArchiveSaveData && !SaveStore::isFile(metaPath(key)))) {
            const uint32_t res = id == kArchiveSaveData ? kNotFormatted : kExtDataNotFound;
            std::fprintf(stderr, "[fs:USER] OpenArchive id %u '%s' -> %08x\n", id, key.c_str(), res);
            r.reply(1, 0, res);
            return;
        }
        const uint32_t h = ++nextArchive_;
        archives_[h] = Archive{id, false, dir};
        r.reply(3, 0, kresult::Success);
        r.cmd[2] = h;
        r.cmd[3] = 0;
    }

    // Resolves (archive handle, path) of a request; false after writing the error reply.
    bool target(IpcRequest &r, uint32_t archiveWord, uint32_t typeWord, uint32_t sizeWord, uint32_t ptrWord,
                Archive **arch, fs::path *host, std::string *romPath) {
        auto it = archives_.find(r.cmd[archiveWord]);
        if (it == archives_.end()) {
            r.reply(1, 0, fsResult(101, 4)); // archive not mounted
            return false;
        }
        *arch = &it->second;
        const std::string path = readPath(k_, r.cmd[typeWord], r.cmd[sizeWord], r.cmd[ptrWord]);
        if (romPath)
            *romPath = path;
        if (it->second.rom)
            return true;
        if (SaveStore::resolve(it->second.dir, path, host) != Err::Ok) {
            r.reply(1, 0, kInvalidPath);
            return false;
        }
        return true;
    }

    void openFile(IpcRequest &r) {
        Archive *a;
        fs::path host;
        std::string rom;
        if (!target(r, 2, 4, 5, 9, &a, &host, &rom))
            return;
        const uint32_t flags = r.cmd[6]; // bit 0 read, 1 write, 2 create
        k_.note("OpenFile '" + (a->rom ? rom : host.string()) + "'");
        if (a->rom) {
            uint64_t fsz;
            if (!b_->romfsFileSize(rom, &fsz)) {
                std::fprintf(stderr, "[fs:USER] OpenFile '%s' failed\n", rom.c_str());
                r.reply(1, 0, kNotFound);
                return;
            }
            const Handle h = k_.createSession(std::make_unique<RomFile>(k_, b_, rom));
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0x10;
            r.cmd[3] = h;
            return;
        }
        openHostFile(r, host, flags);
    }

    // Opens a file of a writable archive (shared by OpenFile and OpenFileDirectly).
    void openHostFile(IpcRequest &r, const fs::path &host, uint32_t flags) {
        if (!SaveStore::isFile(host)) {
            if (!(flags & 4)) {
                r.reply(1, 0, kFileNotFound);
                return;
            }
            const Err e = SaveStore::createFile(host, 0, saves_->maxFileBytes());
            if (e != Err::Ok) {
                r.reply(1, 0, errToResult(e));
                return;
            }
        }
        auto f = std::make_unique<HostFile>(k_, host, flags & 2, saves_->maxFileBytes());
        if (!f->ok()) {
            r.reply(1, 0, kFsError);
            return;
        }
        const Handle h = k_.createSession(std::move(f));
        r.reply(1, 2, kresult::Success);
        r.cmd[2] = 0x10;
        r.cmd[3] = h;
    }

    // OpenFileDirectly(transaction, archive id, archive path type/size, file path type/size, flags,
    // attributes, archive path, file path). The low-level archive ids are the same archives as OpenArchive's.
    void openFileDirectly(IpcRequest &r) {
        const uint32_t id = r.cmd[2];
        if (id == kArchiveRomFs || id == 0x2345678E) { // RomFS: the binary path selects the raw RomFS image
            uint64_t sz;
            if (!b_->rawRomfsSize(&sz)) {
                r.reply(1, 0, kNotFound);
                return;
            }
            const Handle h = k_.createSession(std::make_unique<RomFile>(k_, b_, "", true));
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0x10;
            r.cmd[3] = h;
            return;
        }
        uint32_t archiveId = 0;
        switch (id) {
        case 0x2345678A:
            archiveId = kArchiveSaveData;
            break;
        case 0x1234567B:
            archiveId = kArchiveExtSaveData;
            break;
        case 0x1234567C:
            archiveId = kArchiveSharedExt;
            break;
        case 0x1234567D:
            archiveId = kArchiveSystemSave;
            break;
        default:
            break;
        }
        if (!saves_ || !archiveId) {
            std::fprintf(stderr, "[fs:USER] OpenFileDirectly archive %08x not implemented\n", id);
            r.reply(1, 0, kNotFound);
            return;
        }
        const uint32_t apType = r.cmd[3], apSize = r.cmd[4];
        const std::string key = archiveKey(archiveId, k_.guestPtr(r.cmd[10]), apType == 2 ? apSize : 0);
        fs::path dir;
        if (saves_->archiveDir(key, false, &dir) != Err::Ok ||
            (archiveId == kArchiveSaveData && !SaveStore::isFile(metaPath(key)))) {
            r.reply(1, 0, archiveId == kArchiveSaveData ? kNotFormatted : kExtDataNotFound);
            return;
        }
        fs::path host;
        const std::string path = readPath(k_, r.cmd[5], r.cmd[6], r.cmd[12]);
        if (SaveStore::resolve(dir, path, &host) != Err::Ok) {
            r.reply(1, 0, kInvalidPath);
            return;
        }
        {
            static int logged = 0;
            if (logged++ < 40)
                std::fprintf(
                    stderr,
                    "[fs:USER] OpenFileDirectly archive %08x path '%s' flags %u words %08x %08x %08x "
                    "%08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    id, path.c_str(), r.cmd[7], r.cmd[1], r.cmd[2], r.cmd[3], r.cmd[4], r.cmd[5], r.cmd[6],
                    r.cmd[7], r.cmd[8], r.cmd[9], r.cmd[10], r.cmd[11], r.cmd[12]);
        }
        if (r.cmd[3] == 2 && r.cmd[5] == 2) { // binary paths: show them
            uint32_t a[4] = {}, f[5] = {};
            std::memcpy(a, k_.guestPtr(r.cmd[10]), std::min<uint32_t>(r.cmd[4], 16));
            std::memcpy(f, k_.guestPtr(r.cmd[12]), std::min<uint32_t>(r.cmd[6], 20));
            char line[200];
            std::snprintf(line, sizeof line,
                          "[fs:USER]   archive path %08x %08x %08x %08x file path %08x %08x %08x %08x %08x\n",
                          a[0], a[1], a[2], a[3], f[0], f[1], f[2], f[3], f[4]);
            std::fputs(line, stderr);
        }
        openHostFile(r, host, r.cmd[7]);
    }

    void fileOp(IpcRequest &r, Op op) {
        Archive *a;
        fs::path host;
        // CreateFile/CreateDirectory carry extra words before the path, so the path pointer sits further
        // along.
        const uint32_t ptrWord = op == Op::CreateFile ? 10 : op == Op::CreateDir ? 8 : 7;
        if (!target(r, 2, 4, 5, ptrWord, &a, &host, nullptr))
            return;
        if (a->rom) {
            r.reply(1, 0, fsResult(0x2D2, 6)); // read-only archive
            return;
        }
        Err e = Err::Io;
        bool dir = false;
        switch (op) {
        case Op::CreateFile:
            e = SaveStore::createFile(host, uint64_t(r.cmd[7]) | uint64_t(r.cmd[8]) << 32,
                                      saves_->maxFileBytes());
            break;
        case Op::DeleteFile:
            e = SaveStore::deleteFile(host);
            break;
        case Op::CreateDir:
            e = SaveStore::createDirectory(host);
            dir = true;
            break;
        case Op::DeleteDir:
            e = SaveStore::deleteDirectory(host, false);
            dir = true;
            break;
        case Op::DeleteDirRecursive:
            e = SaveStore::deleteDirectory(host, true);
            dir = true;
            break;
        }
        r.reply(1, 0, errToResult(e, dir));
    }

    void renameOp(IpcRequest &r) {
        // (transaction, src archive u64, src type, src size, dst archive u64, dst type, dst size, 2 paths)
        Archive *sa, *da;
        fs::path from, to;
        if (!target(r, 2, 4, 5, 11, &sa, &from, nullptr) || !target(r, 6, 8, 9, 13, &da, &to, nullptr))
            return;
        if (sa->rom || da->rom) {
            r.reply(1, 0, fsResult(0x2D2, 6));
            return;
        }
        r.reply(1, 0, errToResult(SaveStore::rename(from, to)));
    }

    void openDirectory(IpcRequest &r) {
        Archive *a;
        fs::path host;
        if (!target(r, 1, 3, 4, 6, &a, &host, nullptr))
            return;
        if (a->rom) {
            r.reply(1, 0, kNotFound);
            return;
        }
        std::vector<SaveStore::Entry> entries;
        const Err e = SaveStore::list(host, &entries);
        if (e != Err::Ok) {
            r.reply(1, 0, errToResult(e, true));
            return;
        }
        const Handle h = k_.createSession(std::make_unique<HostDir>(k_, std::move(entries)));
        r.reply(1, 2, kresult::Success);
        r.cmd[2] = 0x10;
        r.cmd[3] = h;
    }

    // GetFormatInfo / FormatSaveData: the formatted state and parameters of the save-data archive.
    void getFormatInfo(IpcRequest &r) {
        const uint32_t id = r.cmd[1];
        // words: [1] id, [2] path type, [3] path size, [4] static-buffer descriptor, [5] path pointer
        const std::string key =
            saves_ ? archiveKey(id, k_.guestPtr(r.cmd[5]), r.cmd[2] == 2 ? r.cmd[3] : 0) : "";
        std::ifstream in(key.empty() ? fs::path() : metaPath(key));
        uint32_t info[4] = {};
        if (key.empty() || !in || !(in >> info[0] >> info[1] >> info[2] >> info[3])) {
            std::fprintf(stderr, "[fs:USER] GetFormatInfo id %u -> not formatted\n", id);
            r.reply(1, 0, kNotFormatted);
            return;
        }
        r.reply(5, 0, kresult::Success);
        r.cmd[2] = info[0];        // total size
        r.cmd[3] = info[1];        // directories
        r.cmd[4] = info[2];        // files
        r.cmd[5] = info[3] & 0xFF; // duplicate data
    }

    void formatSaveData(IpcRequest &r) {
        const uint32_t id = r.cmd[1];
        const std::string key = saves_ ? archiveKey(id, nullptr, 0) : "";
        if (key.empty() || id != kArchiveSaveData) {
            r.reply(1, 0, kNotFound);
            return;
        }
        fs::path dir;
        if (saves_->archiveDir(key, true, &dir) != Err::Ok) {
            r.reply(1, 0, kFsError);
            return;
        }
        std::error_code ec; // formatting erases the archive
        for (const auto &e : fs::directory_iterator(dir, ec))
            fs::remove_all(e.path(), ec);
        std::ofstream out(metaPath(key));
        out << r.cmd[4] << ' ' << r.cmd[5] << ' ' << r.cmd[6] << ' ' << (r.cmd[9] & 0xFF) << '\n';
        std::fprintf(stderr, "[fs:USER] FormatSaveData: size %u dirs %u files %u dup %u\n", r.cmd[4],
                     r.cmd[5], r.cmd[6], r.cmd[9] & 0xFF);
        r.reply(1, 0, out ? kresult::Success : kFsError);
    }

    void createExtSaveData(IpcRequest &r) {
        const uint8_t *info = reinterpret_cast<const uint8_t *>(&r.cmd[1]); // media, id low, id high
        const std::string key = saves_ ? archiveKey(kArchiveExtSaveData, info, 12) : "";
        fs::path dir;
        if (key.empty()) {
            r.reply(1, 0, kNotFound);
            return;
        }
        if (saves_->archiveDir(key, false, &dir) == Err::Ok) {
            r.reply(1, 0, kAlreadyExists);
            return;
        }
        bool ok = saves_->archiveDir(key, true, &dir) == Err::Ok;
        if (ok) { // GetFormatInfo(extdata) reports what it was created with: size limit, directories, files
            std::ofstream meta(metaPath(key));
            meta << r.cmd[7] << ' ' << r.cmd[5] << ' ' << r.cmd[6] << " 0\n";
            ok = bool(meta);
        }
        std::fprintf(stderr, "[fs:USER] CreateExtSaveData '%s' dirs %u files %u\n", key.c_str(), r.cmd[5],
                     r.cmd[6]);
        r.reply(1, 0, ok ? kresult::Success : kFsError);
    }

    Kernel &k_;
    std::shared_ptr<FsBackend> b_;
    std::shared_ptr<SaveStore> saves_;
    std::map<uint32_t, Archive> archives_;
    uint32_t nextArchive_ = 0;
    std::set<uint32_t> seen_;
    std::set<std::pair<uint32_t, uint32_t>> failed_;
};

} // namespace

std::unique_ptr<Service> makeFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend,
                                       std::shared_ptr<SaveStore> saves) {
    return std::make_unique<FsService>(kernel, std::move(backend), std::move(saves));
}

void registerFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend, std::shared_ptr<SaveStore> saves) {
    kernel.registerService(makeFsService(kernel, std::move(backend), std::move(saves)));
}

} // namespace pokerecomp
