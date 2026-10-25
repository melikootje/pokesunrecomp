// ldr:ro -- the static-module loader (issue #8). The game's nn::ro client hands the service heap buffers
// holding a CRS or CRO image together with an address in a window after the executable image where the
// module must appear; the service maps the buffer there (both views stay coherent), rebases and relocates
// it, and links it against the modules already loaded. Evidence for the command layouts: docs/ldr-ro.md.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "pokerecomp/ro.h"
#include "pokerecomp/services.h"

namespace pokerecomp {
namespace {

// Placeholder failure codes: the client only tests the sign bit. Not verified against hardware.
constexpr uint32_t kRoBadMemory = kresult::InvalidAddress;
constexpr uint32_t kRoBadModule = kresult::InvalidCombination;

class LdrRo : public Service {
  public:
    explicit LdrRo(Kernel &k) : k_(k), loader_(k.memory().base()) {}
    const char *name() const override { return "ldr:ro"; }

    void handle(IpcRequest &r) override {
        // The guest copies a freshly loaded module's data segment out of the image right after the request
        // returns, so by the time the next request arrives the data buffer is live.
        loader_.markDataLive();
        switch (r.commandId()) {
        case 0x0001: { // Initialize(crsBuffer, size, mappedAddress) + process handle
            const uint32_t buf = r.cmd[1], size = r.cmd[2], mapped = r.cmd[3];
            if (!mapView(mapped, buf, size)) {
                r.reply(1, 0, kRoBadMemory);
                return;
            }
            std::string err;
            if (!loader_.initializeCrs(mapped, size, &err)) {
                k_.log("[ldr:ro] Initialize failed: " + err);
                r.reply(1, 0, kRoBadModule);
                return;
            }
            k_.dispatcher().invalidate(mapped, size);
            r.reply(1, 0, kresult::Success);
            return;
        }
        case 0x0002: // LoadCRR(crrBuffer, size) + process handle: signatures are not verified
        case 0x0003: // UnloadCRR
            r.reply(1, 0, kresult::Success);
            return;
        case 0x0009: { // LoadCRO_New: 11 normal words + process handle (layout derived from the client stub)
            ro::LoadParams p;
            const uint32_t buf = r.cmd[1];
            p.base = r.cmd[2];
            p.size = r.cmd[3];
            p.dataBuf = r.cmd[4];
            p.dataSize = r.cmd[6];
            p.bssBuf = r.cmd[7];
            p.bssSize = r.cmd[8];
            p.autoLink = (r.cmd[9] & 0xFF) != 0;
            if (!mapView(p.base, buf, p.size)) {
                r.reply(1, 0, kRoBadMemory);
                return;
            }
            std::string err;
            if (!loader_.loadCro(p, &err)) {
                k_.log("[ldr:ro] LoadCRO failed: " + err);
                r.reply(1, 0, kRoBadModule);
                return;
            }
            k_.dispatcher().invalidate(p.base, p.size);
            const ro::Module *m = loader_.find(p.base);
            char b[160];
            std::snprintf(b, sizeof b, "[ldr:ro] loaded '%s' at %08x (%u bytes, data %08x, bss %08x)",
                          m->name.c_str(), p.base, p.size, p.dataBuf, p.bssBuf);
            k_.log(b);
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = p.size; // "fixed size": the whole buffer stays in use (no tail is released)
            return;
        }
        case 0x0005: { // UnloadCRO(mappedAddress, 0, croBuffer) + process handle
            std::string err;
            if (!loader_.unloadCro(r.cmd[1], &err)) {
                k_.log("[ldr:ro] UnloadCRO failed: " + err);
                r.reply(1, 0, kRoBadModule);
                return;
            }
            r.reply(1, 0, kresult::Success);
            return;
        }
        default:
            if (seen_.insert(r.header()).second) {
                char b[96];
                std::snprintf(b, sizeof b, "[ldr:ro] unimplemented command %08x", r.header());
                k_.log(b);
            }
            r.reply(1, 0, kresult::InvalidCommand);
            return;
        }
    }

  private:
    // Makes [mapped, mapped + size) a second view of the guest buffer at `buf`. Falls back to a one-time copy
    // when the host cannot alias that pair of addresses (then writes through the two views diverge).
    bool mapView(uint32_t mapped, uint32_t buf, uint32_t size) {
        if (!size || (mapped & 0xFFF) || (buf & 0xFFF) || uint64_t(mapped) + size > 0xFFFFFFFFull ||
            uint64_t(buf) + size > 0xFFFFFFFFull)
            return false;
        GuestMemory &gm = k_.memory();
        if (gm.alias(mapped, buf, size))
            return true;
        const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
        const uint32_t lo = mapped & ~(pg - 1), hi = (mapped + size + pg - 1) & ~(pg - 1);
        if (!gm.map(lo, hi - lo, GuestMemory::Read | GuestMemory::Write))
            return false;
        std::memcpy(gm.base() + mapped, gm.base() + buf, size);
        k_.log("[ldr:ro] warning: host cannot alias " + hex(buf) + " -> " + hex(mapped) + "; copied instead");
        return true;
    }
    static std::string hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", v);
        return b;
    }

    Kernel &k_;
    ro::Loader loader_;
    std::set<uint32_t> seen_;
};

} // namespace

void registerLdrService(Kernel &kernel) { kernel.registerService(std::make_unique<LdrRo>(kernel)); }

} // namespace pokerecomp
