#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace pokerecomp {

// A fault raised by guest code touching memory that is not mapped (or lacks permission).
struct GuestFault {
    uint32_t address = 0; // guest address (low 32 bits of the faulting host address - base)
    bool write = false;   // best effort; false when the platform does not report it
};

// The 32-bit guest address space as one 4 GiB host reservation: host = base() + guest address.
// Pages are committed with map(); everything else faults. Not copyable; one instance per process
// may use runGuarded() at a time (the fault handler is process-wide).
class GuestMemory {
  public:
    enum Prot : int { None = 0, Read = 1, Write = 2, Exec = 4 };

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;

    bool ok() const { return base_ != nullptr; }
    uint8_t *base() const { return base_; }

    // addr/size must be page aligned (16 KiB granularity is accepted on all hosts; callers use 4 KiB
    // guest pages and this rounds to the host page size internally when it is larger).
    bool map(uint32_t addr, uint32_t size, int prot);
    bool protect(uint32_t addr, uint32_t size, int prot);
    bool unmap(uint32_t addr, uint32_t size);

    // Runs fn(); if it faults on a guest address inside the reservation, returns false and fills `fault`.
    // A fault anywhere else is re-raised with the default action (crash).
    bool runGuarded(const std::function<void()> &fn, GuestFault *fault);

    static size_t hostPageSize();

  private:
    uint8_t *base_ = nullptr;
};

} // namespace pokerecomp
