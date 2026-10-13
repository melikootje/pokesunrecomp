#pragma once
#include <map>

#include "romlib/common.h"

namespace romlib {

// A title profile is a tiny `key=value` text file (no game data), e.g. profiles/ultrasun-usa.profile.
// Keys: name, title_id (16 hex), code_sha256 (of decompressed .code), exheader_sha256 (optional),
// code_size (decimal, optional).
struct TitleProfile {
    std::string name;
    uint64_t titleId = 0;
    std::string codeSha256;
    std::string exheaderSha256;
    uint64_t codeSize = 0;

    static TitleProfile parse(const std::string &text);
    static TitleProfile load(const std::string &path);
};

// Selects a profile by title ID; nullptr when unknown.
const TitleProfile *selectProfile(const std::vector<TitleProfile> &profiles, uint64_t titleId);

// Throws Error with a clear message when the extracted code/exheader do not match the profile
// (unsupported build or unapplied update).
void verifyAgainstProfile(const TitleProfile &p, const Bytes &code, const Bytes &exheaderRaw);

} // namespace romlib
