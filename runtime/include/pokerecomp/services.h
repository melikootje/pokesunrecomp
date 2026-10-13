#pragma once
// HLE system services (issue #10). Each service implements the commands the game uses; anything else falls
// through to the logged "unimplemented" path in the kernel.
#include <memory>
#include <string>

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

namespace pokerecomp {

// Backend for fs:USER: read-only access to the title's RomFS plus (later) save data. Paths use '/' separators
// and start with '/'. The runtime never reads the user's dump itself; the host supplies this.
class FsBackend {
  public:
    virtual ~FsBackend() = default;
    virtual bool romfsFileSize(const std::string &path, uint64_t *size) = 0;
    // Reads up to `size` bytes at `offset`; returns the number of bytes read (0 at end of file) or -1 on
    // error.
    virtual int64_t romfsRead(const std::string &path, uint64_t offset, uint32_t size, void *dst) = 0;
    virtual bool romfsDirExists(const std::string &path) {
        (void)path;
        return false;
    }
    // The raw RomFS image (IVFC header first), as the SDK's MountRom reads it.
    virtual bool rawRomfsSize(uint64_t *size) {
        (void)size;
        return false;
    }
    virtual int64_t rawRomfsRead(uint64_t offset, uint32_t size, void *dst) {
        (void)offset;
        (void)size;
        (void)dst;
        return -1;
    }
};

void registerFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend);

} // namespace pokerecomp
