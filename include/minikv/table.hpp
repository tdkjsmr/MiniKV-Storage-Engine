#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"

namespace minikv {

inline constexpr std::uint8_t kTableFormatVersion = 1;
inline constexpr std::size_t kTableHeaderSize = 32;

struct TableData {
    TableData(std::uint64_t generation, Options options);

    std::uint64_t generation = 0;
    std::uint64_t max_sequence = 0;
    MemTable records;
};

// V3 deliberately uses a sequential table: a checksummed header followed by
// sorted WAL-format records. V4 replaces the linear lookup with data blocks,
// a sparse index, and a footer without changing generation semantics.
Status EncodeTable(
    std::uint64_t generation,
    const MemTable& memtable,
    const Options& options,
    std::string* destination
);

Status DecodeTable(
    std::string_view input,
    const Options& options,
    std::unique_ptr<TableData>* output
);

}  // namespace minikv
