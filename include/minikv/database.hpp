#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/compaction.hpp"
#include "minikv/flush.hpp"
#include "minikv/manifest.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/scan.hpp"
#include "minikv/status.hpp"
#include "minikv/table.hpp"
#include "minikv/wal.hpp"

namespace minikv {

class DatabaseLock;

struct DatabaseOpenResult {
    std::size_t tables_loaded = 0;
    std::size_t wals_recovered = 0;
    std::size_t obsolete_wals_removed = 0;
    std::size_t orphan_files_removed = 0;
    std::uint64_t max_sequence = 0;
    std::uint64_t active_generation = 0;
    std::uint64_t version_id = 0;
    std::uint8_t storage_format_version = 0;
};

struct DatabaseReadStats {
    std::uint64_t point_lookups = 0;
    std::uint64_t mutable_hits = 0;
    std::uint64_t immutable_hits = 0;
    std::uint64_t tables_considered = 0;
    std::uint64_t range_rejections = 0;
    std::uint64_t bloom_filter_checks = 0;
    std::uint64_t bloom_filter_rejections = 0;
    std::uint64_t bloom_false_positives = 0;
    std::uint64_t data_blocks_read = 0;
    std::uint64_t bytes_read = 0;
};

struct CompactionStats {
    std::uint64_t input_files = 0;
    std::uint64_t output_files = 0;
    std::uint64_t input_records = 0;
    std::uint64_t output_records = 0;
    std::uint64_t duplicate_records_dropped = 0;
    std::uint64_t tombstones_dropped = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t bytes_reclaimed = 0;
    std::uint64_t elapsed_microseconds = 0;
};

class Database {
public:
    ~Database();

    static Status Open(
        std::string directory,
        Options options,
        std::unique_ptr<Database>* output,
        DatabaseOpenResult* open_result
    );

    // Exposed for deterministic failure tests. Production callers should use
    // Open, which installs PosixFlushEnvironment.
    static Status OpenWithEnvironment(
        std::string directory,
        Options options,
        std::shared_ptr<FlushEnvironment> environment,
        std::unique_ptr<Database>* output,
        DatabaseOpenResult* open_result
    );

    Status Put(
        std::string_view key,
        std::string_view value,
        WriteOptions write_options = {}
    );
    Status Delete(std::string_view key, WriteOptions write_options = {});
    [[nodiscard]] LookupResult Get(
        std::string_view key,
        DatabaseReadStats* operation_stats = nullptr
    ) const;
    [[nodiscard]] ScanResult Scan(const ScanOptions& scan_options) const;
    [[nodiscard]] ScanResult ScanPrefix(
        std::string_view prefix,
        std::size_t limit = 100,
        std::string_view continuation_token = {}
    ) const;
    [[nodiscard]] ScanResult LoadAll(
        std::size_t limit = 100,
        std::string_view continuation_token = {}
    ) const;

    // V3 flushes in the foreground. On failure, the immutable generation and
    // its WAL remain available and Flush can be retried.
    Status Flush();

    // V7 runs a foreground L0-to-L1 compaction. All L0 inputs and the
    // overlapping L1 range are committed through one VersionEdit.
    Status Compact(CompactionStats* stats = nullptr);

    [[nodiscard]] std::uint64_t last_sequence() const noexcept {
        return last_sequence_;
    }
    [[nodiscard]] std::uint64_t mutable_generation() const noexcept {
        return mutable_generation_;
    }
    [[nodiscard]] std::size_t mutable_size() const noexcept {
        return mutable_.ApproximateDataSize();
    }
    [[nodiscard]] bool has_immutable() const noexcept {
        return immutable_ != nullptr;
    }
    [[nodiscard]] std::size_t flushed_table_count() const noexcept {
        return tables_.size();
    }
    [[nodiscard]] std::size_t level_table_count(
        std::uint32_t level
    ) const noexcept;
    [[nodiscard]] std::uint64_t version_id() const noexcept {
        return version_.id();
    }
    [[nodiscard]] DatabaseReadStats read_statistics() const noexcept {
        return read_statistics_;
    }
    [[nodiscard]] const CompactionStats& last_compaction_statistics()
        const noexcept {
        return last_compaction_statistics_;
    }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    struct CompactionState;

    struct ImmutableGeneration {
        ImmutableGeneration(
            std::uint64_t generation,
            std::string wal_path,
            MemTable memtable
        );

        std::uint64_t generation = 0;
        std::string wal_path;
        MemTable memtable;
        std::unique_ptr<SSTableReader> pending_table;
        bool version_published = false;
        bool wal_removed_and_synced = false;
    };

    Database(
        std::string directory,
        Options options,
        std::shared_ptr<FlushEnvironment> environment,
        MemTable mutable_memtable,
        std::uint64_t mutable_generation,
        std::unique_ptr<WalWriter> wal,
        std::vector<std::unique_ptr<SSTableReader>> tables,
        std::uint64_t last_sequence,
        Version version,
        std::unique_ptr<DatabaseLock> lock
    );

    Status Write(
        ValueType type,
        std::string_view key,
        std::string_view value,
        WriteOptions write_options
    );
    Status FreezeMutable();
    Status ContinueFlush();
    Status PrepareCompaction();
    Status ContinueCompaction(CompactionStats* stats);
    Status OpenMutableWal();
    [[nodiscard]] std::string WalPath(std::uint64_t generation) const;
    [[nodiscard]] static LookupResult UserVisible(LookupResult result);
    [[nodiscard]] LookupResult FinishRead(
        LookupResult result,
        const DatabaseReadStats& operation,
        DatabaseReadStats* operation_stats
    ) const;

    std::string directory_;
    Options options_;
    std::shared_ptr<FlushEnvironment> environment_;
    std::unique_ptr<DatabaseLock> lock_;
    Version version_;
    MemTable mutable_;
    std::uint64_t mutable_generation_ = 0;
    std::unique_ptr<WalWriter> wal_;
    std::unique_ptr<ImmutableGeneration> immutable_;
    std::unique_ptr<CompactionState> compaction_;
    std::vector<std::unique_ptr<SSTableReader>> tables_;
    std::uint64_t last_sequence_ = 0;
    mutable DatabaseReadStats read_statistics_;
    CompactionStats last_compaction_statistics_;
    Status status_;
};

}  // namespace minikv
