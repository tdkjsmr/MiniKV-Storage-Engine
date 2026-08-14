#pragma once

#include <cstddef>

namespace minikv {

struct Options {
    static constexpr std::size_t kDefaultMaxKeySize = 64U * 1024U;
    static constexpr std::size_t kDefaultMaxValueSize = 4U * 1024U * 1024U;
    static constexpr std::size_t kDefaultMemTableSizeLimit = 4U * 1024U * 1024U;
    static constexpr std::size_t kDefaultSSTableBlockSize = 4U * 1024U;

    std::size_t max_key_size = kDefaultMaxKeySize;
    std::size_t max_value_size = kDefaultMaxValueSize;
    std::size_t memtable_size_limit = kDefaultMemTableSizeLimit;
    std::size_t sstable_block_size = kDefaultSSTableBlockSize;
};

}  // namespace minikv
