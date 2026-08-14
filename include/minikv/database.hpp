#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/flush.hpp"
#include "minikv/manifest.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
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
    [[nodiscard]] LookupResult Get(std::string_view key) const;

    // V3 flushes in the foreground. On failure, the immutable generation and
    // its WAL remain available and Flush can be retried.
    Status Flush();

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
    [[nodiscard]] std::uint64_t version_id() const noexcept {
        return version_.id();
    }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
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
    Status OpenMutableWal();
    [[nodiscard]] std::string WalPath(std::uint64_t generation) const;
    [[nodiscard]] static LookupResult UserVisible(LookupResult result);

    std::string directory_;
    Options options_;
    std::shared_ptr<FlushEnvironment> environment_;
    std::unique_ptr<DatabaseLock> lock_;
    Version version_;
    MemTable mutable_;
    std::uint64_t mutable_generation_ = 0;
    std::unique_ptr<WalWriter> wal_;
    std::unique_ptr<ImmutableGeneration> immutable_;
    std::vector<std::unique_ptr<SSTableReader>> tables_;
    std::uint64_t last_sequence_ = 0;
    Status status_;
};

}  // namespace minikv
