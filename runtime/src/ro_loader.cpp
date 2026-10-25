#include "pokerecomp/ro.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pokerecomp {
namespace ro {
namespace {

constexpr uint32_t kHeaderSize = 0x138;
// Header fields (offsets from the start of the image; the first 0x80 bytes are hashes).
enum : uint32_t {
    Magic = 0x80,
    NameOffset = 0x84,
    NextCro = 0x88,
    PrevCro = 0x8C,
    FileSize = 0x90,
    OnUnresolvedTag = 0xAC,
    CodeOffset = 0xB0,
    DataOffset = 0xB8,
    ModuleNameOffset = 0xC0,
    SegTableOffset = 0xC8,
    SegNum = 0xCC,
    ExpNamedOffset = 0xD0,
    ExpNamedNum = 0xD4,
    ExpIndexedOffset = 0xD8,
    ExpIndexedNum = 0xDC,
    ExpStringsOffset = 0xE0,
    ExpTreeOffset = 0xE8,
    ImpModuleOffset = 0xF0,
    ImpModuleNum = 0xF4,
    ExtRelocOffset = 0xF8,
    ExtRelocNum = 0xFC,
    ImpNamedOffset = 0x100,
    ImpNamedNum = 0x104,
    ImpIndexedOffset = 0x108,
    ImpIndexedNum = 0x10C,
    ImpAnonOffset = 0x110,
    ImpAnonNum = 0x114,
    ImpStringsOffset = 0x118,
    StaticAnonOffset = 0x120,
    StaticAnonNum = 0x124,
    IntRelocOffset = 0x128,
    IntRelocNum = 0x12C,
    StaticRelocOffset = 0x130,
    StaticRelocNum = 0x134,
};
constexpr uint32_t kRelocSize = 12, kSegSize = 12, kModEntrySize = 20, kPairSize = 8;
constexpr uint32_t kMaxSegments = 16, kNoTag = 0xFFFFFFFFu;
constexpr uint8_t kRelocNone = 0, kRelocAbsolute = 2;
constexpr uint32_t kMagicCro = 0x304F5243u /* "CRO0" */, kMagicCrs = 0x30535243u /* "CRS0" */;

bool fail(std::string *err, const std::string &m) {
    if (err)
        *err = m;
    return false;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

} // namespace

// ------------------------------------------------------------------------------------ guarded access

void Loader::allow(uint32_t lo, uint32_t len) {
    if (!len)
        return;
    auto it = allowed_.find(lo);
    if (it == allowed_.end() || it->second < lo + len)
        allowed_[lo] = lo + len;
}

bool Loader::ok(uint32_t a, uint32_t n) const {
    auto it = allowed_.upper_bound(a);
    if (it == allowed_.begin())
        return false;
    --it;
    return uint64_t(a) + n <= it->second;
}

bool Loader::rd(uint32_t a, uint32_t *v, std::string *err) const {
    if (!ok(a, 4))
        return fail(err, "read outside the module at " + hex(a));
    std::memcpy(v, mem_ + a, 4);
    return true;
}

bool Loader::wr(uint32_t a, uint32_t v, std::string *err) {
    if (!ok(a, 4))
        return fail(err, "write outside the module at " + hex(a));
    std::memcpy(mem_ + a, &v, 4);
    return true;
}

std::string Loader::cstr(uint32_t a) const {
    std::string s;
    auto it = allowed_.upper_bound(a);
    if (it == allowed_.begin())
        return s;
    --it;
    for (uint32_t p = a; p < it->second && s.size() < 4096 && mem_[p]; p++)
        s += char(mem_[p]);
    return s;
}

// ------------------------------------------------------------------------------------ addresses

bool Loader::symbolAddr(const Module &m, uint32_t tag, uint32_t *out) const {
    const uint32_t idx = tag & 0xF, off = tag >> 4;
    if (idx >= m.segs.size() || off > m.segs[idx].size)
        return false;
    *out = m.segs[idx].addr + off;
    return true;
}

// Where a relocation's 32-bit target word lives right now. Until the guest has copied the data segment out
// of the image, .data is patched in the image so the copy carries the relocated words.
bool Loader::targetAddr(const Module &m, uint32_t tag, uint32_t *out) const {
    const uint32_t idx = tag & 0xF, off = tag >> 4;
    if (idx >= m.segs.size() || uint64_t(off) + 4 > m.segs[idx].size)
        return false;
    const Segment &s = m.segs[idx];
    *out = ((s.type == SegType::Data && !m.dataLive) ? s.imageAddr : s.addr) + off;
    return true;
}

// ------------------------------------------------------------------------------------ parsing

bool Loader::parse(Module &m, const LoadParams &p, std::string *err) {
    m.base = p.base;
    m.imageSize = p.size;
    if (p.size < kHeaderSize || uint64_t(p.base) + p.size > 0xFFFFFFFFull)
        return fail(err, "image smaller than a CRO header");
    allow(p.base, p.size);
    auto H = [&](uint32_t off) {
        uint32_t v = 0;
        rd(p.base + off, &v, nullptr);
        return v;
    };
    const uint32_t magic = H(Magic);
    if (magic != kMagicCro && magic != kMagicCrs)
        return fail(err, "bad CRO magic");
    const uint32_t fileSize = H(FileSize);
    if (fileSize < kHeaderSize || fileSize > p.size)
        return fail(err, "header file size " + hex(fileSize) + " does not fit the mapped " + hex(p.size));

    auto table = [&](uint32_t offField, uint32_t numField, uint32_t entry, Module::Table *t,
                     const char *what) {
        const uint32_t off = H(offField), num = H(numField);
        if (!num) {
            *t = {};
            return true;
        }
        if (off < kHeaderSize || off > fileSize || num > (fileSize - off) / entry)
            return fail(err, std::string(what) + " table exceeds the image");
        t->addr = p.base + off;
        t->num = num;
        return true;
    };
    Module::Table seg;
    if (!table(SegTableOffset, SegNum, kSegSize, &seg, "segment") ||
        !table(ExpNamedOffset, ExpNamedNum, kPairSize, &m.namedExport, "named export") ||
        !table(ExpIndexedOffset, ExpIndexedNum, 4, &m.indexedExport, "indexed export") ||
        !table(ImpModuleOffset, ImpModuleNum, kModEntrySize, &m.importModule, "import module") ||
        !table(ExtRelocOffset, ExtRelocNum, kRelocSize, &m.extReloc, "external relocation") ||
        !table(ImpNamedOffset, ImpNamedNum, kPairSize, &m.namedImport, "named import") ||
        !table(ImpIndexedOffset, ImpIndexedNum, kPairSize, &m.indexedImport, "indexed import") ||
        !table(ImpAnonOffset, ImpAnonNum, kPairSize, &m.anonImport, "anonymous import") ||
        !table(StaticAnonOffset, StaticAnonNum, kPairSize, &m.staticAnon, "static anonymous") ||
        !table(IntRelocOffset, IntRelocNum, kRelocSize, &m.intReloc, "internal relocation") ||
        !table(StaticRelocOffset, StaticRelocNum, kRelocSize, &m.staticReloc, "static relocation"))
        return false;
    if (seg.num > kMaxSegments)
        return fail(err, "too many segments");
    const uint32_t nameOff = H(ModuleNameOffset);
    if (nameOff < kHeaderSize || nameOff >= fileSize)
        return fail(err, "module name outside the image");
    m.name = cstr(p.base + nameOff);

    // Segments.
    for (uint32_t i = 0; i < seg.num; i++) {
        uint32_t off = 0, size = 0, type = 0;
        rd(seg.addr + i * kSegSize, &off, nullptr);
        rd(seg.addr + i * kSegSize + 4, &size, nullptr);
        rd(seg.addr + i * kSegSize + 8, &type, nullptr);
        if (type > 3)
            return fail(err, "bad segment type");
        Segment s;
        s.size = size;
        s.type = SegType(type);
        if (m.isCrs) { // the CRS describes the main executable: offsets are absolute guest addresses
            s.addr = s.imageAddr = off;
            if (size && uint64_t(off) + size > 0xFFFFFFFFull)
                return fail(err, "CRS segment wraps the address space");
            allow(off, size);
        } else if (s.type == SegType::Data) {
            if (size) {
                if (size > p.dataSize)
                    return fail(err, "data buffer too small for the data segment");
                if (off > fileSize || size > fileSize - off)
                    return fail(err, "data segment outside the image");
                s.imageAddr = p.base + off;
                s.addr = p.dataBuf;
                allow(p.dataBuf, std::max(size, 4u));
            }
        } else if (s.type == SegType::Bss) {
            if (size) {
                if (size > p.bssSize)
                    return fail(err, "bss buffer too small for the bss segment");
                s.addr = s.imageAddr = p.bssBuf;
                allow(p.bssBuf, size);
            }
        } else if (off || size) {
            if (off > fileSize || size > fileSize - off)
                return fail(err, "segment outside the image");
            s.addr = s.imageAddr = p.base + off;
        }
        m.segs.push_back(s);
    }

    if (m.isCrs) {
        const uint32_t tag = H(OnUnresolvedTag);
        uint32_t a = 0;
        if (tag != kNoTag && symbolAddr(m, tag, &a))
            m.onUnresolved = a;
    }

    // Named exports (read from the unrebased table: names are image-relative here).
    for (uint32_t i = 0; i < m.namedExport.num; i++) {
        uint32_t nameOffset = 0, tag = 0, a = 0;
        rd(m.namedExport.addr + i * kPairSize, &nameOffset, nullptr);
        rd(m.namedExport.addr + i * kPairSize + 4, &tag, nullptr);
        if (nameOffset >= fileSize || !symbolAddr(m, tag, &a))
            continue;
        m.exports[cstr(p.base + nameOffset)] = a;
    }
    return true;
}

// Writes the rebased view the guest reads: header offsets become absolute addresses, the segment table
// holds the real segment addresses, and table entries point at absolute strings and batches.
bool Loader::rebase(Module &m, const LoadParams &p, std::string *err) {
    auto add = [&](uint32_t a) { // *a += base when non-zero
        uint32_t v = 0;
        if (!rd(a, &v, err))
            return false;
        return v ? wr(a, v + p.base, err) : true;
    };
    static const uint32_t kHdr[] = {NameOffset,       CodeOffset,       DataOffset,       ModuleNameOffset,
                                    SegTableOffset,   ExpNamedOffset,   ExpIndexedOffset, ExpStringsOffset,
                                    ExpTreeOffset,    ImpModuleOffset,  ExtRelocOffset,   ImpNamedOffset,
                                    ImpIndexedOffset, ImpAnonOffset,    ImpStringsOffset, StaticAnonOffset,
                                    IntRelocOffset,   StaticRelocOffset};
    uint32_t segTable = 0, segNum = 0;
    rd(p.base + SegTableOffset, &segTable, nullptr);
    rd(p.base + SegNum, &segNum, nullptr);
    segTable += p.base;
    for (uint32_t off : kHdr)
        if (!add(p.base + off))
            return false;
    if (!m.isCrs)
        for (uint32_t i = 0; i < m.segs.size(); i++) {
            const Segment &s = m.segs[i];
            if (s.size || s.addr)
                if (!wr(segTable + i * kSegSize, s.addr, err))
                    return false;
        }
    for (uint32_t i = 0; i < m.namedExport.num; i++)
        if (!add(m.namedExport.addr + i * kPairSize))
            return false;
    for (uint32_t i = 0; i < m.importModule.num; i++) {
        const uint32_t e = m.importModule.addr + i * kModEntrySize;
        if (!add(e) || !add(e + 4) || !add(e + 12))
            return false;
    }
    for (uint32_t i = 0; i < m.namedImport.num; i++) {
        const uint32_t e = m.namedImport.addr + i * kPairSize;
        if (!add(e) || !add(e + 4))
            return false;
    }
    for (uint32_t i = 0; i < m.indexedImport.num; i++)
        if (!add(m.indexedImport.addr + i * kPairSize + 4))
            return false;
    for (uint32_t i = 0; i < m.anonImport.num; i++)
        if (!add(m.anonImport.addr + i * kPairSize + 4))
            return false;
    for (uint32_t i = 0; i < m.staticAnon.num; i++)
        if (!add(m.staticAnon.addr + i * kPairSize + 4))
            return false;
    return true;
}

// ------------------------------------------------------------------------------------ relocation

bool Loader::relocateInternal(Module &m, std::string *err) {
    for (uint32_t i = 0; i < m.intReloc.num; i++) {
        const uint32_t e = m.intReloc.addr + i * kRelocSize;
        uint32_t tag = 0, addend = 0;
        if (!rd(e, &tag, err) || !rd(e + 8, &addend, err))
            return false;
        const uint8_t type = mem_[e + 4], symSeg = mem_[e + 5];
        if (type == kRelocNone)
            continue;
        if (type != kRelocAbsolute) {
            stats_.unsupportedRelocs++;
            return fail(err, "unsupported internal relocation type " + std::to_string(type));
        }
        uint32_t target = 0;
        if (!targetAddr(m, tag, &target) || symSeg >= m.segs.size())
            return fail(err, "internal relocation " + std::to_string(i) + " points outside the module");
        if (!wr(target, m.segs[symSeg].addr + addend, err))
            return false;
        stats_.internalRelocs++;
    }
    return true;
}

// Applies one relocation batch (a run of external-relocation entries ending at the one flagged "last").
bool Loader::applyBatch(Module &m, uint32_t batchAddr, uint32_t symbol, bool resolved, std::string *err) {
    const uint32_t lo = m.extReloc.addr, hi = lo + m.extReloc.num * kRelocSize;
    for (uint32_t p = batchAddr;; p += kRelocSize) {
        if (p < lo || p + kRelocSize > hi || (p - lo) % kRelocSize)
            return fail(err, "relocation batch leaves the external relocation table");
        uint32_t tag = 0, addend = 0;
        if (!rd(p, &tag, err) || !rd(p + 8, &addend, err))
            return false;
        const uint8_t type = mem_[p + 4], last = mem_[p + 5];
        if (type == kRelocAbsolute) {
            uint32_t target = 0;
            if (!targetAddr(m, tag, &target))
                return fail(err, "external relocation target outside the module");
            if (!wr(target, symbol + addend, err))
                return false;
            if (resolved)
                stats_.externalApplied++;
        } else if (type != kRelocNone) {
            stats_.unsupportedRelocs++;
            return fail(err, "unsupported external relocation type " + std::to_string(type));
        }
        mem_[p + 6] = resolved ? 1 : 0;
        if (last)
            return true;
    }
}

bool Loader::lookup(const std::string &name, uint32_t *addr) const {
    for (uint32_t b : order_) {
        const Module &m = mods_.at(b);
        auto it = m.exports.find(name);
        if (it != m.exports.end()) {
            *addr = it->second;
            return true;
        }
    }
    return false;
}

bool Loader::linkAll(std::string *err) {
    for (uint32_t b : order_) {
        Module &m = mods_.at(b);
        auto once = [&](uint32_t batch, uint32_t symbol, uint32_t provider) {
            if (!resolved_.insert({m.base, batch}).second)
                return true;
            batches_.push_back({m.base, batch, provider});
            stats_.batchesResolved++;
            return applyBatch(m, batch, symbol, true, err);
        };
        for (uint32_t i = 0; i < m.namedImport.num; i++) {
            const uint32_t e = m.namedImport.addr + i * kPairSize;
            uint32_t nameAddr = 0, batch = 0;
            if (!rd(e, &nameAddr, err) || !rd(e + 4, &batch, err))
                return false;
            const std::string name = cstr(nameAddr);
            uint32_t sym = 0, provider = 0;
            bool found = false;
            for (uint32_t pb : order_) {
                const Module &pm = mods_.at(pb);
                auto it = pm.exports.find(name);
                if (it != pm.exports.end()) {
                    sym = it->second;
                    provider = pb;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!resolved_.count({m.base, batch}))
                    stats_.externalUnresolved++;
                continue;
            }
            if (!once(batch, sym, provider))
                return false;
        }
        for (uint32_t i = 0; i < m.importModule.num; i++) {
            const uint32_t e = m.importModule.addr + i * kModEntrySize;
            uint32_t nameAddr = 0, idxAddr = 0, idxNum = 0, anonAddr = 0, anonNum = 0;
            if (!rd(e, &nameAddr, err) || !rd(e + 4, &idxAddr, err) || !rd(e + 8, &idxNum, err) ||
                !rd(e + 12, &anonAddr, err) || !rd(e + 16, &anonNum, err))
                return false;
            const Module *src = findByName(cstr(nameAddr));
            if (!src)
                continue; // the module is not loaded (yet): its batches stay unresolved
            for (uint32_t k = 0; k < idxNum; k++) {
                uint32_t index = 0, batch = 0, tag = 0, sym = 0;
                if (!rd(idxAddr + k * kPairSize, &index, err) ||
                    !rd(idxAddr + k * kPairSize + 4, &batch, err))
                    return false;
                if (index >= src->indexedExport.num || !rd(src->indexedExport.addr + 4 * index, &tag, err) ||
                    !symbolAddr(*src, tag, &sym))
                    return fail(err,
                                "indexed import " + std::to_string(index) + " not exported by " + src->name);
                if (!once(batch, sym, src->base))
                    return false;
            }
            for (uint32_t k = 0; k < anonNum; k++) {
                uint32_t tag = 0, batch = 0, sym = 0;
                if (!rd(anonAddr + k * kPairSize, &tag, err) ||
                    !rd(anonAddr + k * kPairSize + 4, &batch, err))
                    return false;
                if (!symbolAddr(*src, tag, &sym))
                    return fail(err, "anonymous import tag " + hex(tag) + " outside " + src->name);
                if (!once(batch, sym, src->base))
                    return false;
            }
        }
    }
    return true;
}

void Loader::relist() {
    for (size_t i = 0; i < order_.size(); i++) {
        Module &m = mods_.at(order_[i]);
        m.prev = i ? order_[i - 1] : 0;
        m.next = i + 1 < order_.size() ? order_[i + 1] : 0;
        wr(m.base + PrevCro, m.prev, nullptr);
        wr(m.base + NextCro, m.next, nullptr);
    }
}

// ------------------------------------------------------------------------------------ public

bool Loader::initializeCrs(uint32_t base, uint32_t size, std::string *err) {
    if (crs_)
        return fail(err, "the CRS is already initialised");
    Module m;
    m.isCrs = true;
    LoadParams p;
    p.base = base;
    p.size = size;
    if (!parse(m, p, err) || !rebase(m, p, err))
        return false;
    auto it = mods_.emplace(base, std::move(m)).first;
    crs_ = &it->second;
    order_.insert(order_.begin(), base);
    if (!relocateInternal(*crs_, err))
        return false;
    relist();
    return true;
}

bool Loader::loadCro(const LoadParams &p, std::string *err) {
    if (!crs_)
        return fail(err, "LoadCRO before Initialize");
    if (mods_.count(p.base))
        return fail(err, "a module is already mapped at " + hex(p.base));
    for (const auto &kv : mods_)
        if (p.base < kv.first + kv.second.imageSize && kv.first < uint64_t(p.base) + p.size)
            return fail(err, "mapped range overlaps module '" + kv.second.name + "'");
    Module m;
    // Any failure below leaves no half-registered module behind (the guest sees an error result).
    const auto savedAllowed = allowed_;
    auto bail = [&](bool ok) {
        if (!ok)
            allowed_ = savedAllowed;
        return ok;
    };
    if (!bail(parse(m, p, err)) || !bail(rebase(m, p, err)))
        return false;
    for (const auto &kv : mods_)
        if (kv.second.name == m.name) {
            allowed_ = savedAllowed;
            return fail(err, "module '" + m.name + "' is already loaded");
        }
    const uint32_t base = p.base;
    auto it = mods_.emplace(base, std::move(m)).first;
    order_.push_back(base);
    Module &mod = it->second;
    // From here on the module is registered; any failure takes it out again so the guest can retry.
    auto rollback = [&]() {
        std::vector<Batch> keep;
        for (const Batch &b : batches_)
            if (b.importer != base)
                keep.push_back(b);
        batches_ = std::move(keep);
        for (auto r = resolved_.begin(); r != resolved_.end();)
            r = r->first == base ? resolved_.erase(r) : std::next(r);
        order_.erase(std::remove(order_.begin(), order_.end(), base), order_.end());
        mods_.erase(base);
        allowed_ = savedAllowed;
        relist();
        return false;
    };
    // Calls into imports that are not resolved yet land in the CRS's OnUnresolved handler.
    if (crs_->onUnresolved)
        for (uint32_t i = 0; i < mod.extReloc.num; i++) {
            const uint32_t e = mod.extReloc.addr + i * kRelocSize;
            uint32_t tag = 0, addend = 0, target = 0;
            if (!rd(e, &tag, err) || !rd(e + 8, &addend, err))
                return rollback();
            if (mem_[e + 4] == kRelocAbsolute && targetAddr(mod, tag, &target)) {
                if (!wr(target, crs_->onUnresolved + addend, err))
                    return rollback();
                stats_.externalUnresolved++;
            }
        }
    if (!relocateInternal(mod, err))
        return rollback();
    if (p.autoLink && !linkAll(err))
        return rollback();
    relist();
    return true;
}

bool Loader::unloadCro(uint32_t base, std::string *err) {
    auto it = mods_.find(base);
    if (it == mods_.end() || it->second.isCrs)
        return fail(err, "no such CRO");
    // Imports of other modules that were satisfied by this one go back to the unresolved handler.
    std::vector<Batch> keep;
    for (const Batch &b : batches_) {
        if (b.importer == base)
            continue;
        if (b.provider == base) {
            resolved_.erase({b.importer, b.batch});
            auto im = mods_.find(b.importer);
            if (im != mods_.end() && crs_->onUnresolved &&
                !applyBatch(im->second, b.batch, crs_->onUnresolved, false, err))
                return false;
            continue;
        }
        keep.push_back(b);
    }
    batches_ = std::move(keep);
    for (auto r = resolved_.begin(); r != resolved_.end();)
        r = r->first == base ? resolved_.erase(r) : std::next(r);
    allowed_.erase(base);
    order_.erase(std::remove(order_.begin(), order_.end(), base), order_.end());
    mods_.erase(it);
    relist();
    return true;
}

void Loader::markDataLive() {
    for (auto &kv : mods_)
        kv.second.dataLive = true;
}

const Module *Loader::find(uint32_t base) const {
    auto it = mods_.find(base);
    return it == mods_.end() ? nullptr : &it->second;
}

const Module *Loader::findByName(const std::string &name) const {
    for (uint32_t b : order_) {
        const Module &m = mods_.at(b);
        if (m.name == name)
            return &m;
    }
    return nullptr;
}

std::vector<const Module *> Loader::modules() const {
    std::vector<const Module *> out;
    for (uint32_t b : order_)
        out.push_back(&mods_.at(b));
    return out;
}

} // namespace ro
} // namespace pokerecomp
