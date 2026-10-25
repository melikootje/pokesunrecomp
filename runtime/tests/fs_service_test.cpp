// fs:USER save data, extdata and file/directory commands, driven directly through the service (no game data).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/kernel.h"
#include "pokerecomp/save_store.h"
#include "pokerecomp/services.h"

using namespace pokerecomp;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace {
constexpr uint32_t fsResult(uint32_t desc, uint32_t summary) {
    return desc | 17u << 10 | summary << 21 | 25u << 27;
}
constexpr uint32_t kNotFormatted = fsResult(340, 8), kExtNotFound = fsResult(120, 4),
                   kFileNotFound = fsResult(112, 4), kAlreadyExists = fsResult(190, 13),
                   kInvalidPath = fsResult(702, 6), kReadOnly = fsResult(0x2D2, 6),
                   kNotEmpty = fsResult(240, 9);
constexpr uint32_t kMem = 0x00200000, kPath = kMem, kBuf = kMem + 0x1000, kDirBuf = kMem + 0x4000;

struct NoRom : FsBackend {
    bool romfsFileSize(const std::string &, uint64_t *) override { return false; }
    int64_t romfsRead(const std::string &, uint64_t, uint32_t, void *) override { return -1; }
};

struct Fx {
    GuestMemory gm;
    Dispatcher disp;
    Kernel kernel{gm, disp};
    fs::path root = fs::temp_directory_path() / "pokerecomp_fs_service_test";
    std::unique_ptr<Service> svc;
    Fx() {
        fs::remove_all(root);
        gm.map(kMem, 0x10000, GuestMemory::Read | GuestMemory::Write);
        svc = makeFsService(kernel, std::make_shared<NoRom>(), std::make_shared<SaveStore>(root, 1 << 20));
    }
    ~Fx() { fs::remove_all(root); }
    uint32_t path(const std::string &p, uint32_t at = kPath) {
        std::memcpy(gm.base() + at, p.c_str(), p.size() + 1);
        return uint32_t(p.size() + 1);
    }
    // Sends a request; returns the 64 reply words.
    struct Reply {
        uint32_t w[64];
        uint32_t result() const { return w[1]; }
    };
    Reply call(Service &s, uint32_t header, std::initializer_list<uint32_t> words) {
        Reply r;
        std::memset(r.w, 0, sizeof r.w);
        r.w[0] = header;
        unsigned i = 1;
        for (uint32_t v : words)
            r.w[i++] = v;
        IpcRequest req;
        req.kernel = &kernel;
        req.cmd = r.w;
        s.handle(req);
        return r;
    }
    Reply fs(uint32_t header, std::initializer_list<uint32_t> words) { return call(*svc, header, words); }
    uint32_t openArchive(uint32_t id, const std::string &bin = "") {
        uint32_t size;
        if (bin.empty())
            size = path("/"), (void)0;
        else {
            std::memcpy(gm.base() + kPath, bin.data(), bin.size());
            size = uint32_t(bin.size());
        }
        const Reply r = fs(0x080C00C2, {id, bin.empty() ? 3u : 2u, size, 0, kPath});
        lastResult = r.result();
        return r.result() == 0 ? r.w[2] : 0;
    }
    uint32_t lastResult = 0;
    Service *session(const Reply &r) { return kernel.sessionService(r.w[3]); }
};
} // namespace

int main() {
    // --- save data: unformatted until FormatSaveData; GetFormatInfo reports what it was formatted with ---
    {
        Fx f;
        f.path("/");
        CHECK(f.fs(0x084500C2, {4, 3, 2, 0, kPath}).result() == kNotFormatted);
        CHECK(f.openArchive(4) == 0 && f.lastResult == kNotFormatted);
        // FormatSaveData(id, path type, path size, size, dirs, files, dir buckets, file buckets, dup)
        CHECK(f.fs(0x084C0242, {4, 3, 2, 512, 1, 1, 0, 0, 1, 0, kPath}).result() == 0);
        const auto info = f.fs(0x084500C2, {4, 3, 2, 0, kPath});
        CHECK(info.result() == 0 && info.w[2] == 512 && info.w[3] == 1 && info.w[4] == 1 && info.w[5] == 1);
        CHECK(f.openArchive(4) != 0);
    }
    // --- files: create, open, write, read, resize, links, delete ---
    {
        Fx f;
        f.fs(0x084C0242, {4, 3, 2, 512, 1, 1, 0, 0, 1, 0, kPath});
        const uint32_t ar = f.openArchive(4);
        CHECK(ar != 0);
        uint32_t n = f.path("/main");
        // CreateFile(transaction, archive u64, type, size, attributes, file size u64, desc, path)
        CHECK(f.fs(0x08080202, {0, ar, 0, 3, n, 0, 16, 0, 0, kPath}).result() == 0);
        CHECK(f.fs(0x08080202, {0, ar, 0, 3, n, 0, 16, 0, 0, kPath}).result() == kAlreadyExists);
        // OpenFile(transaction, archive u64, type, size, flags, attributes, desc, path): flags 3 = read|write
        auto of = f.fs(0x080201C2, {0, ar, 0, 3, n, 3, 0, 0, kPath});
        CHECK(of.result() == 0);
        Service *file = f.session(of);
        CHECK(file != nullptr);
        std::memcpy(f.gm.base() + kBuf, "hello", 5);
        // Write(offset u64, size, option, desc, buffer) -> written
        auto wr = f.call(*file, 0x08030102, {2, 0, 5, 1, 0, kBuf});
        CHECK(wr.result() == 0 && wr.w[2] == 5);
        CHECK(f.call(*file, 0x08040000, {}).w[2] == 16); // GetSize: the file was created with 16 bytes
        std::memset(f.gm.base() + kBuf + 0x100, 0, 16);
        auto rd = f.call(*file, 0x080200C2, {0, 0, 16, 0, kBuf + 0x100});
        CHECK(rd.result() == 0 && rd.w[2] == 16);
        CHECK(std::memcmp(f.gm.base() + kBuf + 0x100 + 2, "hello", 5) == 0 && f.gm.base()[kBuf + 0x100] == 0);
        // SetSize, and a read past the end returns what is there
        CHECK(f.call(*file, 0x08050080, {4, 0}).result() == 0);
        CHECK(f.call(*file, 0x08040000, {}).w[2] == 4);
        rd = f.call(*file, 0x080200C2, {0, 0, 16, 0, kBuf + 0x100});
        CHECK(rd.w[2] == 4);
        // OpenLinkFile gives a second session on the same file
        auto link = f.call(*file, 0x080C0000, {});
        CHECK(link.result() == 0 && f.session(link) && f.session(link) != file);
        CHECK(f.call(*f.session(link), 0x08040000, {}).w[2] == 4);
        // A read-only open refuses writes
        auto ro = f.fs(0x080201C2, {0, ar, 0, 3, n, 1, 0, 0, kPath});
        auto bad = f.call(*f.session(ro), 0x08030102, {0, 0, 1, 0, 0, kBuf});
        CHECK(bad.result() == kReadOnly && bad.w[2] == 0);
        // Opening a missing file fails, with the create flag it is created
        uint32_t m = f.path("/new.bin");
        CHECK(f.fs(0x080201C2, {0, ar, 0, 3, m, 1, 0, 0, kPath}).result() == kFileNotFound);
        CHECK(f.fs(0x080201C2, {0, ar, 0, 3, m, 7, 0, 0, kPath}).result() == 0);
        // Writes beyond the per-file size limit are refused
        auto huge = f.call(*file, 0x08030102, {0, 0, (1u << 20) + 1, 0, 0, kBuf});
        CHECK(huge.result() != 0);
        // DeleteFile(transaction, archive u64, type, size, desc, path)
        f.path("/main");
        CHECK(f.fs(0x08040142, {0, ar, 0, 3, n, 0, kPath}).result() == 0);
        CHECK(f.fs(0x08040142, {0, ar, 0, 3, n, 0, kPath}).result() == kFileNotFound);
    }
    // --- directories ---
    {
        Fx f;
        f.fs(0x084C0242, {4, 3, 2, 512, 1, 1, 0, 0, 1, 0, kPath});
        const uint32_t ar = f.openArchive(4);
        uint32_t n = f.path("/data");
        // CreateDirectory(transaction, archive u64, type, size, attributes, desc, path)
        CHECK(f.fs(0x08090182, {0, ar, 0, 3, n, 0, 0, kPath}).result() == 0);
        CHECK(f.fs(0x08090182, {0, ar, 0, 3, n, 0, 0, kPath}).result() == kAlreadyExists);
        n = f.path("/data/b.bin");
        f.fs(0x08080202, {0, ar, 0, 3, n, 0, 7, 0, 0, kPath});
        n = f.path("/data/a.txt");
        f.fs(0x08080202, {0, ar, 0, 3, n, 0, 0, 0, 0, kPath});
        n = f.path("/data");
        // OpenDirectory(archive u64, type, size, desc, path)
        auto od = f.fs(0x080B0102, {ar, 0, 3, n, 0, kPath});
        CHECK(od.result() == 0);
        Service *dir = f.session(od);
        auto rd = f.call(*dir, 0x08010042, {4, 0, kDirBuf});
        CHECK(rd.result() == 0 && rd.w[2] == 2);
        const uint8_t *e0 = f.gm.base() + kDirBuf, *e1 = e0 + 0x228;
        CHECK(e0[0] == 'a' && e0[2] == '.' && e0[0x21B] == 0); // sorted: a.txt then b.bin
        CHECK(e1[0] == 'b');
        uint64_t sz;
        std::memcpy(&sz, e1 + 0x220, 8);
        CHECK(sz == 7);
        CHECK(f.call(*dir, 0x08010042, {4, 0, kDirBuf}).w[2] == 0); // exhausted
        // non-empty directories need the recursive delete
        CHECK(f.fs(0x08060142, {0, ar, 0, 3, n, 0, kPath}).result() == kNotEmpty);
        CHECK(f.fs(0x08070142, {0, ar, 0, 3, n, 0, kPath}).result() == 0);
        // rename: (transaction, src archive u64, src type, src size, dst archive u64, dst type, dst size, 2
        // paths)
        n = f.path("/one.bin", kPath);
        f.fs(0x08080202, {0, ar, 0, 3, n, 0, 0, 0, 0, kPath});
        const uint32_t n2 = f.path("/two.bin", kPath + 0x100);
        CHECK(f.fs(0x08050244, {0, ar, 0, 3, n, ar, 0, 3, n2, 0, kPath, 0, kPath + 0x100}).result() == 0);
        CHECK(f.fs(0x08050244, {0, ar, 0, 3, n, ar, 0, 3, n2, 0, kPath, 0, kPath + 0x100}).result() ==
              kFileNotFound);
    }
    // --- OpenFileDirectly on the low-level save-data archive id ---
    {
        Fx f;
        // (transaction, id, archive path type, size, file path type, size, flags, attributes, desc, archive
        // path ptr, desc, file path ptr)
        const uint32_t apPtr = kPath + 0x200, fpPtr = kPath + 0x300;
        f.path("/", apPtr);
        const uint32_t fn = f.path("/main", fpPtr);
        CHECK(f.fs(0x08030204, {0, 0x2345678A, 1, 1, 3, fn, 7, 0, 0, apPtr, 0, fpPtr}).result() ==
              kNotFormatted);
        f.fs(0x084C0242, {4, 3, 2, 512, 1, 1, 0, 0, 1, 0, kPath});
        auto of =
            f.fs(0x08030204, {0, 0x2345678A, 1, 1, 3, fn, 7, 0, 0, apPtr, 0, fpPtr}); // read|write|create
        CHECK(of.result() == 0 && f.session(of) != nullptr);
        std::memcpy(f.gm.base() + kBuf, "save!", 5);
        CHECK(f.call(*f.session(of), 0x08030102, {0, 0, 5, 1, 0, kBuf}).w[2] == 5);
        // the file exists in the archive that OpenArchive(4) sees
        const uint32_t ar = f.openArchive(4);
        CHECK(ar != 0);
        const uint32_t n = f.path("/main");
        CHECK(f.fs(0x080201C2, {0, ar, 0, 3, n, 1, 0, 0, kPath}).result() == 0);
        const uint32_t bad = f.path("/../escape", fpPtr);
        CHECK(f.fs(0x08030204, {0, 0x2345678A, 1, 1, 3, bad, 7, 0, 0, apPtr, 0, fpPtr}).result() ==
              kInvalidPath);
        CHECK(f.fs(0x08030204, {0, 0x77777777, 1, 1, 3, fn, 7, 0, 0, apPtr, 0, fpPtr}).result() != 0);
    }
    // --- guest paths cannot leave the archive ---
    {
        Fx f;
        f.fs(0x084C0242, {4, 3, 2, 512, 1, 1, 0, 0, 1, 0, kPath});
        const uint32_t ar = f.openArchive(4);
        for (const char *evil : {"/../x", "/a/../../x", "/a\\b", "/c:/x"}) {
            const uint32_t n = f.path(evil);
            CHECK(f.fs(0x08080202, {0, ar, 0, 3, n, 0, 0, 0, 0, kPath}).result() == kInvalidPath);
        }
        CHECK(!fs::exists(f.root / "x") && !fs::exists(f.root.parent_path() / "x"));
    }
    // --- extdata: missing until created; GetFormatInfo mirrors the creation parameters ---
    {
        Fx f;
        uint8_t bin[12] = {1, 0, 0, 0, 0x50, 0x1b, 0, 0, 0, 0, 0, 0};
        const std::string key(reinterpret_cast<char *>(bin), 12);
        CHECK(f.openArchive(6, key) == 0 && f.lastResult == kExtNotFound);
        // CreateExtSaveData(media, id low, id high, -, dirs, files, size limit lo, hi, icon size, desc, icon
        // ptr)
        CHECK(f.fs(0x08510242, {1, 0x1b50, 0, 0, 15, 682, 0x400000, 0, 0, 0, kPath}).result() == 0);
        CHECK(f.fs(0x08510242, {1, 0x1b50, 0, 0, 15, 682, 0x400000, 0, 0, 0, kPath}).result() ==
              kAlreadyExists);
        const uint32_t ar = f.openArchive(6, key);
        CHECK(ar != 0);
        std::memcpy(f.gm.base() + kPath, bin, 12);
        const auto info = f.fs(0x084500C2, {6, 2, 12, 0, kPath});
        CHECK(info.result() == 0 && info.w[2] == 0x400000 && info.w[3] == 15 && info.w[4] == 682);
        // a different extdata id is a different archive
        uint8_t other[12] = {1, 0, 0, 0, 0x51, 0x1b, 0, 0, 0, 0, 0, 0};
        CHECK(f.openArchive(6, std::string(reinterpret_cast<char *>(other), 12)) == 0);
    }
    // --- SD card exists from the start; read-only RomFS and unsupported archives reject writes/opens ---
    {
        Fx f;
        CHECK(f.openArchive(9) != 0);
        const uint32_t rom = f.openArchive(3);
        CHECK(rom != 0);
        const uint32_t n = f.path("/x");
        CHECK(f.fs(0x08080202, {0, rom, 0, 3, n, 0, 0, 0, 0, kPath}).result() == kReadOnly);
        CHECK(f.openArchive(0x1234) == 0);
        CHECK(f.fs(0x080E0080, {rom, 0}).result() == 0);                                     // CloseArchive
        CHECK(f.fs(0x08080202, {0, rom, 0, 3, n, 0, 0, 0, 0, kPath}).result() != kReadOnly); // handle is gone
    }
    std::printf(g_fail ? "FAIL\n" : "fs_service ok\n");
    return g_fail ? 1 : 0;
}
