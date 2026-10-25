// hid:USER -- input service HLE (issue #14 groundwork): shared memory with the pad ring buffer, input events.
#include <cstdio>
#include <cstring>
#include <set>

#include "pokerecomp/services.h"

namespace pokerecomp {
namespace {

constexpr uint32_t kSharedSize = 0x1000;
constexpr uint64_t kPadPeriodNs = 4'000'000; // the system refreshes the pad ring about every 4 ms
constexpr uint32_t kPadEntries = 8, kPadEntries0 = 0x28, kTouchBase = 0xA8;

class Hid : public Service {
  public:
    explicit Hid(Kernel &k, std::shared_ptr<InputState> in) : k_(k), in_(std::move(in)) {}
    const char *name() const override { return "hid:USER"; }

    void handle(IpcRequest &r) override {
        switch (r.commandId()) {
        case 0x000A: { // GetIPCHandles -> shared memory + pad/touch/accel/gyro/debug-pad events
            if (!shared_) {
                shared_ = k_.createMemoryBlock(kSharedSize);
                for (auto &e : events_)
                    e = k_.createEvent(false);
                k_.scheduleCallback(kPadPeriodNs, kPadPeriodNs, [this] { updatePad(); });
            }
            r.reply(1, 7, kresult::Success);
            r.cmd[2] = 0x14000000; // copy 6 handles
            r.cmd[3] = shared_;
            for (unsigned i = 0; i < 5; i++)
                r.cmd[4 + i] = events_[i];
            break;
        }
        case 0x0011: // EnableAccelerometer
        case 0x0012: // DisableAccelerometer
        case 0x0013: // EnableGyroscope
        case 0x0014: // DisableGyroscope
            r.reply(1, 0, kresult::Success);
            break;
        case 0x0016: // GetGyroscopeLoCalibrateParam (zeros)
            r.reply(6, 0, kresult::Success);
            std::memset(&r.cmd[2], 0, 5 * 4);
            break;
        case 0x0017: // GetSoundVolume
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0x3F;
            break;
        default:
            if (seen_.insert(r.header()).second) {
                std::fprintf(stderr, "[hid:USER] unimplemented command %08x:", r.header());
                for (unsigned i = 1; i <= r.normalParams() + r.translateParams() && i < 12; i++)
                    std::fprintf(stderr, " %08x", r.cmd[i]);
                std::fprintf(stderr, "\n");
            }
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    void updatePad() {
        const uint32_t a = k_.memoryBlockAddress(shared_);
        if (!a)
            return;
        uint8_t *m = k_.guestPtr(a);
        auto rd = [&](uint32_t o) {
            uint32_t v;
            std::memcpy(&v, m + o, 4);
            return v;
        };
        auto wr = [&](uint32_t o, uint32_t v) { std::memcpy(m + o, &v, 4); };
        const uint32_t prev = rd(0x18); // current_state
        const uint32_t keys = in_->keys;
        const uint32_t index = (rd(0x10) + 1) % kPadEntries;
        const uint64_t now = k_.ticks();
        const uint64_t before = uint64_t(rd(0)) | uint64_t(rd(4)) << 32;
        wr(8, uint32_t(before));
        wr(12, uint32_t(before >> 32));
        wr(0, uint32_t(now));
        wr(4, uint32_t(now >> 32));
        wr(0x10, index);
        wr(0x18, keys);
        wr(0x1C, uint32_t(uint16_t(in_->circleX)) | uint32_t(uint16_t(in_->circleY)) << 16);
        const uint32_t e =
            kPadEntries0 + index * 0x10; // the game reads entry (index % 8) at +0x28 (0x28242c)
        wr(e, keys);
        wr(e + 4, keys & ~prev);
        wr(e + 8, prev & ~keys);
        wr(e + 12, uint32_t(uint16_t(in_->circleX)) | uint32_t(uint16_t(in_->circleY)) << 16);
        // Touch block at +0xA8: same header shape as the pad (timestamps, index at +0x10), entries of
        // {u16 x, u16 y, u8 valid} at +0x20 (read by the game's touch reader at 0x2825d0).
        const uint32_t t0 = kTouchBase, ti = (rd(t0 + 0x10) + 1) % kPadEntries;
        const uint64_t tbefore = uint64_t(rd(t0)) | uint64_t(rd(t0 + 4)) << 32;
        wr(t0 + 8, uint32_t(tbefore));
        wr(t0 + 12, uint32_t(tbefore >> 32));
        wr(t0, uint32_t(now));
        wr(t0 + 4, uint32_t(now >> 32));
        wr(t0 + 0x10, ti);
        const uint32_t te = t0 + 0x20 + ti * 8;
        wr(te, uint32_t(in_->touchX) | uint32_t(in_->touchY) << 16);
        wr(te + 4, in_->touching ? 1u : 0u);
        k_.signalEvent(events_[0]);
    }

    Kernel &k_;
    std::shared_ptr<InputState> in_;
    Handle shared_ = 0;
    Handle events_[6] = {};
    std::set<uint32_t> seen_;
};

} // namespace

void registerHidService(Kernel &kernel, std::shared_ptr<InputState> input) {
    kernel.registerService(std::make_unique<Hid>(kernel, std::move(input)));
}

} // namespace pokerecomp
