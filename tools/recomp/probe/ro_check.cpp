// ro_check: loads the CRS and every CRO from a user-supplied dump through pokerecomp::ro::Loader and audits
// the result. Local analysis only: it reads game data from the dump and prints counts and symbol names, never
// bytes.
//
//   ro_check <dump.cci> [-v]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "pokerecomp/guest_memory.h"
#include "pokerecomp/ro.h"
#include "romlib/dump.h"
#include "romlib/romfs.h"

using namespace pokerecomp;

namespace {
uint32_t rd32(const GuestMemory &gm, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, gm.base() + a, 4);
    return v;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ro_check <dump> [-v]\n");
        return 2;
    }
    const bool verbose = argc > 2 && std::string(argv[2]) == "-v";
    romlib::FileSource src(argv[1]);
    romlib::Dump dump(src);
    romlib::SubSource region = dump.romfsRegion();
    romlib::Romfs fs = romlib::Romfs::parse(region);

    GuestMemory gm;
    if (!gm.ok())
        return 1;
    const int rw = GuestMemory::Read | GuestMemory::Write;
    const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
    auto mapRange = [&](uint32_t a, uint32_t n) {
        const uint32_t lo = a & ~(pg - 1), hi = (a + n + pg - 1) & ~(pg - 1);
        return gm.map(lo, hi - lo, rw);
    };
    // Main-executable address space the CRS describes (zero-filled: only the addresses matter here).
    mapRange(0x100000, 0x6d4000 - 0x100000);
    mapRange(0x04000000, 0x1000);

    ro::Loader ld(gm.base());
    std::string err;
    uint32_t window = 0x00800000, heap = 0x08000000;
    auto place = [&](const std::string &path, bool crs) -> bool {
        const auto bytes = fs.readAll(path);
        const uint32_t size = uint32_t(bytes.size());
        const uint32_t mapped = window;
        window += (size + 0xFFF) & ~0xFFFu;
        mapRange(mapped, size);
        std::memcpy(gm.base() + mapped, bytes.data(), bytes.size());
        if (crs)
            return ld.initializeCrs(mapped, size, &err);
        ro::LoadParams p;
        p.base = mapped;
        p.size = size;
        const uint32_t dataSize = rd32(gm, mapped + 0xBC) + 0x1000,
                       bssSize = rd32(gm, mapped + 0x94) + 0x1000;
        p.dataBuf = heap;
        p.dataSize = dataSize;
        p.bssBuf = heap + ((dataSize + 0xFFF) & ~0xFFFu);
        p.bssSize = bssSize;
        mapRange(p.dataBuf, ((dataSize + 0xFFF) & ~0xFFFu) + bssSize);
        heap += ((dataSize + 0xFFF) & ~0xFFFu) + ((bssSize + 0xFFF) & ~0xFFFu);
        return ld.loadCro(p, &err);
    };

    if (!place("/static.crs", true)) {
        std::printf("CRS failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("CRS: '%s' exports=%zu onUnresolved=%08x\n", ld.crs()->name.c_str(), ld.crs()->exports.size(),
                ld.crs()->onUnresolved);

    size_t loaded = 0, failed = 0;
    for (const auto &f : fs.listFiles()) {
        if (f.size() < 5 || f.compare(f.size() - 4, 4, ".cro") != 0)
            continue;
        if (place(f, false))
            loaded++;
        else {
            failed++;
            std::printf("FAIL %s: %s\n", f.c_str(), err.c_str());
        }
    }
    const ro::Stats &s = ld.stats();
    std::printf("modules loaded=%zu failed=%zu | internal relocs=%" PRIu64 " external applied=%" PRIu64
                " batches resolved=%" PRIu64 " unsupported=%" PRIu64 "\n",
                loaded, failed, s.internalRelocs, s.externalApplied, s.batchesResolved, s.unsupportedRelocs);

    // Audit: every relocated word must land inside a segment of some loaded module (or be the unresolved
    // handler), i.e. the relocation types, segment indices and addends were interpreted consistently.
    auto inSomeSegment = [&](uint32_t v) {
        for (const ro::Module *m : ld.modules())
            for (const ro::Segment &sg : m->segs)
                if (v >= sg.addr && v <= sg.addr + sg.size && (sg.size || v == sg.addr))
                    return true;
        return false;
    };
    size_t checked = 0, outside = 0, unresolvedWords = 0;
    std::set<std::string> unresolvedNames;
    for (const ro::Module *m : ld.modules()) {
        for (uint32_t i = 0; i < m->intReloc.num; i++) {
            const uint32_t e = m->intReloc.addr + i * 12;
            const uint32_t tag = rd32(gm, e), addend = rd32(gm, e + 8);
            const uint8_t sseg = gm.base()[e + 5];
            const ro::Segment &t = m->segs[tag & 0xF];
            const uint32_t at =
                ((t.type == ro::SegType::Data && !m->dataLive) ? t.imageAddr : t.addr) + (tag >> 4);
            checked++;
            if (rd32(gm, at) != m->segs[sseg].addr + addend || !inSomeSegment(rd32(gm, at))) {
                outside++;
                if (verbose && outside <= 12)
                    std::printf("  outside(int) %s tag=%08x symSeg=%u addend=%08x value=%08x\n",
                                m->name.c_str(), tag, sseg, addend, rd32(gm, at));
            }
        }
        for (uint32_t i = 0; i < m->extReloc.num; i++) {
            const uint32_t e = m->extReloc.addr + i * 12;
            const uint32_t tag = rd32(gm, e), addend = rd32(gm, e + 8);
            if (gm.base()[e + 4] != 2)
                continue;
            const ro::Segment &t = m->segs[tag & 0xF];
            const uint32_t at =
                ((t.type == ro::SegType::Data && !m->dataLive) ? t.imageAddr : t.addr) + (tag >> 4);
            const uint32_t v = rd32(gm, at) - addend;
            checked++;
            if (v == ld.crs()->onUnresolved)
                unresolvedWords++;
            else if (!inSomeSegment(v)) {
                outside++;
                if (verbose && outside <= 12)
                    std::printf("  outside(ext) %s tag=%08x type=%u addend=%08x value=%08x\n",
                                m->name.c_str(), tag, gm.base()[e + 4], addend, rd32(gm, at));
            }
        }
        for (uint32_t i = 0; i < m->namedImport.num; i++) {
            const uint32_t e = m->namedImport.addr + i * 8;
            std::string name;
            for (uint32_t a = rd32(gm, e); gm.base()[a] && name.size() < 4096; a++)
                name += char(gm.base()[a]);
            uint32_t addr;
            if (!ld.lookup(name, &addr)) {
                unresolvedNames.insert(m->name + ": " + name.substr(0, 60));
                if (verbose) {
                    const uint32_t b = rd32(gm, e + 4);
                    const uint32_t tag = rd32(gm, b), addend = rd32(gm, b + 8);
                    const ro::Segment &t = m->segs[tag & 0xF];
                    const uint32_t at =
                        ((t.type == ro::SegType::Data && !m->dataLive) ? t.imageAddr : t.addr) + (tag >> 4);
                    std::printf("  unresolved batch: type=%u last=%u resolvedflag=%u word=%08x addend=%08x "
                                "handler=%08x\n",
                                gm.base()[b + 4], gm.base()[b + 5], gm.base()[b + 6], rd32(gm, at), addend,
                                ld.crs()->onUnresolved);
                }
            }
        }
    }
    std::printf("audit: %zu relocated words checked, %zu outside every segment, %zu still at the unresolved "
                "handler\n",
                checked, outside, unresolvedWords);
    std::printf("named imports with no exporter in the loaded set: %zu\n", unresolvedNames.size());
    if (verbose)
        for (const auto &n : unresolvedNames)
            std::printf("  %s\n", n.c_str());
    return failed || outside ? 1 : 0;
}
