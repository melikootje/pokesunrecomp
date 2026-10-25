#pragma once
// Static-module (CRS/CRO) loader behind ldr:ro (issue #8). Works on a guest address space given as a host
// pointer (host base + guest address) and never touches memory outside the module image, the data/bss buffers
// the guest provided, and the segments of already-loaded modules. See docs/ldr-ro.md for what is verified.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pokerecomp {
namespace ro {

enum class SegType : uint32_t { Text = 0, Rodata = 1, Data = 2, Bss = 3 };

struct Segment {
    uint32_t addr = 0; // where code sees the segment (data/bss: the buffers the guest supplied)
    uint32_t imageAddr =
        0; // where the segment's initial bytes live in the mapped image (== addr, except data)
    uint32_t size = 0;
    SegType type = SegType::Text;
    uint32_t fileOffset = 0; // the offset field as stored in the file (restored on unload)
};

struct Module {
    std::string name;
    bool isCrs = false;
    uint32_t base = 0;      // mapped address of the image
    uint32_t imageSize = 0; // bytes mapped at base
    std::vector<Segment> segs;
    // True once the guest has copied the data segment into its buffer: later patches to .data then go to the
    // live buffer instead of the image.
    bool dataLive = false;
    uint32_t onUnresolved = 0; // absolute address (CRS only)

    // Absolute table addresses (already rebased) and entry counts.
    struct Table {
        uint32_t addr = 0, num = 0;
    };
    Table namedExport, indexedExport, importModule, extReloc, namedImport, indexedImport, anonImport,
        intReloc, staticReloc, staticAnon;
    std::unordered_map<std::string, uint32_t> exports; // name -> absolute address
    uint32_t next = 0, prev = 0;                       // neighbours in the module list (mapped bases)
};

struct LoadParams {
    uint32_t base = 0, size = 0;
    uint32_t dataBuf = 0, dataSize = 0, bssBuf = 0, bssSize = 0;
    bool autoLink = true;
};

struct Stats {
    uint64_t internalRelocs = 0, externalApplied = 0, externalUnresolved = 0, batchesResolved = 0;
    uint64_t unsupportedRelocs = 0;
};

class Loader {
  public:
    explicit Loader(uint8_t *mem) : mem_(mem) {}

    // Rebases the CRS (static module). Its segment table already holds absolute addresses of the main
    // executable. Returns false and fills *err on malformed input.
    bool initializeCrs(uint32_t base, uint32_t size, std::string *err);
    // Rebases, relocates and (optionally) links a CRO already mapped at p.base.
    bool loadCro(const LoadParams &p, std::string *err);
    bool unloadCro(uint32_t base, std::string *err);
    // The guest copies a module's data segment into its buffer right after loadCro returns; call this when
    // the next ldr:ro request arrives so later patches target the live buffer.
    void markDataLive();

    const Module *find(uint32_t base) const;
    const Module *findByName(const std::string &name) const;
    const Module *crs() const { return crs_; }
    std::vector<const Module *> modules() const; // CRS first, then load order
    // First export of `name` across the loaded modules in list order.
    bool lookup(const std::string &name, uint32_t *addr) const;
    const Stats &stats() const { return stats_; }

  private:
    struct Batch {
        uint32_t importer, batch, provider;
    };
    bool parse(Module &m, const LoadParams &p, std::string *err);
    bool rebase(Module &m, const LoadParams &p, std::string *err);
    // Undoes rebase(): header and table offsets become image-relative again, as the guest expects after
    // UnloadCRO.
    bool unrebase(Module &m, std::string *err);
    bool relocateInternal(Module &m, std::string *err);
    bool applyBatch(Module &m, uint32_t batchAddr, uint32_t symbol, bool resolved, std::string *err);
    bool linkAll(std::string *err);
    bool symbolAddr(const Module &m, uint32_t tag, uint32_t *out) const;
    bool targetAddr(const Module &m, uint32_t tag, uint32_t *out) const;
    void relist();

    // Guarded guest memory access: only ranges registered with allow() are touched.
    void allow(uint32_t lo, uint32_t len);
    bool ok(uint32_t a, uint32_t n) const;
    bool rd(uint32_t a, uint32_t *v, std::string *err) const;
    bool wr(uint32_t a, uint32_t v, std::string *err);
    uint8_t rb(uint32_t a) const { return mem_[a]; }
    std::string cstr(uint32_t a) const; // bounded by the allowed range containing a

    uint8_t *mem_;
    std::map<uint32_t, uint32_t> allowed_; // start -> end of every range the loader may touch
    std::map<uint32_t, Module> mods_;      // by base
    std::vector<uint32_t> order_;          // load order (bases); CRS first
    Module *crs_ = nullptr;
    std::set<std::pair<uint32_t, uint32_t>> resolved_; // (importer base, batch address)
    std::vector<Batch> batches_;
    Stats stats_;
};

} // namespace ro
} // namespace pokerecomp
