// boot_probe: runs a locally generated recompilation of a user-supplied code image from its entry point on
// the HLE kernel, logging SVCs, IPC, faults and dispatcher coverage. Local analysis only; the generated code
// it links is derived from game data and must not be committed.
//
//   boot_probe <code.bin> [maxInsns] [lenient]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "png.h"
#include "pokerecomp/dispatcher.h"
#include "pokerecomp/gpu.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/kernel.h"
#include "pokerecomp/save_store.h"
#include "pokerecomp/services.h"
#include "romlib/dump.h"

extern "C" {
typedef struct {
    void (*fn)(RcCtx *);
    uint32_t addr, bytes, insns, thumb;
} rc_block_t;
extern const rc_block_t rc_blocks[];
extern const unsigned rc_block_count;
}

using namespace pokerecomp;

namespace {
uint64_t g_progress = 0; // guest instructions executed so far (for time-gated diagnostics)
constexpr uint32_t kText = 0x00100000, kImageEnd = 0x006D5000;
constexpr uint32_t kStackTop = 0x10000000,
                   kStackSize = 0x100000; // main thread stack (3DS: top of the app stack region)
} // namespace

namespace pokerecomp::pica {}
int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: boot_probe <code.bin> [maxInsns] [lenient]\n");
        return 2;
    }
    const uint64_t maxInsns = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000000000ull;
    const bool lenient = argc > 3 && std::string(argv[3]) == "lenient";
    std::vector<uint8_t> code;
    {
        std::ifstream f(argv[1], std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(f), {});
    }
    GuestMemory gm;
    if (!gm.ok())
        return 1;
    const int rw = GuestMemory::Read | GuestMemory::Write;
    const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
    gm.map(kText, (kImageEnd - kText + pg - 1) & ~(pg - 1), rw);
    gm.map(kStackTop - kStackSize, kStackSize, rw);
    std::memcpy(gm.base() + kText, code.data(), code.size());

    Dispatcher disp;
    for (unsigned k = 0; k < rc_block_count; k++) {
        BlockInfo b;
        b.fn = rc_blocks[k].fn;
        b.addr = rc_blocks[k].addr;
        b.bytes = rc_blocks[k].bytes;
        b.insns = rc_blocks[k].insns;
        b.thumb = rc_blocks[k].thumb != 0;
        disp.registerBlock(b);
    }
    // Optional RomFS from the user's own dump (argv[4]), served lazily through romlib.
    struct RomBackend : FsBackend {
        std::unique_ptr<romlib::FileSource> src;
        std::unique_ptr<romlib::Dump> dump;
        std::unique_ptr<romlib::SubSource> region;
        std::unique_ptr<romlib::Romfs> fs;
        // The RomFS archive exposes IVFC level 3 (starting with its 0x28-byte header), not the IVFC header.
        bool rawRomfsSize(uint64_t *size) override {
            if (!fs)
                return false;
            *size = fs->level3Size();
            return true;
        }
        int64_t rawRomfsRead(uint64_t off, uint32_t size, void *dst) override {
            if (!fs || off >= fs->level3Size())
                return 0;
            const uint32_t n = uint32_t(std::min<uint64_t>(size, fs->level3Size() - off));
            region->read(fs->level3Offset() + off, dst, n);
            return n;
        }
        bool romfsFileSize(const std::string &p, uint64_t *size) override {
            if (!fs || !fs->exists(p))
                return false;
            *size = fs->fileSize(p);
            return true;
        }
        int64_t romfsRead(const std::string &p, uint64_t off, uint32_t size, void *dst) override {
            if (!fs || !fs->exists(p))
                return -1;
            const uint64_t total = fs->fileSize(p);
            if (off >= total)
                return 0;
            const uint32_t n = uint32_t(std::min<uint64_t>(size, total - off));
            const auto b = fs->read(p, off, n);
            std::memcpy(dst, b.data(), b.size());
            return int64_t(b.size());
        }
    };
    auto rom = std::make_shared<RomBackend>();
    if (argc > 4) {
        rom->src = std::make_unique<romlib::FileSource>(argv[4]);
        rom->dump = std::make_unique<romlib::Dump>(*rom->src);
        rom->region = std::make_unique<romlib::SubSource>(rom->dump->romfsRegion());
        rom->fs = std::make_unique<romlib::Romfs>(romlib::Romfs::parse(*rom->region));
        std::printf("RomFS: %zu files available\n", rom->fs->listFiles().size());
    }
    Kernel kernel(gm, disp);
    disp.setTraceDepth(48);
    kernel.setUnimplementedPolicy(lenient ? UnimplementedPolicy::ReturnSuccess
                                          : UnimplementedPolicy::ReturnError);
    kernel.setLog([](const std::string &s) { std::printf("[kernel] %s\n", s.c_str()); });
    kernel.configure(KernelConfig{});
    registerDefaultServices(kernel);
    // Writable archives (save data, extdata, SD card) live in PROBE_SAVEDIR (default ./savedata, never
    // committed).
    auto saves =
        std::make_shared<SaveStore>(std::getenv("PROBE_SAVEDIR") ? std::getenv("PROBE_SAVEDIR") : "savedata");
    registerFsService(kernel, rom, saves);
    registerLdrService(kernel);
    auto gpu = registerGspService(kernel);
    auto input = std::make_shared<InputState>();
    registerHidService(kernel, input);
    kernel.createMainThread(kText, kStackTop, 0);
    std::printf("registered %zu blocks; policy for unimplemented IPC: %s\n", disp.blockCount(),
                lenient ? "success" : "error");

    // PROBE_SCRIPT=<file>: timed input, one event per line (instruction counts; '#' starts a comment):
    //   press <at> <duration> <hexKeyMask>
    //   touch <at> <duration> <x> <y>
    //   repeat <from> <to> <period> <duration> <hexKeyMask>     (a key held for <duration> of every <period>)
    struct InputEvent {
        uint64_t start, end;
        uint32_t keys;
        bool touch;
        unsigned x, y;
    };
    std::vector<InputEvent> script;
    if (const char *sf = std::getenv("PROBE_SCRIPT")) {
        std::ifstream in(sf);
        std::string line;
        while (std::getline(in, line)) {
            line = line.substr(0, line.find('#'));
            std::istringstream ls(line);
            std::string cmd;
            if (!(ls >> cmd))
                continue;
            if (cmd == "press") {
                uint64_t at, dur;
                std::string mask;
                if (ls >> at >> dur >> mask)
                    script.push_back({at, at + dur, uint32_t(std::stoul(mask, nullptr, 16)), false, 0, 0});
            } else if (cmd == "touch") {
                uint64_t at, dur;
                unsigned x, y;
                if (ls >> at >> dur >> x >> y)
                    script.push_back({at, at + dur, 0, true, x, y});
            } else if (cmd == "repeat") {
                uint64_t from, to, period, dur;
                std::string mask;
                if (ls >> from >> to >> period >> dur >> mask)
                    for (uint64_t t = from; t < to; t += period)
                        script.push_back({t, t + dur, uint32_t(std::stoul(mask, nullptr, 16)), false, 0, 0});
            }
        }
        std::printf("input script: %zu events\n", script.size());
    }
    // PROBE_PRESS=<period>:<hold>:<hexKeyMask> holds the pad keys for `hold` of every `period` guest
    // instructions (run in 5M-instruction slices so the host can change the input between them).
    uint64_t pressPeriod = 0, pressHold = 0;
    uint32_t pressMask = 0;
    if (const char *ps = std::getenv("PROBE_PRESS"))
        std::sscanf(ps, "%" SCNu64 ":%" SCNu64 ":%x", &pressPeriod, &pressHold, &pressMask);
    // PROBE_PROTECT=<hexAddr>:<hexSize>:<afterInsns> makes a guest range inaccessible once `afterInsns` have
    // run, so the first access faults and the report names the code that touches it.
    uint32_t protAddr = 0, protSize = 0;
    uint64_t protAfter = 0;
    if (const char *pp = std::getenv("PROBE_PROTECT"))
        std::sscanf(pp, "%x:%x:%" SCNu64, &protAddr, &protSize, &protAfter);
    // PROBE_TOUCH=<period>:<hold>:<x>,<y>[;<x>,<y>...] touches the bottom screen for `hold` of every `period`
    // instructions, moving to the next point each period.
    uint64_t touchPeriod = 0, touchHold = 0;
    std::vector<std::pair<unsigned, unsigned>> touchPts;
    if (const char *tp = std::getenv("PROBE_TOUCH")) {
        int used = 0;
        std::sscanf(tp, "%" SCNu64 ":%" SCNu64 ":%n", &touchPeriod, &touchHold, &used);
        for (const char *c = tp + used; *c;) {
            unsigned x = 0, y = 0;
            int n = 0;
            if (std::sscanf(c, "%u,%u%n", &x, &y, &n) < 2)
                break;
            touchPts.push_back({x, y});
            c += n;
            if (*c == ';')
                c++;
        }
    }
    // PROBE_SHOT=<dir>:<everyInsns> writes top/bottom screen PNGs (and always one at the end).
    std::string shotDir;
    uint64_t shotEvery = 0, shotIndex = 0;
    if (const char *sp = std::getenv("PROBE_SHOT")) {
        const std::string spec = sp;
        const size_t c = spec.find(':');
        shotDir = spec.substr(0, c);
        if (c != std::string::npos)
            shotEvery = std::strtoull(spec.c_str() + c + 1, nullptr, 10);
    }
    auto shot = [&](uint64_t at) {
        if (shotDir.empty())
            return;
        for (int sc = 0; sc < 2; sc++) {
            Image img;
            if (gpu->capture(sc, &img)) {
                char name[256];
                std::snprintf(name, sizeof name, "%s/%s_%04" PRIu64 "_%" PRIu64 "M.png", shotDir.c_str(),
                              sc == kTopScreen ? "top" : "bottom", shotIndex, at / 1000000);
                probe::writePng(name, img.width, img.height, img.rgba);
            }
        }
        shotIndex++;
    };
    if (const char *dl = std::getenv("PROBE_DRAWLOG")) { // log the first N draw calls
        static unsigned remaining;
        remaining = unsigned(std::strtoul(dl, nullptr, 10));
        gpu->pica().setDrawLog(
            [](void *, const std::string &s) {
                // identical draw configurations (everything before the vertex-layout part) are counted
                // together
                static std::map<std::string, unsigned> uniq;
                const size_t kc = s.find(" color="), kl = s.find(" | loc=");
                const std::string key = kc == std::string::npos ? s : s.substr(kc, kl - kc);
                if (uniq[key]++ == 0 && remaining) {
                    remaining--;
                    std::printf("[pica] %s\n", s.c_str());
                }
            },
            nullptr);
    }
    // PROBE_RT_AT=<dir>:<n> dumps the render target (as it is before the transfer) at the n-th display
    // transfer.
    if (const char *ra = std::getenv("PROBE_RT_AT")) {
        static std::string dir;
        static unsigned target, count;
        static GuestMemory *gmp;
        dir = ra;
        const size_t c = dir.find(':');
        target = unsigned(std::strtoul(dir.c_str() + c + 1, nullptr, 10));
        dir = dir.substr(0, c);
        count = 0;
        gmp = &gm;
        gpu->onDisplayTransfer = [](const uint32_t *t) {
            if (count++ != target)
                return;
            std::vector<uint8_t> img(256 * 512 * 4);
            for (unsigned y = 0; y < 512; y++)
                for (unsigned x = 0; x < 256; x++) {
                    const Rgba c =
                        decodePixel(gmp->base() + t[0] + tiledOffset(x, 511 - y, 256, 4), PixFmt::RGBA8);
                    uint8_t *d = &img[(size_t(y) * 256 + x) * 4];
                    d[0] = c.r;
                    d[1] = c.g;
                    d[2] = c.b;
                    d[3] = 255;
                }
            char name[300];
            std::snprintf(name, sizeof name, "%s/rt_%u_out%08x.png", dir.c_str(), target, t[3]);
            probe::writePng(name, 256, 512, img);
        };
    }
    if (const char *db =
            std::getenv("PROBE_FRAMEBBOX")) { // print the extent of the triangles drawn before each transfer
        static unsigned from, count = 0, shown = 0;
        static Gpu *g;
        from = unsigned(std::strtoul(db, nullptr, 10));
        g = gpu.get();
        auto prev = gpu->onDisplayTransfer;
        gpu->onDisplayTransfer = [prev](const uint32_t *t) {
            if (prev)
                prev(t);
            pica::PicaStats &st = g->pica().mutableStats();
            if (count++ >= from && shown++ < 6)
                std::printf("[frame] transfer out=%08x tris=%" PRIu64 " frags=%" PRIu64 " draws=%" PRIu64
                            " bbox=(%g,%g)-(%g,%g)\n",
                            t[3], st.triangles, st.fragments, st.draws, st.bboxMin[0], st.bboxMin[1],
                            st.bboxMax[0], st.bboxMax[1]);
            st.bboxMin[0] = st.bboxMin[1] = 1e30f;
            st.bboxMax[0] = st.bboxMax[1] = -1e30f;
        };
    }
    if (const char *td = std::getenv("PROBE_TEXDUMP")) { // PROBE_TEXDUMP=<dir>:<afterInsns>:<count>
        static std::string dir;
        static uint64_t from = 0, num = 0, shown = 0;
        char d[300] = {};
        std::sscanf(td, "%299[^:]:%" SCNu64 ":%" SCNu64, d, &from, &num);
        dir = d;
        gpu->pica().setTextureHook([](int unit, const pica::TexInfo &t, uint32_t env0) {
            if (g_progress < from || shown >= num || t.width > 512 || t.height > 512)
                return;
            std::vector<uint8_t> img(size_t(t.width) * t.height * 4);
            for (unsigned y = 0; y < t.height; y++)
                for (unsigned x = 0; x < t.width; x++) {
                    float c[4];
                    if (!pica::fetchTexel(t, int(x), int(t.height - 1 - y), c))
                        return;
                    for (int k = 0; k < 4; k++)
                        img[(size_t(y) * t.width + x) * 4 + k] =
                            uint8_t(std::min(1.0f, c[k]) * 255.0f + 0.5f);
                }
            char name[200];
            std::snprintf(name, sizeof name, "%s/tex_%04" PRIu64 "_u%d_fmt%u_%ux%u_env%08x.png", dir.c_str(),
                          shown, unit, t.format, t.width, t.height, env0);
            probe::writePng(name, t.width, t.height, img);
            shown++;
        });
    }
    if (const char *dsa = std::getenv("PROBE_DRAWSUM_AFTER")) { // PROBE_DRAWSUM_AFTER=<insns>:<count>
        static uint64_t from = 0, num = 0, shown = 0;
        std::sscanf(dsa, "%" SCNu64 ":%" SCNu64, &from, &num);
        gpu->pica().setDrawLog(
            [](void *, const std::string &s) {
                if (g_progress >= from && shown < num) {
                    std::printf("[sum#%" PRIu64 "] %s\n", shown, s.c_str());
                    shown++;
                }
            },
            nullptr);
    }
    if (const char *ds =
            std::getenv("PROBE_DRAWSUM")) { // PROBE_DRAWSUM=<fromDraw>:<count>: full register summary
        static uint64_t from = 0, num = 0, seen = 0;
        std::sscanf(ds, "%" SCNu64 ":%" SCNu64, &from, &num);
        gpu->pica().setDrawLog(
            [](void *, const std::string &s) {
                if (seen >= from && seen < from + num)
                    std::printf("[sum %" PRIu64 "] %s\n", seen, s.c_str());
                seen++;
            },
            nullptr);
    }
    if (const char *rd =
            std::getenv("PROBE_RT_DRAW")) { // PROBE_RT_DRAW=<dir>:<i>,<j>,...: render target after draws
        static std::string dir;
        static std::vector<uint64_t> which;
        static uint64_t seen = 0;
        static GuestMemory *gmp;
        std::string spec = rd;
        const size_t c = spec.find(':');
        dir = spec.substr(0, c);
        for (size_t p = c + 1; p < spec.size();) {
            const size_t e = spec.find(',', p);
            which.push_back(std::strtoull(spec.c_str() + p, nullptr, 10));
            if (e == std::string::npos)
                break;
            p = e + 1;
        }
        gmp = &gm;
        gpu->pica().setDrawEndLog([](void *, uint32_t, uint64_t, const float *, const float *) {
            for (uint64_t w : which)
                if (w == seen) {
                    std::vector<uint8_t> img(256 * 512 * 4);
                    for (unsigned y = 0; y < 512; y++)
                        for (unsigned x = 0; x < 256; x++) {
                            const Rgba c = decodePixel(
                                gmp->base() + kVramBase + tiledOffset(x, 511 - y, 256, 4), PixFmt::RGBA8);
                            uint8_t *d = &img[(size_t(y) * 256 + x) * 4];
                            d[0] = c.r;
                            d[1] = c.g;
                            d[2] = c.b;
                            d[3] = 255;
                        }
                    char name[300];
                    std::snprintf(name, sizeof name, "%s/draw_%llu.png", dir.c_str(),
                                  (unsigned long long)seen);
                    probe::writePng(name, 256, 512, img);
                }
            seen++;
        });
    }
    if (const char *rs =
            std::getenv("PROBE_RT_SEQ")) { // PROBE_RT_SEQ=<dir>:<fromInsns>:<count>: RT after each draw
        static std::string dir;
        static uint64_t from = 0, num = 0, shown = 0;
        static GuestMemory *gmp;
        char dbuf[300] = {};
        std::sscanf(rs, "%299[^:]:%" SCNu64 ":%" SCNu64, dbuf, &from, &num);
        dir = dbuf;
        gmp = &gm;
        gpu->pica().addDrawEndHook([](uint32_t n, uint64_t frags, const float *mn, const float *mx) {
            if (g_progress < from || shown >= num)
                return;
            std::vector<uint8_t> img(256 * 512 * 4);
            for (unsigned y = 0; y < 512; y++)
                for (unsigned x = 0; x < 256; x++) {
                    const Rgba c =
                        decodePixel(gmp->base() + kVramBase + tiledOffset(x, 511 - y, 256, 4), PixFmt::RGBA8);
                    uint8_t *d = &img[(size_t(y) * 256 + x) * 4];
                    d[0] = c.r;
                    d[1] = c.g;
                    d[2] = c.b;
                    d[3] = 255;
                }
            char name[300];
            std::snprintf(name, sizeof name, "%s/seq_%03llu.png", dir.c_str(), (unsigned long long)shown);
            probe::writePng(name, 256, 512, img);
            std::printf("[seq %03llu] n=%u frags=%llu bbox=(%g,%g)-(%g,%g)\n", (unsigned long long)shown, n,
                        (unsigned long long)frags, mn[0], mn[1], mx[0], mx[1]);
            shown++;
        });
    }
    if (const char *gx = std::getenv("PROBE_GXLOG_AFTER")) { // PROBE_GXLOG_AFTER=<insns>:<count>
        static uint64_t from = 0, num = 0, shown = 0;
        std::sscanf(gx, "%" SCNu64 ":%" SCNu64, &from, &num);
        gpu->onGxCommand = [](uint32_t id, const uint32_t *w) {
            if (g_progress < from || shown >= num)
                return;
            shown++;
            std::printf("[gx@%" PRIu64 "M] cmd %u: %08x %08x %08x %08x %08x %08x %08x\n",
                        g_progress / 1000000, id, w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        };
    }
    if (const char *vx = std::getenv("PROBE_VTX")) { // PROBE_VTX=<insns>:<vertexCount>:<count>
        static Gpu *vtxGpu;
        vtxGpu = gpu.get();
        static uint8_t *vtxMem;
        vtxMem = gm.base();
        static uint64_t from = 0, want = 0, num = 0, shown = 0;
        std::sscanf(vx, "%" SCNu64 ":%" SCNu64 ":%" SCNu64, &from, &want, &num);
        gpu->pica().setVertexHook([](uint32_t n, uint32_t i, const pica::Vec4 *in,
                                     const pica::ClipVertex &cv) {
            if (g_progress < from || n != want || shown >= num)
                return;
            shown++;
            if (shown == 1) { // the command list that carried the latest NaN uniform
                const pica::PicaStats &ps = vtxGpu->pica().stats();
                std::printf("[vtx nanlist] %08x word %u uniform %u\n", ps.lastNanList, ps.lastNanWord,
                            ps.lastNanReg);
                for (uint32_t k = 0; k < 64 && ps.lastNanWord + k >= 20; k++) {
                    uint32_t v;
                    std::memcpy(&v, vtxMem + ps.lastNanList + (ps.lastNanWord - 20 + k) * 4, 4);
                    std::printf("[vtx nl %3u] %08x\n", ps.lastNanWord - 20 + k, v);
                }
            }
            if (shown == 1) { // the shader's constants, entry point and the first words of its program
                const pica::ShaderUnit &su = vtxGpu->pica().shader();
                std::printf("[vtx shader] entry=%u\n", su.entry);
                for (int c = 0; c < 96; c++)
                    std::printf("[vtx c%02d] %g %g %g %g\n", c, su.fconst[c][0], su.fconst[c][1],
                                su.fconst[c][2], su.fconst[c][3]);
                for (int w = 0; w < 24; w++)
                    std::printf("[vtx code %03u] %08x\n", unsigned(su.entry + w), su.code[su.entry + w]);
            }
            std::printf(
                "[vtx n=%u i=%u] in0=(%g %g %g %g) in1=(%g %g %g %g) in2=(%g %g %g %g) clip=(%g %g %g %g)\n",
                n, i, in[0][0], in[0][1], in[0][2], in[0][3], in[1][0], in[1][1], in[1][2], in[1][3],
                in[2][0], in[2][1], in[2][2], in[2][3], cv.pos[0], cv.pos[1], cv.pos[2], cv.pos[3]);
        });
    }
    if (const char *da = std::getenv("PROBE_DRAWEND_AFTER")) { // PROBE_DRAWEND_AFTER=<insns>:<count>
        static uint64_t from = 0, num = 0, shown = 0;
        std::sscanf(da, "%" SCNu64 ":%" SCNu64, &from, &num);
        gpu->pica().setDrawEndLog([](void *, uint32_t n, uint64_t frags, const float *mn, const float *mx) {
            if (g_progress >= from && shown < num) {
                shown++;
                std::printf("[draw@%" PRIu64 "M] n=%u frags=%" PRIu64 " bbox=(%g,%g)-(%g,%g)\n",
                            g_progress / 1000000, n, frags, mn[0], mn[1], mx[0], mx[1]);
            }
        });
    }
    if (const char *dl = std::getenv("PROBE_DRAWEND")) { // PROBE_DRAWEND=<fromDraw>:<count>
        static uint64_t from = 0, num = 0, seen = 0;
        std::sscanf(dl, "%" SCNu64 ":%" SCNu64, &from, &num);
        gpu->pica().setDrawEndLog([](void *, uint32_t n, uint64_t frags, const float *mn, const float *mx) {
            if (seen >= from && seen < from + num)
                std::printf("[draw %" PRIu64 "] n=%u frags=%" PRIu64 " bbox=(%g,%g)-(%g,%g)\n", seen, n,
                            frags, mn[0], mn[1], mx[0], mx[1]);
            seen++;
        });
    }
    // PROBE_RENDER_AFTER=<insns>: rasterize only after this many guest instructions (the vertex stage always
    // runs).
    uint64_t renderAfter = 0;
    if (const char *ra = std::getenv("PROBE_RENDER_AFTER"))
        renderAfter = std::strtoull(ra, nullptr, 10);
    // PROBE_RENDER_WINDOW=<insns>: with PROBE_SHOT, rasterize only during the last <insns> before each
    // screenshot.
    uint64_t renderWindow = 0;
    if (const char *rw = std::getenv("PROBE_RENDER_WINDOW"))
        renderWindow = std::strtoull(rw, nullptr, 10);
    // PROBE_PCSAMPLE=<fromInsns>: histogram of every thread's pc at each 1M-instruction slice (printed at the
    // end).
    uint64_t pcSampleFrom = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> pcHist;
    if (const char *pc = std::getenv("PROBE_PCSAMPLE"))
        pcSampleFrom = std::strtoull(pc, nullptr, 10);
    RunResult rr = RunResult::InstructionLimit;
    if (protSize && protAfter) {
        rr = kernel.run(protAfter);
        gm.protect(protAddr, protSize, GuestMemory::None);
    }
    for (uint64_t done = 0; done < maxInsns && rr == RunResult::InstructionLimit;) {
        // Slices of 5M instructions let the host change input, rasterization and screenshots between them.
        const bool sliced =
            pressPeriod || touchPeriod || shotEvery || renderAfter || !script.empty() || pcSampleFrom;
        const uint64_t n =
            sliced ? std::min<uint64_t>(pcSampleFrom ? 1000000 : 5000000, maxInsns - done) : maxInsns;
        gpu->pica().setRasterEnabled(done >= renderAfter && (!renderWindow || !shotEvery ||
                                                             done % shotEvery >= shotEvery - renderWindow));
        input->keys = pressPeriod && (done % pressPeriod) < pressHold ? pressMask : 0;
        if (touchPeriod && !touchPts.empty()) {
            const auto &pt = touchPts[(done / touchPeriod) % touchPts.size()];
            input->touching = (done % touchPeriod) < touchHold;
            input->touchX = uint16_t(pt.first);
            input->touchY = uint16_t(pt.second);
        }
        if (!script.empty() && !touchPeriod)
            input->touching = false;
        for (const InputEvent &e : script)
            if (done >= e.start && done < e.end) {
                if (e.touch) {
                    input->touching = true;
                    input->touchX = uint16_t(e.x);
                    input->touchY = uint16_t(e.y);
                } else {
                    input->keys |= e.keys;
                }
            }
        g_progress = done;
        if (pcSampleFrom && done >= pcSampleFrom)
            for (uint32_t id = 1; id <= 16; id++)
                if (const RcCtx *c = kernel.threadContext(id))
                    pcHist[{id, c->r[15] | (c->thumb ? 1u : 0u)}]++;
        rr = kernel.run(n);
        done += n;
        if (shotEvery && (done / n) % std::max<uint64_t>(1, shotEvery / n) == 0)
            shot(done);
        if (rr != RunResult::InstructionLimit)
            break;
    }
    shot(maxInsns);
    if (const char *dm = std::getenv("PROBE_DUMPMEM")) { // PROBE_DUMPMEM=<hexAddr>:<hexSize>:<file>
        unsigned a = 0, n = 0;
        char path[400] = {};
        if (std::sscanf(dm, "%x:%x:%399s", &a, &n, path) == 3) {
            std::ofstream o(path, std::ios::binary);
            o.write(reinterpret_cast<const char *>(gm.base() + a), n);
        }
    }
    if (const char *rt = std::getenv("PROBE_DUMP_RT")) { // raw 256x512 RGBA8 render target at VRAM 0x18000000
        std::vector<uint8_t> img(256 * 512 * 4);
        for (unsigned y = 0; y < 512; y++)
            for (unsigned x = 0; x < 256; x++) {
                const Rgba c =
                    decodePixel(gm.base() + kVramBase + tiledOffset(x, 511 - y, 256, 4), PixFmt::RGBA8);
                uint8_t *d = &img[(size_t(y) * 256 + x) * 4];
                d[0] = c.r;
                d[1] = c.g;
                d[2] = c.b;
                d[3] = 255;
            }
        probe::writePng(rt, 256, 512, img);
    }
    for (int sc = 0; sc < 2; sc++) {
        const Gpu::Screen &s = gpu->screen(sc);
        std::printf("screen %d: valid=%d addr=%08x stride=%u fmt=%d\n", sc, s.valid, s.addr, s.stride,
                    int(s.fmt));
    }
    for (int k = 0; k < 2; k++) {
        const uint32_t *t = gpu->stats().lastTransfer[k];
        std::printf("last transfer: src=%08x dst=%08x in=%08x out=%08x flags=%08x\n", t[0], t[1], t[2], t[3],
                    t[4]);
    }
    {
        const GxStats &g = gpu->stats();
        std::printf("gx: fills=%" PRIu64 " transfers=%" PRIu64 " copies=%" PRIu64 " dmas=%" PRIu64
                    " rejected=%" PRIu64 "\n",
                    g.fills, g.transfers, g.copies, g.dmas, g.rejected);
    }
    {
        const pica::PicaStats &p = gpu->pica().stats();
        std::printf(
            "pica: lists=%" PRIu64 " regwrites=%" PRIu64 " draws=%" PRIu64 " skipped=%" PRIu64
            " tris=%" PRIu64 " culled=%" PRIu64 " clipped=%" PRIu64 " fragments=%" PRIu64 " vsRuns=%" PRIu64
            " vsFail=%" PRIu64 " badAddr=%" PRIu64 " jumps=%" PRIu64 " jumpsRejected=%" PRIu64 "\n",
            p.commandLists, p.regWrites, p.draws, p.drawsSkipped, p.triangles, p.culled, p.clipped,
            p.fragments, p.shaderRuns, p.shaderFailures, p.badAddress, p.bufferJumps, p.jumpsRejected);
        for (int u = 0; u < 3; u++) {
            std::printf("pica tex unit %d formats:", u);
            for (int f = 0; f < 16; f++)
                if (p.texFormatDraws[u][f])
                    std::printf(" %d:%" PRIu64, f, p.texFormatDraws[u][f]);
            std::printf("\n");
        }
        std::printf("pica nan uniforms=%" PRIu64 " first: list %08x word %u uniform %u\n", p.nanUniforms,
                    p.firstNanList, p.firstNanWord, p.firstNanReg);
        std::printf("pica unsupported: lighting=%" PRIu64 " procTex=%" PRIu64 " fog=%" PRIu64
                    " shadow=%" PRIu64 " geoShader=%" PRIu64 " texFmt=%" PRIu64 " cube=%" PRIu64
                    " immediate=%" PRIu64 " logicOp=%" PRIu64 "\n",
                    p.unsupportedLighting, p.unsupportedProcTex, p.unsupportedFog, p.unsupportedShadow,
                    p.unsupportedGeometryShader, p.unsupportedTexFormat, p.unsupportedCube,
                    p.unsupportedImmediate, p.unsupportedLogicOp);
    }
    for (const auto &kv : pcHist)
        if (kv.second >= 3)
            std::printf("[pcsample] T%u pc=%08x x%" PRIu64 "\n", kv.first.first, kv.first.second & ~1u,
                        kv.second);
    {
        std::vector<std::pair<uint64_t, std::string>> top;
        for (const auto &kv : kernel.ipcHistogram()) {
            char b[96];
            std::snprintf(b, sizeof b, "%s %08x", kv.first.first.c_str(), kv.first.second);
            top.push_back({kv.second, b});
        }
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 40; i++)
            std::printf("[ipc] %8" PRIu64 "  %s\n", top[i].first, top[i].second.c_str());
    }
    if (std::getenv("PROBE_RECENT_IPC"))
        for (const std::string &l : kernel.recentIpc())
            std::printf("[recent ipc] %s\n", l.c_str());
    static const char *names[] = {
        "all threads exited", "deadlock", "instruction limit", "process exit", "break", "trap", "fault"};
    std::printf("result: %s\n", names[int(rr)]);
    if (rr == RunResult::Fault) {
        std::printf("  guest fault at %08x\n  recent block pcs:", kernel.lastFault().address);
        for (uint32_t pc : disp.recentPcs())
            std::printf(" %08x", pc & ~1u);
        std::printf("\n");
    }
    if (rr == RunResult::Break)
        std::printf("  svcBreak reason %u\n", kernel.breakReason());

    if (const char *sc = std::getenv(
            "PROBE_SCAN")) { // PROBE_SCAN=w0,w1,... (hex): guest addresses holding that word sequence
        std::vector<uint32_t> pat;
        for (const char *c = sc; *c;) {
            char *e;
            pat.push_back(uint32_t(std::strtoul(c, &e, 16)));
            c = *e ? e + 1 : e;
        }
        unsigned found = 0;
        for (uint64_t a = 0x00100000; a + pat.size() * 4 <= 0x50000000 && found < 40; a += 4)
            if (!std::memcmp(gm.base() + a, pat.data(), pat.size() * 4)) {
                std::printf("scan hit at %08x\n", unsigned(a));
                found++;
            }
    }
    if (const char *lg = std::getenv("PROBE_LITREGS")) { // like PROBE_REGS, at the latest lit draw
        for (const char *c = lg; *c;) {
            unsigned lo = 0, hi = 0;
            int n = 0;
            if (std::sscanf(c, "%x-%x%n", &lo, &hi, &n) < 2)
                break;
            for (unsigned r = lo; r <= hi && r < 0x300; r++)
                std::printf("[litreg %03x] %08x\n", r, gpu->pica().litSnapshot()[r]);
            c += n;
            if (*c == ',')
                c++;
        }
    }
    if (const char *rg =
            std::getenv("PROBE_REGS")) { // PROBE_REGS=<hexLo>-<hexHi>[,..]: final PICA register values
        for (const char *c = rg; *c;) {
            unsigned lo = 0, hi = 0;
            int n = 0;
            if (std::sscanf(c, "%x-%x%n", &lo, &hi, &n) < 2)
                break;
            for (unsigned r = lo; r <= hi && r < 0x300; r++)
                std::printf("[reg %03x] %08x\n", r, gpu->pica().reg(r));
            c += n;
            if (*c == ',')
                c++;
        }
    }
    if (const char *w = std::getenv("PROBE_WORDS")) { // comma-separated guest addresses (hex) to print
        std::string list = w;
        size_t pos = 0;
        while (pos < list.size()) {
            const size_t e = list.find(',', pos);
            const uint32_t a = uint32_t(std::strtoul(list.substr(pos, e - pos).c_str(), nullptr, 16));
            uint32_t v = 0;
            std::memcpy(&v, gm.base() + a, 4);
            std::printf("word %08x = %08x\n", a, v);
            if (e == std::string::npos)
                break;
            pos = e + 1;
        }
    }
    if (const char *tp = std::getenv("PROBE_TRACE")) { // full deterministic kernel trace to a file
        std::ofstream t(tp);
        for (const auto &l : kernel.trace())
            t << l << "\n";
    }
    for (const auto &d : kernel.describeThreads())
        std::printf("  %s\n", d.c_str());
    for (uint32_t id = 1; id <= 16; id++)
        if (const RcCtx *c = kernel.threadContext(id)) {
            std::printf("thread %u: pc=%08x", id, c->r[15]);
            for (int k = 0; k < 15; k++)
                std::printf(" r%d=%08x", k, c->r[k]);
            std::printf("\n");
            if (id == 1) { // stack words that look like code addresses (return-address candidates)
                std::printf("  T1 stack code refs:");
                for (uint32_t a = c->r[13]; a < c->r[13] + 0x400 && a < kStackTop; a += 4) {
                    uint32_t v;
                    std::memcpy(&v, gm.base() + a, 4);
                    if (v >= kText && v < 0x006D0000)
                        std::printf(" %08x", v);
                }
                std::printf("\n");
            }
        }
    {
        const auto &tr = kernel.trace();
        std::printf("last trace lines:\n");
        for (size_t k = tr.size() > 14 ? tr.size() - 14 : 0; k < tr.size(); k++)
            std::printf("  | %s\n", tr[k].c_str());
    }
    // trace summary: SVC histogram and the sequence of IPC requests
    std::map<std::string, unsigned> svcHist;
    std::vector<std::string> ipc;
    for (const auto &t : kernel.trace()) {
        if (t.rfind("svc ", 0) == 0)
            svcHist[t.substr(4, 4)]++;
        else if (t.rfind("ipc ", 0) == 0 || t.rfind("srv ", 0) == 0)
            ipc.push_back(t);
    }
    {
        unsigned shown = 0;
        for (const auto &t : kernel.trace())
            if ((t.rfind("create", 0) == 0 || t.rfind("arbitrate", 0) == 0) && shown++ < 20)
                std::printf("  arb: %s\n", t.c_str());
    }
    std::printf("svcs=%" PRIu64 " ipc=%" PRIu64 " thread switches=%" PRIu64 " virtual ticks=%" PRIu64 "\n",
                kernel.stats().svcs, kernel.stats().ipcRequests, kernel.stats().switches, kernel.ticks());
    for (auto &kv : svcHist)
        std::printf("  svc %s x%u\n", kv.first.c_str(), kv.second);
    std::printf("first IPC requests:\n");
    for (size_t k = 0; k < ipc.size() && k < 40; k++)
        std::printf("  %s\n", ipc[k].c_str());
    const auto un = kernel.unimplementedCommands();
    std::printf("unimplemented service commands: %zu\n", un.size());
    for (auto &u : un)
        std::printf("  %s %08x\n", u.first.c_str(), u.second);
    // Fallback run starts (pcs whose predecessor was not also interpreted) become extra block entries on
    // the next translate run: the "log hits to improve coverage" loop.
    {
        const auto &hits = disp.fallbackHits();
        std::ofstream o("fallback_entries.txt");
        size_t n = 0;
        for (const auto &kv : hits) {
            const uint32_t pc = kv.first & ~1u;
            const bool thumb = kv.first & 1;
            const bool prev =
                hits.count((pc - 4) | (thumb ? 1u : 0u)) || (thumb && hits.count((pc - 2) | 1u));
            if (!prev) {
                o << std::hex << pc << " " << (thumb ? 1 : 0) << "\n";
                n++;
            }
        }
        std::printf("wrote %zu fallback entry points to fallback_entries.txt\n", n);
    }
    const Coverage &cv = disp.coverage();
    std::printf("coverage: %" PRIu64 " recompiled, %" PRIu64 " interpreted insns (%.2f%%), %" PRIu64
                " blocks run\n",
                cv.recompiledInsns, cv.interpretedInsns, cv.percentRecompiled(), cv.blocksRun);
    for (const auto &t : disp.topFallbacks(10))
        std::printf("  fallback pc %08x%s x%" PRIu64 "\n", t.first & ~1u, (t.first & 1) ? " (thumb)" : "",
                    t.second);
    return 0;
}
