#pragma once
#include "romlib/common.h"

namespace romlib {
std::string sha256Hex(const uint8_t *data, size_t len);
// Streams a source in chunks so multi-GiB inputs are never fully resident.
std::string sha256Hex(const Source &src, uint64_t off, uint64_t len);
} // namespace romlib
