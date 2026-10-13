#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "pokerecomp/services.h"

namespace pokerecomp {
namespace {

constexpr uint32_t kArchiveRomFs = 0x00000003; // RomFS archive id (SelfNCCH is 0x2345678E)

// Reads a path from a request's static-buffer translate parameter.
std::string readPath(Kernel &k, uint32_t pathType, uint32_t size, uint32_t ptr) {
    std::string out;
    const uint8_t *p = k.guestPtr(ptr);
    if (pathType == 4) { // UTF-16
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            const uint16_t c = uint16_t(p[i] | p[i + 1] << 8);
            if (c == 0)
                break;
            out += c < 0x80 ? char(c) : '?';
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
            r.reply(2, 2, n < 0 ? 0xC8804464u : kresult::Success);
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
        case 0x0808: // Close
        case 0x0809: // Flush
            r.reply(1, 0, kresult::Success);
            break;
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

class FsService : public Service {
  public:
    FsService(Kernel &k, std::shared_ptr<FsBackend> b) : k_(k), b_(std::move(b)) {}
    const char *name() const override { return "fs:USER"; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x08010002: // Initialize(process id)
        case 0x08610042: // InitializeWithSdkVersion(version, process id)
        case 0x08620040: // SetPriority(priority)
            r.reply(1, 0, kresult::Success);
            break;
        case 0x080C00C2: { // OpenArchive(archive id, path type, path size, path)
            const uint32_t id = r.cmd[1], type = r.cmd[2], size = r.cmd[3], ptr = r.cmd[5];
            const std::string path = readPath(k_, type, size, ptr);
            if (id == kArchiveRomFs) {
                const uint32_t h = ++nextArchive_;
                archives_[h] = id;
                r.reply(3, 0, kresult::Success);
                r.cmd[2] = h;
                r.cmd[3] = 0;
            } else {
                std::fprintf(stderr, "[fs:USER] OpenArchive id %08x path '%s' not implemented\n", id,
                             path.c_str());
                r.reply(1, 0, 0xC8804464u); // not found
            }
            break;
        }
        case 0x080201C2: { // OpenFile(transaction, archive handle u64, path type, path size, flags,
                           // attributes, path)
            const uint32_t ah = r.cmd[2], type = r.cmd[4], size = r.cmd[5], ptr = r.cmd[9];
            const std::string path = readPath(k_, type, size, ptr);
            auto it = archives_.find(ah);
            uint64_t fsz;
            if (it == archives_.end() || !b_->romfsFileSize(path, &fsz)) {
                std::fprintf(stderr, "[fs:USER] OpenFile '%s' failed (archive %u)\n", path.c_str(), ah);
                r.reply(1, 0, 0xC8804464u);
                break;
            }
            const Handle h = k_.createSession(std::make_unique<RomFile>(k_, b_, path));
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0x10; // move handle
            r.cmd[3] = h;
            break;
        }
        case 0x08030204: { // OpenFileDirectly(transaction, archive id, path types/sizes, flags, attributes,
                           // paths)
            const uint32_t id = r.cmd[2];
            if (id == kArchiveRomFs ||
                id == 0x2345678E) { // RomFS: the binary path selects the raw RomFS image
                uint64_t sz;
                if (!b_->rawRomfsSize(&sz)) {
                    r.reply(1, 0, 0xC8804464u);
                    break;
                }
                const Handle h = k_.createSession(std::make_unique<RomFile>(k_, b_, "", true));
                r.reply(1, 2, kresult::Success);
                r.cmd[2] = 0x10;
                r.cmd[3] = h;
                break;
            }
            std::fprintf(stderr, "[fs:USER] OpenFileDirectly archive %08x not implemented\n", id);
            r.reply(1, 0, 0xC8804464u);
            break;
        }
        case 0x08160042: // CloseArchive(handle u64)
            r.reply(1, 0, kresult::Success);
            break;
        case 0x08630000: // GetPriority -> 0
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        case 0x08170000: // IsSdmcDetected -> true (the game treats a missing SD card as fatal)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 1;
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
    Kernel &k_;
    std::shared_ptr<FsBackend> b_;
    std::map<uint32_t, uint32_t> archives_;
    uint32_t nextArchive_ = 0;
    std::set<uint32_t> seen_;
};

} // namespace

void registerFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend) {
    kernel.registerService(std::make_unique<FsService>(kernel, std::move(backend)));
}

} // namespace pokerecomp
