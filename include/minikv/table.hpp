#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/bloom_filter.hpp"
#include "minikv/file.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"

namespace minikv {

inline constexpr std::uint8_t kTableFormatVersion = 3;
inline constexpr std::size_t kTableHeaderSize = 32;
inline constexpr std::size_t kTableBlockHeaderSize = 20;
inline constexpr std::size_t kTableIndexHeaderSize = 20;
inline constexpr std::size_t kTableFooterSize = 104;
inline constexpr std::size_t kMaximumTableIndexSize = 64U * 1024U * 1024U;

struct SSTableMetadata {
    std::uint64_t generation = 0;
    std::uint64_t record_count = 0;
    std::uint64_t minimum_sequence = 0;
    std::uint64_t maximum_sequence = 0;
    std::uint64_t file_size = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t bloom_size = 0;
    std::uint64_t bloom_bit_count = 0;
    std::uint8_t bloom_hash_count = 0;
    std::uint32_t block_count = 0;
    std::string minimum_key;
    std::string maximum_key;
};

struct SSTableReadStats {
    std::uint64_t tables_considered = 0;
    std::uint64_t range_rejections = 0;
    std::uint64_t bloom_filter_checks = 0;
    std::uint64_t bloom_filter_rejections = 0;
    std::uint64_t bloom_false_positives = 0;
    std::uint64_t data_blocks_read = 0;
    std::uint64_t bytes_read = 0;
};

class SSTableReader;

class SSTableIterator {
public:
    SSTableIterator(const SSTableIterator&) = delete;
    SSTableIterator& operator=(const SSTableIterator&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const MemTableRecord* record() const noexcept;
    [[nodiscard]] const SSTableReadStats& statistics() const noexcept {
        return statistics_;
    }
    Status Next();

private:
    friend class SSTableReader;

    SSTableIterator(
        const SSTableReader* reader,
        std::string begin,
        std::optional<std::string> end
    );

    Status Initialize();
    Status LoadBlock(std::size_t block);
    void EnforceUpperBound();

    const SSTableReader* reader_ = nullptr;
    std::string begin_;
    std::optional<std::string> end_;
    std::vector<MemTableRecord> records_;
    std::size_t block_ = 0;
    std::size_t record_ = 0;
    bool valid_ = false;
    SSTableReadStats statistics_;
};

// Keeps only verified metadata and the sparse index in memory. Data blocks are
// read from the immutable file for each point lookup.
class SSTableReader {
public:
    static Status Open(
        std::string path,
        Options options,
        std::unique_ptr<SSTableReader>* output
    );

    SSTableReader(const SSTableReader&) = delete;
    SSTableReader& operator=(const SSTableReader&) = delete;

    [[nodiscard]] LookupResult Get(
        std::string_view key,
        SSTableReadStats* stats = nullptr
    ) const;

    // Reads records in key order. This is intended for diagnostics and tests,
    // not for the Database point-read path.
    Status ReadRecords(
        std::size_t limit,
        std::vector<MemTableRecord>* records
    ) const;

    // Creates a forward iterator positioned at the first key in [begin, end).
    // Only one data block is retained by the iterator at a time.
    Status NewIterator(
        std::string_view begin,
        std::optional<std::string_view> end,
        std::unique_ptr<SSTableIterator>* output
    ) const;

    [[nodiscard]] const SSTableMetadata& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    friend class SSTableIterator;

    struct IndexEntry {
        std::string first_key;
        std::uint64_t block_offset = 0;
        std::uint32_t block_size = 0;
    };

    SSTableReader(
        std::string path,
        Options options,
        std::unique_ptr<RecoveryFile> file
    );

    Status ReadBlock(
        std::size_t index,
        std::vector<MemTableRecord>* records,
        SSTableReadStats* stats
    ) const;

    std::string path_;
    Options options_;
    std::unique_ptr<RecoveryFile> file_;
    SSTableMetadata metadata_;
    std::vector<IndexEntry> index_;
    std::unique_ptr<BloomFilter> bloom_filter_;
};

// Builds one immutable format-version-3 SSTable in memory. Publication and
// directory durability are handled by PublishTable.
Status EncodeTable(
    std::uint64_t generation,
    const MemTable& memtable,
    const Options& options,
    std::string* destination
);

}  // namespace minikv
