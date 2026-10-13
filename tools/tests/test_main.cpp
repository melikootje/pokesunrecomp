#include <cstdio>
#include <functional>

#include "fixtures.h"
#include "romlib/blz.h"
#include "romlib/cro.h"
#include "romlib/dump.h"
#include "romlib/profile.h"
#include "romlib/sha256.h"

using namespace romlib;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)
#define THROWS(expr)                                                                                         \
    do {                                                                                                     \
        bool t_ = false;                                                                                     \
        try {                                                                                                \
            expr;                                                                                            \
        } catch (const Error &) {                                                                            \
            t_ = true;                                                                                       \
        }                                                                                                    \
        CHECK(t_);                                                                                           \
    } while (0)

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = (i % 97 < 60) ? uint8_t(i % 13) : uint8_t(x >> 24);
    }
    return b;
}

static void testSha() {
    auto h = [](const std::string &s) {
        return sha256Hex(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    };
    CHECK(h("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(h("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(h("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Bytes mil(1000000, 'a');
    MemorySource ms(mil);
    CHECK(sha256Hex(ms, 0, mil.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void testBlz() {
    for (size_t prefix : {size_t(0), size_t(16)}) {
        Bytes raw = pattern(20000, 7);
        Bytes c = fx::blzCompress(raw, prefix);
        CHECK(c.size() < raw.size());
        CHECK(blzDecompress(c) == raw);
    }
    Bytes tiny = pattern(64, 3);
    CHECK(blzDecompress(fx::blzCompress(tiny, 0)) == tiny);
    // malformed
    THROWS(blzDecompress(Bytes{1, 2, 3}));
    Bytes c = fx::blzCompress(pattern(5000, 1), 0);
    Bytes bad = c;
    bad[bad.size() - 5] = 0; // top = 0
    THROWS(blzDecompress(bad));
    bad = c;
    fx::w32(bad, bad.size() - 4, 0xFFFFFFFFu);
    THROWS(blzDecompress(bad));
    bad = c;
    bad.erase(bad.begin(), bad.begin() + 10); // truncated stream
    THROWS(blzDecompress(bad));
}

static fx::NcchSpec baseSpec() {
    fx::NcchSpec s;
    s.code = pattern(30000, 11);
    s.romfs = {{"/a.bin", pattern(100, 1)},
               {"/dir/b.bin", pattern(5000, 2)},
               {"/dir/sub/c.txt", {'h', 'i'}},
               {"/empty", {}}};
    return s;
}

static void testContainerRoundTrip(bool compress, bool cci) {
    fx::NcchSpec s = baseSpec();
    s.compress = compress;
    Bytes ncch = fx::buildNcch(s);
    MemorySource src(cci ? fx::buildCci(ncch) : ncch);
    Dump d(src);
    CHECK(d.app().programId == s.programId);
    CHECK(d.app().productCode == "CTR-P-TEST");
    ExHeader eh = d.app().exHeader(src);
    CHECK(eh.appTitle == "TESTAPP");
    CHECK(eh.compressExefsCode == compress);
    CHECK(eh.text.address == 0x00100000);
    CHECK(eh.stackSize == 0x4000);
    CHECK(eh.bssSize == 0x2000);
    CHECK(eh.services.size() == 3 && eh.services[1] == "fs:USER");
    CHECK(eh.kernelCaps.size() == 1);

    SubSource er = d.exefsRegion();
    Exefs fs = Exefs::parse(er);
    CHECK(fs.entries().size() == 2);
    CHECK(fs.code(eh.compressExefsCode) == s.code);
    CHECK(fs.read("icon") == Bytes(0x40, 0x11));
    THROWS(fs.read("banner"));

    SubSource rr = d.romfsRegion();
    Romfs rf = Romfs::parse(rr);
    CHECK(rf.listFiles().size() == 4);
    CHECK(rf.readAll("/dir/b.bin") == s.romfs[1].data);
    CHECK(rf.readAll("/dir/sub/c.txt") == s.romfs[2].data);
    CHECK(rf.readAll("/empty").empty());
    CHECK(rf.read("/dir/b.bin", 10, 20) == Bytes(s.romfs[1].data.begin() + 10, s.romfs[1].data.begin() + 30));
    THROWS(rf.read("/a.bin", 90, 20));
    THROWS(rf.readAll("/nope"));
    CHECK(rf.listDirs().size() == 2);
}

static void testRejections() {
    fx::NcchSpec s = baseSpec();
    s.encrypted = true;
    MemorySource enc(fx::buildNcch(s));
    bool msgOk = false;
    try {
        Dump d(enc);
    } catch (const Error &e) {
        msgOk = std::string(e.what()).find("decrypted") != std::string::npos;
    }
    CHECK(msgOk);

    MemorySource junk(Bytes(0x400, 0x5A));
    THROWS(Dump d(junk));
    MemorySource tiny(Bytes(10, 0));
    THROWS(Dump d(tiny));

    // truncated dump: partition claims more than the file holds
    Bytes cci = fx::buildCci(fx::buildNcch(baseSpec()));
    cci.resize(cci.size() / 2);
    MemorySource trunc(cci);
    THROWS(Dump d(trunc));

    // corrupt exefs entry
    Bytes n = fx::buildNcch(baseSpec());
    MemorySource nsrc0(n);
    Dump d0(nsrc0);
    size_t exefsAbs = d0.app().exefsOff;
    fx::w32(n, exefsAbs + 12, 0x7FFFFFFF);
    MemorySource nsrc(n);
    Dump d(nsrc);
    SubSource er = d.exefsRegion();
    THROWS(Exefs::parse(er));

    // corrupt romfs: bad magic and out-of-range file
    Bytes n2 = fx::buildNcch(baseSpec());
    MemorySource s2(n2);
    Dump d2(s2);
    n2[d2.app().romfsOff] = 'X';
    MemorySource s3(n2);
    Dump d3(s3);
    SubSource r3 = d3.romfsRegion();
    THROWS(Romfs::parse(r3));
}

// A 4 GiB logical source that only materializes explicitly written segments, to prove ingestion is lazy.
class SparseSource : public Source {
  public:
    explicit SparseSource(uint64_t size) : size_(size) {}
    void put(uint64_t off, const Bytes &b) { segs_[off] = b; }
    uint64_t size() const override { return size_; }
    void read(uint64_t off, void *dst, size_t len) const override {
        if (off > size_ || len > size_ - off)
            throw Error("sparse: past end");
        bytesRead += len;
        std::memset(dst, 0, len);
        for (const auto &[so, sb] : segs_) {
            uint64_t a = std::max<uint64_t>(off, so), b = std::min<uint64_t>(off + len, so + sb.size());
            if (a < b)
                std::memcpy(static_cast<uint8_t *>(dst) + (a - off), &sb[a - so], b - a);
        }
    }
    mutable uint64_t bytesRead = 0;

  private:
    uint64_t size_;
    std::map<uint64_t, Bytes> segs_;
};

static void testLazyFourGiB() {
    fx::NcchSpec s = baseSpec();
    Bytes ncch = fx::buildNcch(s);
    Bytes cci = fx::buildCci(ncch);
    // declare the whole 4 GiB in the NCSD header so the table validates against file size
    fx::w32(cci, 0x104, uint32_t((4ull << 30) / 0x200));
    SparseSource src(4ull << 30);
    src.put(0, cci);
    src.bytesRead = 0;
    Dump d(src);
    SubSource rr = d.romfsRegion();
    Romfs rf = Romfs::parse(rr);
    CHECK(rf.readAll("/a.bin") == s.romfs[0].data);
    CHECK(src.bytesRead < (1u << 20));
}

static void testProfiles() {
    fx::NcchSpec s = baseSpec();
    MemorySource src(fx::buildNcch(s));
    Dump d(src);
    SubSource er = d.exefsRegion();
    Bytes code = Exefs::parse(er).code(true);
    Bytes exh = d.exHeaderRaw();
    std::string good =
        "# test\nname = Test\ntitle_id = 0004000000ABCD00\ncode_size = " + std::to_string(code.size()) +
        "\ncode_sha256 = " + sha256Hex(code.data(), code.size()) +
        "\nexheader_sha256 = " + sha256Hex(exh.data(), exh.size()) + "\n";
    TitleProfile p = TitleProfile::parse(good);
    CHECK(p.titleId == 0x0004000000ABCD00ull);
    verifyAgainstProfile(p, code, exh); // no throw
    std::vector<TitleProfile> v{p};
    CHECK(selectProfile(v, p.titleId) == &v[0]);
    CHECK(selectProfile(v, 1) == nullptr);

    Bytes wrong = code;
    wrong[5] ^= 1;
    THROWS(verifyAgainstProfile(p, wrong, exh));
    Bytes shorter(code.begin(), code.end() - 1);
    THROWS(verifyAgainstProfile(p, shorter, exh));
    Bytes exh2 = exh;
    exh2[0x100] ^= 1;
    THROWS(verifyAgainstProfile(p, code, exh2));
    THROWS(TitleProfile::parse("name=x\n"));
    THROWS(TitleProfile::parse("title_id=zz\n"));
    THROWS(TitleProfile::parse("title_id=1\nbogus=1\n"));
    THROWS(TitleProfile::parse("garbage\n"));
}

static void testOverlay() {
    fx::NcchSpec base = baseSpec();
    fx::NcchSpec upd = baseSpec();
    upd.code = pattern(31000, 99);
    upd.romfs = {{"/a.bin", {1, 2, 3}}, {"/new.bin", {9}}};
    MemorySource bs(fx::buildNcch(base)), us(fx::buildNcch(upd));
    Dump bd(bs), ud(us);
    SubSource br = bd.romfsRegion(), ur = ud.romfsRegion();
    Romfs brf = Romfs::parse(br), urf = Romfs::parse(ur);
    Overlay o{&brf, &urf};
    CHECK(o.readAll("/a.bin") == Bytes({1, 2, 3}));       // update wins
    CHECK(o.readAll("/dir/b.bin") == base.romfs[1].data); // falls back to base
    CHECK(o.readAll("/new.bin") == Bytes({9}));
    CHECK(!o.exists("/missing"));
    THROWS(o.readAll("/missing"));
}

static void testCro() {
    Bytes b(0x300, 0);
    auto w = [&](uint32_t o, uint32_t v) {
        for (int i = 0; i < 4; i++)
            b[o + i] = uint8_t(v >> (8 * i));
    };
    std::memcpy(&b[0x80], "CRO0", 4);
    w(0x90, 0x300);
    w(0x94, 0x40);
    w(0xB0, 0x200);
    w(0xB4, 0x20);
    w(0xC0, 0x2F0);
    memcpy(&b[0x2F0], "mod", 4);
    w(0xC8, 0x140); // segment table
    w(0xCC, 2);
    w(0x140, 0x200);
    w(0x144, 0x20);
    w(0x148, 0);
    w(0x14C, 0);
    w(0x150, 0x40);
    w(0x154, 3);
    w(0x128, 0x160); // internal relocations
    w(0x12C, 1);
    w(0x160, (0x10u << 4) | 0);
    b[0x164] = 2;
    w(0x168, 0x20);
    Cro c = Cro::parse(b);
    CHECK(!c.isCrs && c.name() == "mod");
    CHECK(c.segments.size() == 2 && c.segments[1].type == 3 && c.segments[1].size == 0x40);
    CHECK(c.internalRelocs.size() == 1 && c.internalRelocs[0].type == 2 && c.internalRelocs[0].value == 0x20);
    CHECK(c.internalRelocs[0].target == 0x100);
    w(0xCC, 0x10000000); // segment table far beyond the file
    bool threw = false;
    try {
        Cro::parse(b);
    } catch (const Error &) {
        threw = true;
    }
    CHECK(threw);
    b[0x80] = 'X';
    threw = false;
    try {
        Cro::parse(b);
    } catch (const Error &) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    struct T {
        const char *n;
        std::function<void()> f;
    };
    T tests[] = {
        {"sha256", testSha},
        {"blz", testBlz},
        {"ncch compressed", [] { testContainerRoundTrip(true, false); }},
        {"ncch uncompressed", [] { testContainerRoundTrip(false, false); }},
        {"cci compressed", [] { testContainerRoundTrip(true, true); }},
        {"rejections", testRejections},
        {"lazy 4GiB", testLazyFourGiB},
        {"profiles", testProfiles},
        {"overlay", testOverlay},
        {"cro", testCro},
    };
    for (auto &t : tests) {
        int before = g_fail;
        try {
            t.f();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "  exception: %s\n", e.what());
            g_fail++;
        }
        std::printf("[%s] %s\n", g_fail == before ? " ok " : "FAIL", t.n);
    }
    return g_fail ? 1 : 0;
}
