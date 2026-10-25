// ldr:ro module loader on synthetic CRS/CRO images (built here from scratch; no game data).
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pokerecomp/ro.h"

using namespace pokerecomp::ro;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace {

constexpr uint32_t tag(uint32_t seg, uint32_t off) { return (off << 4) | seg; }

struct Reloc {
    uint32_t tag;
    uint8_t type;
    uint8_t b5; // external: 1 on the last entry of a batch (set by the builder); internal: symbol segment
    uint32_t addend;
};
struct Batch {
    std::vector<Reloc> relocs;
};
struct ImportModule {
    std::string name;
    std::vector<std::pair<uint32_t, Batch>> indexed; // (export index, batch)
    std::vector<std::pair<uint32_t, Batch>> anon;    // (segment tag in the exporter, batch)
};
struct Def {
    std::string name;
    bool crs = false;
    uint32_t textSize = 0x100, roSize = 0x20, dataSize = 0x40, bssSize = 0x30;
    std::vector<std::pair<uint32_t, uint32_t>> absSegs; // CRS only: (address, size) of segments 0..n-1
    std::vector<std::pair<std::string, uint32_t>> namedExports;
    std::vector<uint32_t> indexedExports;
    std::vector<ImportModule> importModules;
    std::vector<std::pair<std::string, Batch>> namedImports;
    std::vector<Reloc> internal;
    uint32_t onUnresolved = 0xFFFFFFFFu;
};

struct Out {
    std::vector<uint8_t> b;
    uint32_t pos() const { return uint32_t(b.size()); }
    void align(uint32_t a) {
        while (b.size() % a)
            b.push_back(0);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++)
            b.push_back(uint8_t(v >> (8 * i)));
    }
    void at32(uint32_t off, uint32_t v) {
        for (int i = 0; i < 4; i++)
            b[off + i] = uint8_t(v >> (8 * i));
    }
};

std::vector<uint8_t> build(const Def &d, uint32_t *dataOffOut = nullptr) {
    Out o;
    o.b.assign(0x138, 0);
    std::memcpy(&o.b[0x80], d.crs ? "CRO0" : "CRO0", 4); // the real CRS also carries CRO0
    const uint32_t nseg = d.crs ? uint32_t(d.absSegs.size()) : 5;
    const uint32_t segTab = o.pos();
    o.b.resize(o.b.size() + 12 * nseg, 0);
    // strings
    auto str = [&](const std::string &s) {
        const uint32_t p = o.pos();
        for (char c : s)
            o.b.push_back(uint8_t(c));
        o.b.push_back(0);
        return p;
    };
    const uint32_t modName = str(d.name);
    std::vector<uint32_t> expNames, impModNames, impNames;
    for (auto &e : d.namedExports)
        expNames.push_back(str(e.first));
    for (auto &m : d.importModules)
        impModNames.push_back(str(m.name));
    for (auto &n : d.namedImports)
        impNames.push_back(str(n.first));
    o.align(4);
    const uint32_t expTab = o.pos();
    for (size_t i = 0; i < d.namedExports.size(); i++) {
        o.u32(expNames[i]);
        o.u32(d.namedExports[i].second);
    }
    const uint32_t expIdx = o.pos();
    for (uint32_t t : d.indexedExports)
        o.u32(t);

    // external relocations: collect all batches, remember their offsets
    std::vector<Reloc> ext;
    auto addBatch = [&](const Batch &b) {
        const uint32_t idx = uint32_t(ext.size());
        for (size_t i = 0; i < b.relocs.size(); i++) {
            Reloc r = b.relocs[i];
            r.b5 = i + 1 == b.relocs.size() ? 1 : 0;
            ext.push_back(r);
        }
        return idx;
    };
    std::vector<uint32_t> namedBatch;
    for (auto &n : d.namedImports)
        namedBatch.push_back(addBatch(n.second));
    struct ModBatches {
        std::vector<std::pair<uint32_t, uint32_t>> idx, anon;
    };
    std::vector<ModBatches> mb(d.importModules.size());
    for (size_t i = 0; i < d.importModules.size(); i++) {
        for (auto &x : d.importModules[i].indexed)
            mb[i].idx.push_back({x.first, addBatch(x.second)});
        for (auto &x : d.importModules[i].anon)
            mb[i].anon.push_back({x.first, addBatch(x.second)});
    }
    // tables (batch offsets are filled once the external relocation table position is known)
    const uint32_t impModTab = o.pos();
    o.b.resize(o.b.size() + 20 * d.importModules.size(), 0);
    const uint32_t impNamedTab = o.pos();
    o.b.resize(o.b.size() + 8 * d.namedImports.size(), 0);
    std::vector<uint32_t> idxTabs(d.importModules.size()), anonTabs(d.importModules.size());
    uint32_t idxCount = 0, anonCount = 0;
    for (size_t i = 0; i < d.importModules.size(); i++) {
        idxTabs[i] = o.pos();
        o.b.resize(o.b.size() + 8 * mb[i].idx.size(), 0);
        idxCount += uint32_t(mb[i].idx.size());
    }
    for (size_t i = 0; i < d.importModules.size(); i++) {
        anonTabs[i] = o.pos();
        o.b.resize(o.b.size() + 8 * mb[i].anon.size(), 0);
        anonCount += uint32_t(mb[i].anon.size());
    }
    o.align(4);
    const uint32_t extTab = o.pos();
    for (auto &r : ext) {
        o.u32(r.tag);
        o.u32(uint32_t(r.type) | uint32_t(r.b5) << 8);
        o.u32(r.addend);
    }
    const uint32_t intTab = o.pos();
    for (auto &r : d.internal) {
        o.u32(r.tag);
        o.u32(uint32_t(r.type) | uint32_t(r.b5) << 8);
        o.u32(r.addend);
    }
    for (size_t i = 0; i < d.namedImports.size(); i++) {
        o.at32(impNamedTab + 8 * i, impNames[i]);
        o.at32(impNamedTab + 8 * i + 4, extTab + 12 * namedBatch[i]);
    }
    for (size_t i = 0; i < d.importModules.size(); i++) {
        o.at32(impModTab + 20 * i, impModNames[i]);
        o.at32(impModTab + 20 * i + 4, idxTabs[i]);
        o.at32(impModTab + 20 * i + 8, uint32_t(mb[i].idx.size()));
        o.at32(impModTab + 20 * i + 12, anonTabs[i]);
        o.at32(impModTab + 20 * i + 16, uint32_t(mb[i].anon.size()));
        for (size_t k = 0; k < mb[i].idx.size(); k++) {
            o.at32(idxTabs[i] + 8 * k, mb[i].idx[k].first);
            o.at32(idxTabs[i] + 8 * k + 4, extTab + 12 * mb[i].idx[k].second);
        }
        for (size_t k = 0; k < mb[i].anon.size(); k++) {
            o.at32(anonTabs[i] + 8 * k, mb[i].anon[k].first);
            o.at32(anonTabs[i] + 8 * k + 4, extTab + 12 * mb[i].anon[k].second);
        }
    }
    // segments
    uint32_t textOff = 0, roOff = 0, dataOff = 0;
    if (!d.crs) {
        o.align(0x100);
        textOff = o.pos();
        o.b.resize(o.b.size() + d.textSize, 0);
        o.align(0x100);
        roOff = o.pos();
        o.b.resize(o.b.size() + d.roSize, 0);
        o.align(0x100);
        dataOff = o.pos();
        o.b.resize(o.b.size() + d.dataSize, 0);
        o.align(0x1000);
        const uint32_t segs[5][3] = {{textOff, d.textSize, 0},
                                     {roOff, d.roSize, 1},
                                     {dataOff, d.dataSize, 2},
                                     {0, d.bssSize, 3},
                                     {0, 0, 0}};
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 3; k++)
                o.at32(segTab + 12 * i + 4 * k, segs[i][k]);
    } else {
        o.align(0x1000);
        for (uint32_t i = 0; i < nseg; i++) {
            o.at32(segTab + 12 * i, d.absSegs[i].first);
            o.at32(segTab + 12 * i + 4, d.absSegs[i].second);
            o.at32(segTab + 12 * i + 8, i == 2 ? 2 : (i == 1 ? 1 : 0));
        }
    }
    if (dataOffOut)
        *dataOffOut = dataOff;
    o.at32(0x90, o.pos());
    o.at32(0x84, modName);
    o.at32(0xAC, d.onUnresolved);
    o.at32(0xA0, 0xFFFFFFFFu);
    o.at32(0xA4, 0xFFFFFFFFu);
    o.at32(0xA8, 0xFFFFFFFFu);
    o.at32(0xB0, textOff);
    o.at32(0xB4, d.textSize);
    o.at32(0xB8, dataOff);
    o.at32(0xBC, d.dataSize);
    o.at32(0xC0, modName);
    o.at32(0xC4, uint32_t(d.name.size() + 1));
    o.at32(0xC8, segTab);
    o.at32(0xCC, nseg);
    o.at32(0xD0, d.namedExports.empty() ? 0 : expTab);
    o.at32(0xD4, uint32_t(d.namedExports.size()));
    o.at32(0xD8, d.indexedExports.empty() ? 0 : expIdx);
    o.at32(0xDC, uint32_t(d.indexedExports.size()));
    o.at32(0xF0, d.importModules.empty() ? 0 : impModTab);
    o.at32(0xF4, uint32_t(d.importModules.size()));
    o.at32(0xF8, ext.empty() ? 0 : extTab);
    o.at32(0xFC, uint32_t(ext.size()));
    o.at32(0x100, d.namedImports.empty() ? 0 : impNamedTab);
    o.at32(0x104, uint32_t(d.namedImports.size()));
    o.at32(0x108, idxCount ? idxTabs[0] : 0);
    o.at32(0x10C, idxCount);
    o.at32(0x110, anonCount ? anonTabs[0] : 0);
    o.at32(0x114, anonCount);
    o.at32(0x128, d.internal.empty() ? 0 : intTab);
    o.at32(0x12C, uint32_t(d.internal.size()));
    return o.b;
}

constexpr uint32_t kMemSize = 16u << 20;
constexpr uint32_t kExeText = 0x100000, kExeData = 0x200000, kCrsBase = 0x400000, kA = 0x500000,
                   kB = 0x580000, kDataA = 0x800000, kBssA = 0x801000, kDataB = 0x802000, kBssB = 0x803000;

struct Fx {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kMemSize, 0);
    Loader ld{mem.data()};
    uint32_t w(uint32_t a) const {
        uint32_t v;
        std::memcpy(&v, &mem[a], 4);
        return v;
    }
    void put(uint32_t base, const std::vector<uint8_t> &img) {
        std::memcpy(&mem[base], img.data(), img.size());
    }
};

Def crsDef() {
    Def c;
    c.name = "|static|";
    c.crs = true;
    c.absSegs = {{kExeText, 0x1000}, {0x180000, 0x100}, {kExeData, 0x1000}};
    c.namedExports = {{"exeFunc", tag(0, 0x10)}, {"exeData", tag(2, 0x20)}};
    c.onUnresolved = tag(0, 0x40);
    // the main executable imports ModA's function at text+0x20 into a word of its .data
    ImportModule im;
    im.name = "ModA";
    Batch b;
    b.relocs = {{tag(2, 0x100), 2, 0, 8}};
    im.anon.push_back({tag(0, 0x20), b});
    c.importModules.push_back(im);
    return c;
}

} // namespace

int main() {
    std::string err;

    // --- CRS, then ModA with auto-link ---
    {
        Fx f;
        f.put(kCrsBase, build(crsDef()));
        CHECK(f.ld.initializeCrs(kCrsBase, 0x1000, &err));
        CHECK(f.ld.crs() && f.ld.crs()->name == "|static|");
        CHECK(f.ld.crs()->onUnresolved == kExeText + 0x40);
        uint32_t a = 0;
        CHECK(f.ld.lookup("exeFunc", &a) && a == kExeText + 0x10);
        CHECK(f.ld.lookup("exeData", &a) && a == kExeData + 0x20);
        CHECK(!f.ld.lookup("nothing", &a));

        Def da;
        da.name = "ModA";
        da.namedExports = {{"modAFunc", tag(0, 0x20)}};
        da.indexedExports = {tag(0, 0x30)};
        // data word 0 -> text+0x10 (internal), data word 4 -> data+0x8 (internal, target is the data buffer)
        da.internal = {{tag(2, 0), 2, 0, 0x10}, {tag(2, 4), 2, 2, 0x8}};
        da.namedImports = {{"exeFunc", Batch{{{tag(2, 8), 2, 0, 4}}}},
                           {"missing", Batch{{{tag(2, 12), 2, 0, 1}}}}};
        ImportModule im;
        im.name = "|static|";
        im.anon.push_back({tag(2, 0x20), Batch{{{tag(2, 16), 2, 0, 0}, {tag(0, 8), 2, 0, 0x100}}}});
        da.importModules.push_back(im);
        uint32_t dataOff = 0;
        f.put(kA, build(da, &dataOff));

        LoadParams p;
        p.base = kA;
        p.size = 0x2000;
        p.dataBuf = kDataA;
        p.dataSize = 0x40;
        p.bssBuf = kBssA;
        p.bssSize = 0x30;
        CHECK(f.ld.loadCro(p, &err));
        const Module *m = f.ld.find(kA);
        CHECK(m && m->name == "ModA");
        CHECK(m->segs.size() == 5);
        CHECK(m->segs[0].addr > kA && m->segs[0].addr < kA + 0x2000); // text lives inside the image
        CHECK(m->segs[2].addr == kDataA && m->segs[2].imageAddr == kA + dataOff);
        CHECK(m->segs[3].addr == kBssA);
        // The guest-visible segment table now holds real addresses.
        uint32_t segTab = f.w(kA + 0xC8);
        CHECK(segTab > kA && f.w(segTab + 12 * 2) == kDataA && f.w(segTab + 12 * 3) == kBssA);
        CHECK(f.w(kA + 0x84) > kA); // module name offset rebased
        // Internal relocations went into the image's data (the guest copies it to the data buffer
        // afterwards).
        CHECK(f.w(kA + dataOff + 0) == m->segs[0].addr + 0x10);
        CHECK(f.w(kA + dataOff + 4) == kDataA + 0x8); // symbol segment 2 -> the data buffer
        // Named import from the CRS, anonymous import from the CRS, unresolved import.
        CHECK(f.w(kA + dataOff + 8) == kExeText + 0x10 + 4);
        CHECK(f.w(kA + dataOff + 12) == f.ld.crs()->onUnresolved + 1);
        CHECK(f.w(kA + dataOff + 16) == kExeData + 0x20);
        // The main executable's import of ModA was patched in place (live memory, not the image).
        CHECK(f.w(kExeData + 0x100) == m->segs[0].addr + 0x20 + 8);
        CHECK(f.ld.lookup("modAFunc", &a) && a == m->segs[0].addr + 0x20);
        // Module list in the headers.
        CHECK(f.w(kCrsBase + 0x88) == kA && f.w(kA + 0x8C) == kCrsBase && f.w(kA + 0x88) == 0);
        CHECK(f.ld.stats().internalRelocs == 2);

        // The guest copies .data out; from now on patches to ModA's data go to the live buffer.
        std::memcpy(&f.mem[kDataA], &f.mem[kA + dataOff], 0x40);
        f.ld.markDataLive();

        // ModB imports ModA's indexed export and refers to ModA by name.
        Def db;
        db.name = "ModB";
        ImportModule imb;
        imb.name = "ModA";
        imb.indexed.push_back({0, Batch{{{tag(2, 0), 2, 0, 2}}}});
        db.importModules.push_back(imb);
        db.namedImports = {{"modAFunc", Batch{{{tag(2, 4), 2, 0, 0}}}}};
        uint32_t dataOffB = 0;
        f.put(kB, build(db, &dataOffB));
        LoadParams pb = p;
        pb.base = kB;
        pb.dataBuf = kDataB;
        pb.bssBuf = kBssB;
        CHECK(f.ld.loadCro(pb, &err));
        CHECK(f.w(kB + dataOffB + 0) == f.ld.find(kA)->segs[0].addr + 0x30 + 2);
        CHECK(f.w(kB + dataOffB + 4) == f.ld.find(kA)->segs[0].addr + 0x20);
        CHECK(f.w(kB + 0x8C) == kA && f.w(kA + 0x88) == kB);

        // Unloading ModA sends the main executable's and ModB's imports back to the unresolved handler.
        CHECK(f.ld.unloadCro(kA, &err));
        CHECK(!f.ld.find(kA));
        CHECK(f.w(kExeData + 0x100) == f.ld.crs()->onUnresolved + 8);
        CHECK(f.w(kB + dataOffB + 0) == f.ld.crs()->onUnresolved + 2);
        CHECK(f.w(kCrsBase + 0x88) == kB && f.w(kB + 0x8C) == kCrsBase);
        // ModA can be loaded again (at the same place) and relinks.
        f.put(kA, build(da, &dataOff));
        CHECK(f.ld.loadCro(p, &err));
        CHECK(f.w(kExeData + 0x100) == f.ld.find(kA)->segs[0].addr + 0x20 + 8);
    }

    // --- rejected input leaves no module behind ---
    {
        Fx f;
        f.put(kCrsBase, build(crsDef()));
        CHECK(f.ld.initializeCrs(kCrsBase, 0x1000, &err));
        LoadParams p;
        p.base = kA;
        p.size = 0x2000;
        p.dataBuf = kDataA;
        p.dataSize = 0x40;
        p.bssBuf = kBssA;
        p.bssSize = 0x30;

        CHECK(!f.ld.loadCro(p, &err) && !f.ld.find(kA)); // zeroed memory: bad magic

        Def d;
        d.name = "Bad";
        d.dataSize = 0x40;
        f.put(kA, build(d));
        LoadParams small = p;
        small.dataSize = 0x20;
        CHECK(!f.ld.loadCro(small, &err) && !f.ld.find(kA)); // data buffer too small
        CHECK(err.find("data buffer") != std::string::npos);
        small = p;
        small.bssSize = 0x10;
        CHECK(!f.ld.loadCro(small, &err) && !f.ld.find(kA));

        Def r = d;
        r.internal = {{tag(2, 0x1000), 2, 0, 0}}; // target beyond the data segment
        f.put(kA, build(r));
        CHECK(!f.ld.loadCro(p, &err));
        CHECK(err.find("outside") != std::string::npos);
        Def t = d;
        t.internal = {{tag(2, 0), 3, 0, 0}}; // type 3 (relative) is not implemented: refused, not guessed
        f.put(kA, build(t));
        CHECK(!f.ld.loadCro(p, &err) && f.ld.stats().unsupportedRelocs == 1);

        // File size larger than the mapped range.
        Def ok = d;
        f.put(kA, build(ok));
        LoadParams tooSmall = p;
        tooSmall.size = 0x800;
        CHECK(!f.ld.loadCro(tooSmall, &err));

        // The same image loads fine once valid, and a second module with the same name is refused.
        f.put(kA, build(ok));
        CHECK(f.ld.loadCro(p, &err));
        f.put(kB, build(ok));
        LoadParams p2 = p;
        p2.base = kB;
        p2.dataBuf = kDataB;
        p2.bssBuf = kBssB;
        CHECK(!f.ld.loadCro(p2, &err));
        CHECK(err.find("already loaded") != std::string::npos);
        // overlapping mapping
        LoadParams p3 = p;
        p3.base = kA + 0x1000;
        CHECK(!f.ld.loadCro(p3, &err));
        // LoadCRO before the CRS exists
        Fx g;
        CHECK(!g.ld.loadCro(p, &err));
        // Writes can only reach registered memory: a CRS import aimed outside its segments is refused.
        Fx h;
        Def c2 = crsDef();
        c2.importModules[0].anon[0].second.relocs[0].tag = tag(2, 0x5000); // beyond the 0x1000-byte .data
        h.put(kCrsBase, build(c2));
        CHECK(h.ld.initializeCrs(kCrsBase, 0x1000, &err));
        Def da;
        da.name = "ModA";
        da.indexedExports = {};
        h.put(kA, build(da));
        CHECK(!h.ld.loadCro(p, &err));
    }

    std::printf(g_fail ? "FAIL\n" : "ro_loader ok\n");
    return g_fail ? 1 : 0;
}
