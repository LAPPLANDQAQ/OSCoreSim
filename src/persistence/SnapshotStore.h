#pragma once

#include "auth/UserAccount.h"
#include "memory/MemoryBlock.h"
#include "process/PCB.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oscore {

struct KernelSnapshot {
    std::vector<UserAccount> users;
    std::uint32_t nextPid = 1;
    std::vector<PCB> pcbs;
    std::array<std::vector<std::uint32_t>, 3> readyQueues;
    std::vector<MemoryBlock> memoryBlocks;
    std::uint32_t totalMemoryKB = 1024;
    AllocAlgorithm allocAlgorithm = AllocAlgorithm::FIRST_FIT;
    bool schedulerRunning = false;
    std::string schedulerOwner;
};

struct SnapshotSummary {
    std::size_t users = 0;
    std::size_t processes = 0;
    std::size_t memoryBlocks = 0;
    std::array<std::size_t, 3> readyQueueSizes{};
};

class SnapshotStore {
public:
    explicit SnapshotStore(std::string path = "data/os_state.bin");

    [[nodiscard]] std::string defaultPath() const;
    [[nodiscard]] bool exists() const;
    [[nodiscard]] SnapshotSummary summarize(const KernelSnapshot& snapshot) const;

    bool save(const KernelSnapshot& snapshot, std::string& message) const;
    bool load(KernelSnapshot& snapshot, std::string& message) const;

private:
    std::string path_;
};

} // namespace oscore
