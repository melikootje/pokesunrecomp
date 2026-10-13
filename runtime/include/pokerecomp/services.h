#pragma once
// HLE system services (issue #10). Each service implements the commands the game uses; anything else falls
// through to the logged "unimplemented" path in the kernel.
#include "pokerecomp/kernel.h"

namespace pokerecomp {

struct ServiceOptions {
    uint32_t language = 1; // cfg: system language (1 = English)
    uint32_t region = 1;   // cfg: secure-info region (1 = USA)
    bool logFatalErrors = true;
};

// Registers apt, cfg, ptm, ndm and err:f (and further services as they are added).
void registerDefaultServices(Kernel &kernel, const ServiceOptions &opt = {});

} // namespace pokerecomp
