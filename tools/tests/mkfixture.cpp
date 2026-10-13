// Writes synthetic fixtures for CLI tests: mkfixture <dir>
#include <fstream>

#include "fixtures.h"

static void save(const std::string &p, const fx::Bytes &b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::string d = argv[1];
    fx::NcchSpec s;
    for (size_t i = 0; i < 9000; i++)
        s.code.push_back(uint8_t(i % 7 + (i / 300)));
    s.romfs = {{"/hello.txt", {'h', 'e', 'l', 'l', 'o'}}, {"/d/x.bin", fx::Bytes(300, 7)}};
    save(d + "/test.cci", fx::buildCci(fx::buildNcch(s)));
    fx::NcchSpec u = s;
    u.code.push_back(0xEE);
    u.romfs = {{"/hello.txt", {'u', 'p'}}};
    save(d + "/update.cxi", fx::buildNcch(u));
    s.encrypted = true;
    save(d + "/encrypted.cxi", fx::buildNcch(s));
    return 0;
}
