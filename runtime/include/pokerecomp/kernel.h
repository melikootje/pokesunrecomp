#pragma once
// HLE of the Horizon kernel interface (issue #9): threads, synchronisation objects, memory, handles and the
// SVC ABI, scheduled cooperatively and deterministically on one host thread ("green threads").
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"

namespace pokerecomp {

using Handle = uint32_t;

// Result codes (subset), values as seen by guest code.
namespace kresult {
constexpr uint32_t Success = 0;
constexpr uint32_t Timeout = 0x09401BFE;
constexpr uint32_t InvalidHandle = 0xD8E007F7;
constexpr uint32_t InvalidAddress = 0xE0E01BF5;
constexpr uint32_t OutOfMemory = 0xD86007F3;
constexpr uint32_t NotAuthorized = 0xD9001BEA;
constexpr uint32_t InvalidCommand = 0xD900182F;
constexpr uint32_t PortNotFound = 0xD88007FA;
constexpr uint32_t InvalidCombination = 0xE0E01BEE;
constexpr uint32_t MaxCount = 0xD8E007FD;
constexpr uint32_t NotLocked = 0xD9001BF4;
} // namespace kresult

constexpr uint32_t kCurrentThread = 0xFFFF8000, kCurrentProcess = 0xFFFF8001;
constexpr uint64_t kTicksPerSecond = 268111856;
constexpr uint32_t kTlsBase = 0x1FF82000, kTlsSize = 0x200, kCmdBufOffset = 0x80;
constexpr uint32_t kThreadExitAddress = 0x00FF0000; // lr given to new threads; reaching it exits the thread

class Kernel;

// Command buffer view handed to service implementations (TLS + 0x80 of the calling thread).
struct IpcRequest {
    Kernel *kernel = nullptr;
    uint32_t *cmd = nullptr; // 64 words
    uint32_t header() const { return cmd[0]; }
    uint32_t commandId() const { return cmd[0] >> 16; }
    unsigned normalParams() const { return (cmd[0] >> 6) & 0x3F; }
    unsigned translateParams() const { return cmd[0] & 0x3F; }
    // Writes the reply header (command id kept) with the given parameter counts and result in word 1.
    void reply(unsigned normal, unsigned translate, uint32_t result) {
        cmd[0] = (cmd[0] & 0xFFFF0000u) | (normal << 6) | translate;
        cmd[1] = result;
    }
};

class Service {
  public:
    virtual ~Service() = default;
    virtual const char *name() const = 0;
    virtual void handle(IpcRequest &req) = 0;
};

// System state the kernel publishes to the process through the configuration memory page (0x1FF80000).
struct KernelConfig {
    uint32_t appMemType =
        1; // ExHeader system mode: 0 = 64 MB, 1 = 72 MB, 2 = 80 MB application memory (Old 3DS)
    uint8_t kernelMajor = 2, kernelMinor = 46, kernelRevision = 0; // reported kernel version
    uint32_t ctrSdkVersion = 0;
};
constexpr uint32_t kConfigPage = 0x1FF80000;

enum class UnimplementedPolicy { ReturnError, ReturnSuccess };

enum class RunResult { AllExited, Deadlock, InstructionLimit, ProcessExit, Break, Trap, Fault };

struct KernelStats {
    uint64_t svcs = 0, switches = 0, ipcRequests = 0;
};

class Kernel {
  public:
    Kernel(GuestMemory &gm, Dispatcher &disp);
    ~Kernel();

    // Must be called before createMainThread(): maps and fills the configuration page.
    void configure(const KernelConfig &cfg);
    static uint32_t appMemorySize(uint32_t systemMode);

    // Starts the primary thread (does not run it).
    Handle createMainThread(uint32_t entry, uint32_t stackTop, uint32_t arg, int priority = 0x30);

    // Runs until every thread has exited, all remaining threads are blocked forever, the process exits, or
    // `maxInsns` guest instructions have executed.
    RunResult run(uint64_t maxInsns);

    // Registers a service reachable through `srv:GetServiceHandle` (the kernel owns it).
    void registerService(std::unique_ptr<Service> svc);
    void setUnimplementedPolicy(UnimplementedPolicy p) { policy_ = p; }

    // Observability
    void setLog(std::function<void(const std::string &)> f) { log_ = std::move(f); }
    const std::vector<std::string> &trace() const { return trace_; } // deterministic scheduling/SVC trace
    uint64_t ticks() const { return now_; }
    // Number of requests per (service name, command header) over the whole run.
    const std::map<std::pair<std::string, uint32_t>, uint64_t> &ipcHistogram() const { return ipcHist_; }
    // The last requests to services other than the high-rate gsp/hid/dsp ones, with their results (diagnostics).
    const std::deque<std::string> &recentIpc() const { return recentIpc_; }
    // Adds a line to that list (services record what a request meant, e.g. a file path).
    void note(const std::string &line) {
        recentIpc_.push_back(line);
        if (recentIpc_.size() > 48)
            recentIpc_.pop_front();
    }
    const KernelStats &stats() const { return stats_; }
    uint32_t exitCode() const { return exitCode_; }
    uint32_t breakReason() const { return breakReason_; }
    GuestFault lastFault() const { return fault_; }
    // Returns the combined "unimplemented service/command" list seen so far (service, command header).
    std::vector<std::pair<std::string, uint32_t>> unimplementedCommands() const;

    // For services and tests
    GuestMemory &memory() { return gm_; }
    Dispatcher &dispatcher() { return disp_; }
    void log(const std::string &s) const {
        if (log_)
            log_(s);
    }
    Handle createEvent(bool sticky);
    // The service object behind a session handle (nullptr for anything else); lets tests drive sessions
    // directly.
    Service *sessionService(Handle h);
    // Shared memory block owned by a service; the process maps it with MapMemoryBlock. Returns 0 for the
    // address until mapped.
    Handle createMemoryBlock(uint32_t size);
    uint32_t memoryBlockAddress(Handle h) const;
    Handle createMutex(bool locked);
    // Creates a session handle backed by a kernel-owned service object (e.g. an open file).
    Handle createSession(std::unique_ptr<Service> svc);
    // Releases a service object created by createSession (called when the guest closes the file, etc.).
    Handle createSemaphore(int32_t initial, int32_t max);
    // Ends the run with RunResult::Break and records why (used for fatal errors reported by the guest).
    void stopWithReason(uint32_t reason, const std::string &message);
    const std::string &stopMessage() const { return stopMessage_; }
    // Host view of guest memory for services.
    uint8_t *guestPtr(uint32_t addr) { return gm_.base() + addr; }
    void signalEvent(Handle h);
    // Signals `h` after `delayNs` of virtual time (models system services that notify asynchronously).
    void scheduleSignal(Handle h, uint64_t delayNs, uint64_t periodNs = 0);
    // Runs `cb` on the scheduler after `delayNs` of virtual time (periodically when periodNs > 0).
    void scheduleCallback(uint64_t delayNs, uint64_t periodNs, std::function<void()> cb);
    uint32_t currentThreadId() const;
    // One line per live thread describing its state and what it waits for (deadlock diagnostics).
    std::vector<std::string> describeThreads() const;
    // Debug access to a thread's registers (nullptr for unknown ids).
    const RcCtx *threadContext(uint32_t id) const;

    struct Impl;

  private:
    GuestMemory &gm_;
    Dispatcher &disp_;
    std::unique_ptr<Impl> impl_;
    std::function<void(const std::string &)> log_;
    std::vector<std::string> trace_;
    UnimplementedPolicy policy_ = UnimplementedPolicy::ReturnError;
    uint64_t now_ = 0;
    KernelStats stats_;
    std::map<std::pair<std::string, uint32_t>, uint64_t> ipcHist_;
    std::deque<std::string> recentIpc_;
    uint32_t exitCode_ = 0, breakReason_ = 0;
    std::string stopMessage_;
    GuestFault fault_;
};

} // namespace pokerecomp
