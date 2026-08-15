#pragma once

#include <cstddef>
#include <cstdint>

#include "minikv/file.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"

namespace minikv {

struct WalRecoveryResult {
    std::size_t records_recovered = 0;
    std::uint64_t valid_bytes = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t min_sequence = 0;
    std::uint64_t max_sequence = 0;
    bool tail_truncated = false;
};

// Rebuilds a MemTable from a WAL. Only an incomplete final record is
// truncated. Any malformed complete record is Corruption. Outputs are updated
// only after the whole recovery operation succeeds.
Status RecoverWal(
    RecoveryFile& file,
    const Options& options,
    MemTable* memtable,
    WalRecoveryResult* result
);

}  // namespace minikv
