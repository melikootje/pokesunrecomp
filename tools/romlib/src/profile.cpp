#include "romlib/profile.h"

#include <fstream>
#include <sstream>

#include "romlib/sha256.h"

namespace romlib {
namespace {
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

TitleProfile TitleProfile::parse(const std::string &text) {
    TitleProfile p;
    bool haveId = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw Error("profile: malformed line: " + line);
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        try {
            if (k == "name")
                p.name = v;
            else if (k == "title_id") {
                p.titleId = std::stoull(v, nullptr, 16);
                haveId = true;
            } else if (k == "code_sha256")
                p.codeSha256 = lower(v);
            else if (k == "exheader_sha256")
                p.exheaderSha256 = lower(v);
            else if (k == "code_size")
                p.codeSize = std::stoull(v);
            else
                throw Error("profile: unknown key " + k);
        } catch (const std::logic_error &) {
            throw Error("profile: bad value for " + k);
        }
    }
    if (!haveId)
        throw Error("profile: missing title_id");
    return p;
}

TitleProfile TitleProfile::load(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        throw Error("cannot open profile " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

const TitleProfile *selectProfile(const std::vector<TitleProfile> &profiles, uint64_t titleId) {
    for (const auto &p : profiles)
        if (p.titleId == titleId)
            return &p;
    return nullptr;
}

void verifyAgainstProfile(const TitleProfile &p, const Bytes &code, const Bytes &exheaderRaw) {
    if (p.codeSize && code.size() != p.codeSize)
        throw Error("unsupported build: .code size " + std::to_string(code.size()) + " != profile " +
                    std::to_string(p.codeSize) + " (wrong version or missing update?)");
    if (!p.codeSha256.empty() && sha256Hex(code.data(), code.size()) != p.codeSha256)
        throw Error("unsupported build: .code hash does not match profile '" + p.name +
                    "' (wrong version or missing update?)");
    if (!p.exheaderSha256.empty() && sha256Hex(exheaderRaw.data(), exheaderRaw.size()) != p.exheaderSha256)
        throw Error("unsupported build: exheader hash does not match profile '" + p.name + "'");
}

} // namespace romlib
