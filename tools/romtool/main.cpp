// romtool: inspect/extract a user-supplied, decrypted 3DS dump. Never ships or fetches game data.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/stat.h>

#include "romlib/cro.h"
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
               "  romtool elf     <dump> <out.elf> [--update UPD]   (local analysis container)\n"
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

// Wraps the decompressed code image in an ELF32-ARM file (one PT_LOAD per segment) so standard
// reverse-engineering tools can load it. Local use only; the output is game data (see policy).
int cmdElf(const Args &a) {
    if (a.pos.size() != 2) {
        usage();
        return 2;
    }
    Loaded l(a);
    ExHeader eh = ExHeader::parse(l.exhRaw);
    struct Seg {
        uint32_t addr, filesz, memsz, flags;
    };
    auto pg = [](uint32_t p) { return p * 0x1000u; };
    // .code holds text, ro, data back to back, each padded to whole pages except the last.
    Seg segs[3] = {{eh.text.address, eh.text.size, pg(eh.text.pages), 5},
                   {eh.ro.address, eh.ro.size, pg(eh.ro.pages), 4},
                   {eh.data.address, eh.data.size, pg(eh.data.pages) + eh.bssSize, 6}};
    const size_t imgOffs[3] = {0, pg(eh.text.pages), size_t(pg(eh.text.pages)) + pg(eh.ro.pages)};
    Bytes elf(0x34 + 3 * 0x20, 0);
    auto p16 = [&](size_t o, uint16_t v) {
        elf[o] = uint8_t(v);
        elf[o + 1] = uint8_t(v >> 8);
    };
    auto p32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; i++)
            elf[o + i] = uint8_t(v >> (8 * i));
    };
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(elf.data(), ident, sizeof ident);
    p16(16, 2);
    p16(18, 40);
    p32(20, 1);
    p32(24, eh.text.address);
    p32(28, 0x34);
    p16(40, 0x34);
    p16(42, 0x20);
    p16(44, 3);
    Bytes body;
    for (int i = 0; i < 3; i++) {
        if (imgOffs[i] + segs[i].filesz > l.code.size())
            throw Error("segment exceeds code image");
        size_t ph = 0x34 + i * 0x20;
        p32(ph, 1);
        p32(ph + 4, uint32_t(elf.size() + body.size()));
        p32(ph + 8, segs[i].addr);
        p32(ph + 12, segs[i].addr);
        p32(ph + 16, segs[i].filesz);
        p32(ph + 20, std::max(segs[i].memsz, segs[i].filesz));
        p32(ph + 24, segs[i].flags);
        p32(ph + 28, 0x1000);
        body.insert(body.end(), l.code.begin() + long(imgOffs[i]),
                    l.code.begin() + long(imgOffs[i] + segs[i].filesz));
        while (body.size() % 16)
            body.push_back(0);
    }
    elf.insert(elf.end(), body.begin(), body.end());
    writeFile(a.pos[1], elf);
    std::printf("wrote %s (%zu bytes); local use only\n", a.pos[1].c_str(), elf.size());
    return 0;
}

// Prints structure and counts of CRO/CRS modules in the RomFS (no payload bytes).
int cmdCro(const Args &a) {
    if (a.pos.empty()) {
        usage();
        return 2;
    }
    FileSource src(a.pos[0]);
    Dump d(src);
    SubSource region = d.romfsRegion();
    Romfs fs = Romfs::parse(region);
    std::map<unsigned, size_t> relTypes[3];
    size_t modules = 0, bad = 0;
    for (const auto &f : fs.listFiles()) {
        const bool cro = f.size() > 4 && f.compare(f.size() - 4, 4, ".cro") == 0;
        const bool crs = f.size() > 4 && f.compare(f.size() - 4, 4, ".crs") == 0;
        if (!cro && !crs)
            continue;
        try {
            Cro c = Cro::parse(fs.readAll(f));
            modules++;
            for (auto &r : c.internalRelocs)
                relTypes[0][r.type]++;
            for (auto &r : c.externalRelocs)
                relTypes[1][r.type]++;
            for (auto &r : c.staticRelocs)
                relTypes[2][r.type]++;
            if (a.pos.size() > 1 && f.find(a.pos[1]) != std::string::npos) {
                std::printf("%s '%s' size=%u code=%u+%u data=%u+%u bss=%u segs=%zu int=%zu ext=%zu "
                            "static=%zu imports=%zu named=%zu indexed=%zu anon=%zu expN=%zu expI=%zu\n",
                            f.c_str(), c.name().c_str(), c.fileSize, c.codeOffset, c.codeSize, c.dataOffset,
                            c.dataSize, c.bssSize, c.segments.size(), c.internalRelocs.size(),
                            c.externalRelocs.size(), c.staticRelocs.size(), c.importModules.size(),
                            c.namedImports.size(), c.indexedImports.size(), c.anonymousImports.size(),
                            c.namedExports.size(), c.indexedExports.size());
                for (size_t k = 0; k < c.segments.size(); k++)
                    std::printf("  seg %zu: offset=%08x size=%08x type=%u\n", k, c.segments[k].offset,
                                c.segments[k].size, c.segments[k].type);
            }
        } catch (const Error &e) {
            bad++;
            std::printf("%s: %s\n", f.c_str(), e.what());
        }
    }
    std::printf("modules parsed=%zu failed=%zu\n", modules, bad);
    static const char *names[] = {"internal", "external", "static"};
    for (int t = 0; t < 3; t++) {
        std::printf("%s relocation types:", names[t]);
        for (auto &kv : relTypes[t])
            std::printf(" %u:%zu", kv.first, kv.second);
        std::printf("\n");
    }
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
        if (cmd == "elf")
            return cmdElf(a);
        if (cmd == "cro")
            return cmdCro(a);
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
