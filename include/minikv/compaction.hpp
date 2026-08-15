#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"
#include "minikv/table.hpp"

namespace minikv {

class FlushEnvironment;

struct CompactionInput {
    std::uint32_t level = 0;
    const SSTableReader* table = nullptr;
};

struct CompactionBuildStats {
    std::uint64_t input_records = 0;
    std::uint64_t output_records = 0;
    std::uint64_t duplicate_records_dropped = 0;
    std::uint64_t tombstones_dropped = 0;
};

// Reads each immutable input in key order and performs a minimum-heap merge.
// The greatest Sequence wins for equal user keys. Tombstones may be dropped
// only when the caller has proved the target level is bottommost for the full
// selected key range.
[[nodiscard]] Status BuildCompactionOutputs(
    const std::vector<CompactionInput>& inputs,
    const Options& options,
    bool may_drop_tombstones,
    std::vector<MemTable>* outputs,
    CompactionBuildStats* stats
);

// Crash-safe batch publication for already-built outputs:
// encode all -> write/sync all temporary files -> rename all -> sync directory
// -> reopen and validate all final files. MANIFEST publication is deliberately
// handled by Database after this function succeeds.
[[nodiscard]] Status PublishCompactionTables(
    const std::string& directory,
    std::uint64_t first_file_number,
    const std::vector<MemTable>& outputs,
    const Options& options,
    FlushEnvironment& environment,
    std::vector<std::unique_ptr<SSTableReader>>* published
);

}  // namespace minikv
