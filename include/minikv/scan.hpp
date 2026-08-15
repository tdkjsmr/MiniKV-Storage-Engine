#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "minikv/status.hpp"

namespace minikv {

struct ScanEntry {
    std::string key;
    std::string value;
};

struct ScanStatistics {
    std::uint64_t sources_considered = 0;
    std::uint64_t tables_considered = 0;
    std::uint64_t data_blocks_read = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t records_examined = 0;
    std::uint64_t obsolete_versions_skipped = 0;
    std::uint64_t tombstones_skipped = 0;
};

// Scan uses the bytewise half-open interval [begin, end). An absent end is
// unbounded. An empty begin starts before every valid (non-empty) key.
struct ScanOptions {
    std::string begin;
    std::optional<std::string> end;
    std::size_t limit = 100;
    std::string continuation_token;
};

struct ScanResult {
    Status status;
    std::vector<ScanEntry> entries;
    bool truncated = false;
    std::string continuation_token;
    ScanStatistics statistics;
};

}  // namespace minikv
