// gsp::Gpu -- GPU service HLE (issue #11 groundwork): shared-memory interrupt queue and command queue, VBlank
// interrupts, immediate completion of GPU commands. Rendering itself is not performed here.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "pokerecomp/gpu.h"
#include "pokerecomp/services.h"

namespace pokerecomp {
namespace {

constexpr uint32_t kSharedSize = 0x1000; // the game maps the next block 0x2000 above
constexpr uint32_t kQueueSlots = 0x34, kCmdQueueOffset = 0x800, kCmdSlots = 15, kCmdSize = 0x20;
constexpr uint64_t kFrameNs = 16'666'667; // 60 Hz VBlank

enum Irq : uint8_t { PSC0 = 0, PSC1 = 1, PDC0 = 2, PDC1 = 3, PPF = 4, P3D = 5, DMA = 6 };

class Gsp : public Service {
  public:
    explicit Gsp(Kernel &k, std::shared_ptr<Gpu> gpu) : k_(k), gpu_(std::move(gpu)) {}
    const char *name() const override { return "gsp::Gpu"; }

    void handle(IpcRequest &r) override {
        switch (r.commandId()) {
        case 0x0013: { // RegisterInterruptRelayQueue(flags, event handle) -> thread index, shared memory
            event_ = r.cmd[3];
            std::fprintf(stderr, "[gsp::Gpu] RegisterInterruptRelayQueue at tick %llu\n",
                         (unsigned long long)k_.ticks());
            if (!shared_)
                shared_ = k_.createMemoryBlock(kSharedSize);
            if (!vblankScheduled_) {
                vblankScheduled_ = true;
                k_.scheduleCallback(kFrameNs, kFrameNs, [this] {
                    consumeFramebufferInfo(0x200, kTopScreen);
                    consumeFramebufferInfo(0x240, kBottomScreen);
                    post(PDC0);
                    post(PDC1);
                });
            }
            r.reply(2, 2, kresult::Success);
            r.cmd[2] = 0; // GSP thread index
            r.cmd[3] = 0; // copy-handle descriptor
            r.cmd[4] = shared_;
            break;
        }
        case 0x0014: // UnregisterInterruptRelayQueue
        case 0x0017: // ReleaseRight
        case 0x0016: // AcquireRight
        case 0x0015: // TryAcquireRight
        case 0x0018: // ImportDisplayCaptureInfo (zeroed info)
        case 0x0019: // SaveVramSysArea
        case 0x001A: // RestoreVramSysArea
        case 0x001B: // ResetGpuCore
        case 0x001C: // SetLedForceOff
        case 0x001E: // SetInternalPriorities
        case 0x0001: // WriteHWRegs
        case 0x0002: // WriteHWRegsWithMask
        case 0x0003: // WriteHWRegRepeat
        case 0x0005: // SetBufferSwap
        case 0x0008: // FlushDataCache
        case 0x0009: // InvalidateDataCache
        case 0x000B: // SetLcdForceBlack
        case 0x0010: // SetAxiConfigQoSMode
        case 0x0011: // SetPerfLogMode
        case 0x001F: // StoreDataCache
            if (r.commandId() == 0x0018) {
                r.reply(1 + 0x10, 0, kresult::Success);
                std::memset(&r.cmd[2], 0, 0x40);
            } else {
                r.reply(1, 0, kresult::Success);
            }
            break;
        case 0x0004: { // ReadHWRegs(offset, size, out buffer) -> zeros
            const uint32_t size = r.cmd[2], desc = r.cmd[3], ptr = r.cmd[4];
            if (ptr)
                std::memset(k_.guestPtr(ptr), 0, size);
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = desc;
            r.cmd[3] = ptr;
            break;
        }
        case 0x000C: // TriggerCmdReqQueue
            processCommands();
            r.reply(1, 0, kresult::Success);
            break;
        default:
            if (seen_.insert(r.header()).second) {
                std::fprintf(stderr, "[gsp::Gpu] unimplemented command %08x:", r.header());
                for (unsigned i = 1; i <= r.normalParams() + r.translateParams() && i < 12; i++)
                    std::fprintf(stderr, " %08x", r.cmd[i]);
                std::fprintf(stderr, "\n");
            }
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    uint8_t *mem() {
        const uint32_t a = k_.memoryBlockAddress(shared_);
        return a ? k_.guestPtr(a) : nullptr;
    }

    // The game publishes a new framebuffer description by setting the header's update flag; the GPU takes it
    // over at the next VBlank and clears the flag. Layout (3dbrew, GSP shared memory): byte 0 selects one of
    // two 0x1C-byte entries that start at +4 {active, left vaddr, right vaddr, stride, format, status,
    // attribute}.
    void consumeFramebufferInfo(uint32_t offset, int screen) {
        uint8_t *q = mem();
        if (!q || !(q[offset + 1] & 1))
            return;
        const uint8_t *e = q + offset + 4 + (q[offset] & 1) * 0x1C;
        uint32_t w[7];
        std::memcpy(w, e, sizeof w);
        Gpu::Screen s;
        s.addr = w[1];
        s.stride = w[3];
        s.fmt = PixFmt(std::min<uint32_t>(w[4] & 7, 4));
        s.valid = (w[4] & 7) <= 4 && s.addr != 0;
        gpu_->setScreen(screen, s);
        q[offset + 1] = 0;
    }

    // Appends an interrupt to the relay queue of GSP thread 0 and wakes the game's interrupt thread.
    void post(Irq id) {
        uint8_t *q = mem();
        if (!q)
            return;
        uint8_t &index = q[0], &count = q[1], &err = q[2];
        if (count >= kQueueSlots) {
            err = 1; // missed interrupt
            return;
        }
        q[0xC + (index + count) % kQueueSlots] = id;
        count++;
        if (event_)
            k_.signalEvent(event_);
    }

    // Runs the transfer-engine side of a GX command (word layout: 3dbrew, GSP shared memory).
    void executeGx(uint32_t id, const uint32_t *w) {
        if (gpu_->onGxCommand)
            gpu_->onGxCommand(id, w);
        switch (id) {
        case 0: // RequestDma(src, dst, size)
            gpu_->dma(w[1], w[2], w[3]);
            break;
        case 2: // SetMemoryFill: two units; control words are packed in w7
            gpu_->memoryFill(w[1], w[3], w[2], w[7] & 0xFFFF);
            gpu_->memoryFill(w[4], w[6], w[5], w[7] >> 16);
            break;
        case 3: // SetDisplayTransfer(src, dst, inDim, outDim, flags)
            if (!gpu_->displayTransfer(w[1], w[2], w[3], w[4], w[5]))
                std::fprintf(stderr, "[gsp::Gpu] display transfer rejected: %08x %08x %08x %08x %08x\n", w[1],
                             w[2], w[3], w[4], w[5]);
            break;
        case 4: // SetTextureCopy(src, dst, size, in line|gap<<16, out line|gap<<16); units of 16 bytes
            gpu_->textureCopy(w[1], w[2], w[3], (w[4] & 0xFFFF) * 16, (w[4] >> 16) * 16, (w[5] & 0xFFFF) * 16,
                              (w[5] >> 16) * 16);
            break;
        case 1: // ProcessCommandList(buffer, size)
            gpu_->pica().processCommandList(w[1], w[2]);
            break;
        default: // 5: FlushCacheRegions does not touch the transfer engine
            break;
        }
    }

    void processCommands() {
        uint8_t *base = mem();
        if (!base)
            return;
        uint8_t *q = base + kCmdQueueOffset;
        uint8_t &index = q[0], &total = q[1];
        while (total > 0) {
            const uint8_t *c = q + 0x20 + index * kCmdSize;
            const uint32_t id = c[0];
            if (loggedGx_ < 40) {
                loggedGx_++;
                uint32_t w[8];
                std::memcpy(w, c, 32);
                std::fprintf(stderr, "[gsp::Gpu] GX cmd %u: %08x %08x %08x %08x %08x %08x %08x\n", id, w[1],
                             w[2], w[3], w[4], w[5], w[6], w[7]);
            }
            uint32_t w[8];
            std::memcpy(w, c, 32);
            GuestFault fault;
            const bool ran = k_.memory().runGuarded([&] { executeGx(id, w); }, &fault);
            if (!ran)
                std::fprintf(stderr, "[gsp::Gpu] GX command %u faulted at guest %08x\n", id, fault.address);
            switch (id) {
            case 0:
                post(DMA);
                break;
            case 1:
            case 5:
                post(P3D);
                break;
            case 2:
                post(PSC0);
                post(PSC1);
                break;
            case 3:
            case 4:
                post(PPF);
                break;
            default:
                std::fprintf(stderr, "[gsp::Gpu] unknown GX command %u\n", id);
                break;
            }
            index = uint8_t((index + 1) % kCmdSlots);
            total--;
        }
        q[2] = 0; // no more pending work
    }

    Kernel &k_;
    std::shared_ptr<Gpu> gpu_;
    Handle event_ = 0, shared_ = 0;
    bool vblankScheduled_ = false;
    unsigned loggedGx_ = 0;
    std::set<uint32_t> seen_;
};

} // namespace

std::shared_ptr<Gpu> registerGspService(Kernel &kernel) {
    // VRAM is mapped into the process at its virtual address; GX commands refer to it by that address.
    kernel.memory().map(kVramBase, kVramSize, GuestMemory::Read | GuestMemory::Write);
    auto gpu = std::make_shared<Gpu>(kernel.memory().base());
    kernel.registerService(std::make_unique<Gsp>(kernel, gpu));
    return gpu;
}

} // namespace pokerecomp
