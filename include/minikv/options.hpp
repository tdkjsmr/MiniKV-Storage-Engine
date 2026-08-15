#pragma once

#include <cstddef>

namespace minikv {

struct Options {
    static constexpr std::size_t kDefaultMaxKeySize = 64U * 1024U;
    static constexpr std::size_t kDefaultMaxValueSize = 4U * 1024U * 1024U;
    static constexpr std::size_t kDefaultMemTableSizeLimit = 4U * 1024U * 1024U;
    static constexpr std::size_t kDefaultSSTableBlockSize = 4U * 1024U;
    static constexpr double kDefaultBloomFalsePositiveRate = 0.01;
    static constexpr std::size_t kDefaultCompactionOutputSizeLimit =
        2U * 1024U * 1024U;

    std::size_t max_key_size = kDefaultMaxKeySize;
    std::size_t max_value_size = kDefaultMaxValueSize;
    std::size_t memtable_size_limit = kDefaultMemTableSizeLimit;
    std::size_t sstable_block_size = kDefaultSSTableBlockSize;
    bool bloom_filter_enabled = true;
    double bloom_false_positive_rate = kDefaultBloomFalsePositiveRate;
    std::size_t compaction_output_size_limit =
        kDefaultCompactionOutputSizeLimit;
};

}  // namespace minikv
