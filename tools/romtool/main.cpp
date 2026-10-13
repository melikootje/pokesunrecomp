// romtool: inspect/extract a user-supplied, decrypted 3DS dump. Never ships or fetches game data.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

#include "romlib/dump.h"
#include "romlib/profile.h"
#include "romlib/sha256.h"

using namespace romlib;

namespace {

void usage() {
    std::fputs("usage:\n"
               "  romtool info    <dump> [--profile P]...\n"
               "  romtool extract <dump> <outdir> [--update UPD] [--profile P]... [--allow-unknown]\n"
               "  romtool profile <dump> [--update UPD]   (prints a profile for your own dump)\n"
               "  romtool ls      <dump>\n"
               "  romtool cat     <dump> <romfs-path>   (writes to stdout)\n",
               stderr);
}

void writeFile(const std::string &path, const Bytes &b) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw Error("cannot write " + path);
    f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
    if (!f)
        throw Error("write failed: " + path);
}

void makeDir(const std::string &p) {
    if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
        throw Error("cannot create " + p);
}

std::string hex64(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

struct Args {
    std::vector<std::string> pos;
    std::vector<std::string> profiles;
    std::string update;
    bool allowUnknown = false;
};

Args parseArgs(int argc, char **argv) {
    Args a;
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        if (s == "--profile" && i + 1 < argc)
            a.profiles.push_back(argv[++i]);
        else if (s == "--update" && i + 1 < argc)
            a.update = argv[++i];
        else if (s == "--allow-unknown")
            a.allowUnknown = true;
        else if (s.rfind("--", 0) == 0)
            throw Error("unknown option " + s);
        else
            a.pos.push_back(s);
    }
    return a;
}

std::vector<TitleProfile> loadProfiles(const Args &a) {
    std::vector<TitleProfile> v;
    for (const auto &p : a.profiles)
        v.push_back(TitleProfile::load(p));
    return v;
}

int cmdInfo(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 2;
    }
    FileSource src(a.pos[0]);
    Dump d(src);
    const Ncch &n = d.app();
    std::printf(
        "product code : %s\nprogram id   : %s\nexefs        : %llu bytes\nromfs        : %llu bytes\n",
        n.productCode.c_str(), hex64(n.programId).c_str(), (unsigned long long)n.exefsSize,
        (unsigned long long)n.romfsSize);
    if (n.hasExHeader) {
        ExHeader eh = n.exHeader(src);
        std::printf("title        : %s\ntext         : 0x%08x (%u pages)\nro           : 0x%08x (%u pages)\n"
                    "data         : 0x%08x (%u pages) bss 0x%x\nstack        : 0x%x\ncompressed   : "
                    "%s\nservices     :",
                    eh.appTitle.c_str(), eh.text.address, eh.text.pages, eh.ro.address, eh.ro.pages,
                    eh.data.address, eh.data.pages, eh.bssSize, eh.stackSize,
                    eh.compressExefsCode ? "yes" : "no");
        for (const auto &s : eh.services)
            std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    auto profiles = loadProfiles(a);
    const TitleProfile *p = selectProfile(profiles, n.programId);
    std::printf("profile      : %s\n", p ? p->name.c_str() : "(none matched)");
    return 0;
}

// Everything extracted from a dump (+ optional update); owns the sources it points into.
struct Loaded {
    FileSource src;
    std::unique_ptr<Dump> dump;
    std::unique_ptr<SubSource> exefsRegion;
    std::unique_ptr<Exefs> exefs;
    std::unique_ptr<FileSource> updSrc;
    std::unique_ptr<Dump> updDump;
    std::unique_ptr<SubSource> updExefsRegion;
    std::unique_ptr<Exefs> updExefs;
    Bytes code, exhRaw;

    explicit Loaded(const Args &a) : src(a.pos[0]) {
        dump = std::make_unique<Dump>(src);
        exefsRegion = std::make_unique<SubSource>(dump->exefsRegion());
        exefs = std::make_unique<Exefs>(Exefs::parse(*exefsRegion));
        exhRaw = dump->exHeaderRaw();
        bool compressed = ExHeader::parse(exhRaw).compressExefsCode;
        // Optional update overlay: its ExeFS/ExHeader supersede the base.
        if (!a.update.empty()) {
            updSrc = std::make_unique<FileSource>(a.update);
            updDump = std::make_unique<Dump>(*updSrc);
            if (updDump->app().programId != dump->app().programId &&
                (updDump->app().programId & ~0xFFFF00000000ull) !=
                    (dump->app().programId & ~0xFFFF00000000ull))
                throw Error("update does not belong to this title (program id mismatch)");
            if (updDump->app().exefsSize) {
                updExefsRegion = std::make_unique<SubSource>(updDump->exefsRegion());
                updExefs = std::make_unique<Exefs>(Exefs::parse(*updExefsRegion));
                if (updExefs->find(".code")) {
                    exhRaw = updDump->exHeaderRaw();
                    compressed = ExHeader::parse(exhRaw).compressExefsCode;
                }
            }
        }
        const Exefs &codeFs = (updExefs && updExefs->find(".code")) ? *updExefs : *exefs;
        code = codeFs.code(compressed);
    }
};

int cmdExtract(const Args &a) {
    if (a.pos.size() != 2) {
        usage();
        return 2;
    }
    Loaded l(a);
    const Ncch &n = l.dump->app();
    auto profiles = loadProfiles(a);
    const TitleProfile *prof = selectProfile(profiles, n.programId);
    if (!prof && !a.allowUnknown) {
        std::fprintf(stderr, "error: no profile matches title id %s; supply --profile or --allow-unknown\n",
                     hex64(n.programId).c_str());
        return 1;
    }
    if (prof)
        verifyAgainstProfile(*prof, l.code, l.exhRaw);

    // `a.pos[0]` is the dump; the output dir is pos[1].
    makeDir(a.pos[1]);
    std::string out = a.pos[1] + "/exefs";
    makeDir(out);
    writeFile(out + "/code.bin", l.code);
    writeFile(out + "/exheader.bin", l.exhRaw);
    for (const char *nm : {"icon", "banner", "logo"})
        if (l.exefs->find(nm))
            writeFile(out + "/" + nm + ".bin", l.exefs->read(nm));
    ExHeader ehOut = ExHeader::parse(l.exhRaw);
    std::ofstream svc(out + "/services.txt");
    for (const auto &s : ehOut.services)
        svc << s << "\n";
    std::printf("wrote %s/code.bin (%zu bytes, sha256 %s)\n", out.c_str(), l.code.size(),
                sha256Hex(l.code.data(), l.code.size()).c_str());
    std::printf("RomFS is accessed lazily (romtool ls/cat); it is not unpacked.\n");
    return 0;
}

// Prints a profile for the user's own dump so later runs can refuse other builds.
int cmdProfile(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 2;
    }
    Loaded l(a);
    std::printf("name = %s\ntitle_id = %s\ncode_size = %zu\ncode_sha256 = %s\nexheader_sha256 = %s\n",
                l.dump->app().productCode.c_str(), hex64(l.dump->app().programId).c_str(), l.code.size(),
                sha256Hex(l.code.data(), l.code.size()).c_str(),
                sha256Hex(l.exhRaw.data(), l.exhRaw.size()).c_str());
    return 0;
}

int cmdLs(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 2;
    }
    FileSource src(a.pos[0]);
    Dump d(src);
    SubSource region = d.romfsRegion();
    Romfs fs = Romfs::parse(region);
    for (const auto &f : fs.listFiles())
        std::printf("%s\t%llu\n", f.c_str(), (unsigned long long)fs.fileSize(f));
    return 0;
}

int cmdCat(const Args &a) {
    if (a.pos.size() != 2) {
        usage();
        return 2;
    }
    FileSource src(a.pos[0]);
    Dump d(src);
    SubSource region = d.romfsRegion();
    Romfs fs = Romfs::parse(region);
    uint64_t size = fs.fileSize(a.pos[1]);
    for (uint64_t off = 0; off < size;) {
        size_t n = size_t(std::min<uint64_t>(size - off, 1 << 20));
        Bytes b = fs.read(a.pos[1], off, n);
        std::fwrite(b.data(), 1, b.size(), stdout);
        off += n;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    try {
        Args a = parseArgs(argc, argv);
        if (cmd == "info")
            return cmdInfo(a);
        if (cmd == "extract")
            return cmdExtract(a);
        if (cmd == "profile")
            return cmdProfile(a);
        if (cmd == "ls")
            return cmdLs(a);
        if (cmd == "cat")
            return cmdCat(a);
        usage();
        return 2;
    } catch (const Error &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
