#pragma once
// HLE system services (issue #10). Each service implements the commands the game uses; anything else falls
// through to the logged "unimplemented" path in the kernel.
#include <memory>
#include <string>

#include "pokerecomp/kernel.h"

namespace pokerecomp {

class Gpu;

// Host-updated controller state published through hid:USER shared memory.
struct InputState {
    uint32_t keys = 0; // 3DS HID key bits (A=1, B=2, Select=4, Start=8, ...)
    int16_t circleX = 0, circleY = 0;
    // Bottom-screen touch in pixels (0..319, 0..239); `touching` false means no contact.
    bool touching = false;
    uint16_t touchX = 0, touchY = 0;
};

struct ServiceOptions {
    uint32_t language = 1; // cfg: system language (1 = English)
    uint32_t region = 1;   // cfg: secure-info region (1 = USA)
    bool logFatalErrors = true;
};

// Registers apt, cfg, ptm, ndm and err:f (and further services as they are added).
void registerDefaultServices(Kernel &kernel, const ServiceOptions &opt = {});

// ldr:ro (CRO/CRS loader).
void registerLdrService(Kernel &kernel);

// gsp::Gpu (shared-memory queues, VBlank, GX commands). Maps VRAM and returns the GPU the service drives, so
// the host can capture the screens.
std::shared_ptr<Gpu> registerGspService(Kernel &kernel);

// hid:USER (pad ring buffer in shared memory; the host mutates `input` between kernel runs or from a
// callback).
void registerHidService(Kernel &kernel, std::shared_ptr<InputState> input);

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

class SaveStore;
// `saves` (optional) provides writable archives: save data, extdata and the SD card. Without it only RomFS
// exists.
void registerFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend,
                       std::shared_ptr<SaveStore> saves = nullptr);
// The same service without registering it (tests call it directly).
std::unique_ptr<Service> makeFsService(Kernel &kernel, std::shared_ptr<FsBackend> backend,
                                       std::shared_ptr<SaveStore> saves);

} // namespace pokerecomp
