// Host-backed save store: confinement of guest paths and the basic file/directory operations.
#include <cstdio>
#include <fstream>

#include "pokerecomp/save_store.h"

using namespace pokerecomp;
namespace fs = std::filesystem;
using Err = SaveStore::Err;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

int main() {
    const fs::path root = fs::temp_directory_path() / "pokerecomp_save_store_test";
    fs::remove_all(root);
    SaveStore st(root, 1 << 20);

    fs::path a;
    CHECK(st.archiveDir("extdata_01_0000000000001b50", false, &a) == Err::NotFound); // not there yet
    CHECK(st.archiveDir("extdata_01_0000000000001b50", true, &a) == Err::Ok && SaveStore::isDirectory(a));
    CHECK(st.archiveDir("../evil", true, &a) == Err::Invalid &&
          st.archiveDir("a/b", true, &a) == Err::Invalid);
    CHECK(st.archiveDir("a:b", true, &a) == Err::Invalid && st.archiveDir("", true, &a) == Err::Invalid);
    CHECK(st.archiveDir("extdata_01_0000000000001b50", false, &a) == Err::Ok);

    // path confinement
    fs::path p;
    CHECK(SaveStore::resolve(a, "/data/file.bin", &p) == Err::Ok && p == a / "data" / "file.bin");
    CHECK(SaveStore::resolve(a, "data//./file.bin", &p) == Err::Ok && p == a / "data" / "file.bin");
    CHECK(SaveStore::resolve(a, "/", &p) == Err::Ok && p == a);
    CHECK(SaveStore::resolve(a, "/../x", &p) == Err::Invalid);
    CHECK(SaveStore::resolve(a, "/a/../../x", &p) == Err::Invalid);
    CHECK(SaveStore::resolve(a, "/a\\b", &p) == Err::Invalid);
    CHECK(SaveStore::resolve(a, std::string("/a\0b", 4), &p) == Err::Invalid);
    CHECK(SaveStore::resolve(a, "/c:/windows", &p) == Err::Invalid);
    CHECK(SaveStore::resolve(a, "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17", &p) ==
          Err::Invalid); // too deep

    // directories and files
    fs::path d, f;
    CHECK(SaveStore::resolve(a, "/data", &d) == Err::Ok && SaveStore::createDirectory(d) == Err::Ok);
    CHECK(SaveStore::createDirectory(d) == Err::Exists);
    fs::path orphan;
    SaveStore::resolve(a, "/nodir/sub", &orphan);
    CHECK(SaveStore::createDirectory(orphan) == Err::NotFound); // parent missing
    SaveStore::resolve(a, "/data/file.bin", &f);
    CHECK(SaveStore::createFile(f, 100, st.maxFileBytes()) == Err::Ok && fs::file_size(f) == 100);
    CHECK(SaveStore::createFile(f, 100, st.maxFileBytes()) == Err::Exists);
    fs::path big;
    SaveStore::resolve(a, "/data/big.bin", &big);
    CHECK(SaveStore::createFile(big, (1 << 20) + 1, st.maxFileBytes()) == Err::TooBig &&
          !SaveStore::isFile(big));
    fs::path g;
    SaveStore::resolve(a, "/data/b.bin", &g);
    CHECK(SaveStore::rename(f, g) == Err::Ok && SaveStore::isFile(g) && !SaveStore::isFile(f));
    CHECK(SaveStore::rename(f, g) == Err::NotFound);
    std::vector<SaveStore::Entry> list;
    CHECK(SaveStore::list(d, &list) == Err::Ok && list.size() == 1 && list[0].name == "b.bin" &&
          list[0].size == 100);
    CHECK(SaveStore::deleteDirectory(d, false) == Err::NotEmpty);
    CHECK(SaveStore::deleteFile(g) == Err::Ok && SaveStore::deleteFile(g) == Err::NotFound);
    CHECK(SaveStore::deleteDirectory(d, false) == Err::Ok && !SaveStore::isDirectory(d));
    SaveStore::resolve(a, "/x", &d);
    SaveStore::createDirectory(d);
    SaveStore::resolve(a, "/x/y", &p);
    SaveStore::createDirectory(p);
    CHECK(SaveStore::deleteDirectory(d, true) == Err::Ok && !SaveStore::isDirectory(d));

    fs::remove_all(root);
    std::printf(g_fail ? "FAIL\n" : "save_store ok\n");
    return g_fail ? 1 : 0;
}
