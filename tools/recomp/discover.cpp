// recomp_discover <code.bin> <funcmap.tsv> <classification.json> [out_jump_tables.json]
// Local analysis tool: statically discovers basic blocks over a code image (base 0x100000) and reports
// counts. Outputs only addresses and counts; it never prints or stores instruction bytes.
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

// Pulls the [[start,end],...] pairs of "literal_pool_ranges" out of classification.json.
std::vector<std::pair<uint32_t, uint32_t>> poolRanges(const std::string &json) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    const size_t k = json.find("\"literal_pool_ranges\":");
    if (k == std::string::npos)
        return v;
    size_t p = json.find('[', k);
    int depth = 0;
    std::vector<uint32_t> nums;
    for (; p < json.size(); p++) {
        const char ch = json[p];
        if (ch == '[')
            depth++;
        else if (ch == ']') {
            if (--depth == 0)
                break;
        } else if (ch >= '0' && ch <= '9') {
            uint32_t n = 0;
            while (json[p] >= '0' && json[p] <= '9')
                n = n * 10 + uint32_t(json[p++] - '0');
            nums.push_back(n);
            p--;
        }
    }
    for (size_t i = 0; i + 1 < nums.size(); i += 2)
        v.push_back({nums[i], nums[i + 1]});
    return v;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 4)
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
            std::string a, s, mode, th;
            in >> a >> s >> mode >> th;
            entries.push_back({uint32_t(std::stoul(a, nullptr, 16)), mode == "thumb"});
        }
    }
    std::string cls;
    {
        std::ifstream f(argv[3]);
        cls.assign(std::istreambuf_iterator<char>(f), {});
    }
    const auto pools = poolRanges(cls);
    recomp::DiscoverOptions opt;
    opt.isData = [&](uint32_t a) {
        // pools are sorted and disjoint: binary search
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
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> tables;
    opt.onJumpTable = [&](uint32_t site, const std::vector<uint32_t> &t) { tables.push_back({site, t}); };
    recomp::DiscoverStats st;
    const auto blocks = recomp::discoverBlocks(img, entries, opt, &st);
    size_t thumbBlocks = 0, maxLen = 0;
    for (const auto &b : blocks) {
        thumbBlocks += b.thumb;
        maxLen = std::max(maxLen, b.insns.size());
    }
    std::printf("entries=%zu pool_ranges=%zu blocks=%zu (thumb %zu) insns=%zu max_block=%zu\n",
                entries.size(), pools.size(), st.blocks, thumbBlocks, st.insns, maxLen);
    std::printf("jump_tables=%zu targets=%zu unresolved_indirect=%zu\n", st.jumpTables, st.jumpTableTargets,
                st.unresolvedIndirect);
    if (argc >= 5) {
        std::ofstream o(argv[4]);
        o << "[";
        for (size_t i = 0; i < tables.size(); i++) {
            o << (i ? "," : "") << "{\"site\":" << tables[i].first << ",\"targets\":[";
            for (size_t k = 0; k < tables[i].second.size(); k++)
                o << (k ? "," : "") << tables[i].second[k];
            o << "]}";
        }
        o << "]";
    }
    return 0;
}
