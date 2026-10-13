// recomp_translate <code.bin> <funcmap.tsv> <classification.json> <outdir> [blocksPerFile]
// Local-only pipeline: discover blocks and emit C. The output is derived from game code and must never be
// committed (docs/legal-policy.md); keep <outdir> outside the repository or in an ignored directory.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "recomp/blocks.h"

namespace {
constexpr uint32_t kBase = 0x100000;
struct CodeImage : recomp::ImageReader {
    std::vector<uint8_t> data;
    bool read(uint32_t a, void *d, size_t n) const override {
        if (a < kBase || uint64_t(a - kBase) + n > data.size())
            return false;
        std::memcpy(d, &data[a - kBase], n);
        return true;
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc < 5)
        return 2;
    CodeImage img;
    {
        std::ifstream f(argv[1], std::ios::binary);
        img.data.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::vector<recomp::Entry> entries;
    {
        std::ifstream m(argv[2]);
        std::string line;
        std::getline(m, line);
        while (std::getline(m, line)) {
            std::istringstream in(line);
            std::string a, s, mode;
            in >> a >> s >> mode;
            entries.push_back({uint32_t(std::stoul(a, nullptr, 16)), mode == "thumb"});
        }
    }
    // literal pools from classification.json
    std::vector<std::pair<uint32_t, uint32_t>> pools;
    {
        std::ifstream f(argv[3]);
        std::string json((std::istreambuf_iterator<char>(f)), {});
        const size_t k = json.find("\"literal_pool_ranges\":");
        size_t p = json.find('[', k);
        int depth = 0;
        std::vector<uint32_t> nums;
        for (; p < json.size(); p++) {
            const char c = json[p];
            if (c == '[')
                depth++;
            else if (c == ']') {
                if (--depth == 0)
                    break;
            } else if (c >= '0' && c <= '9') {
                uint32_t n = 0;
                while (json[p] >= '0' && json[p] <= '9')
                    n = n * 10 + uint32_t(json[p++] - '0');
                nums.push_back(n);
                p--;
            }
        }
        for (size_t i = 0; i + 1 < nums.size(); i += 2)
            pools.push_back({nums[i], nums[i + 1]});
    }
    recomp::DiscoverOptions opt;
    opt.isData = [&](uint32_t a) {
        size_t lo = 0, hi = pools.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (a < pools[mid].first)
                hi = mid;
            else if (a >= pools[mid].second)
                lo = mid + 1;
            else
                return true;
        }
        return false;
    };
    recomp::DiscoverStats st;
    const auto blocks = recomp::discoverBlocks(img, entries, opt, &st);
    const size_t per = argc >= 6 ? size_t(std::stoul(argv[5])) : 2000;
    mkdir(argv[4], 0755);
    size_t files = 0;
    for (size_t b = 0; b < blocks.size(); b += per, files++) {
        char name[512];
        std::snprintf(name, sizeof name, "%s/blocks_%04zu.c", argv[4], files);
        std::ofstream(name) << recomp::emitBlockFunctionsC(blocks, b, b + per);
    }
    {
        std::string name = std::string(argv[4]) + "/block_table.c";
        std::ofstream(name) << recomp::emitBlockTableC(blocks);
    }
    std::printf("blocks=%zu insns=%zu files=%zu jump_tables=%zu\n", st.blocks, st.insns, files,
                st.jumpTables);
    return 0;
}
