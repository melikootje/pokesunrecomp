#pragma once
#include "romlib/common.h"

namespace romlib {
// Nintendo "backward LZ" (as used for the ExeFS .code of compressed titles). Throws Error on
// malformed input. Note: this is not LZ11.
Bytes blzDecompress(const Bytes &in);
} // namespace romlib
