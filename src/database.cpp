#include "minikv/database.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "minikv/file.hpp"
#include "minikv/manifest.hpp"
#include "minikv/recovery.hpp"
#include "database_lock.hpp"

namespace minikv {
namespace {

namespace fs = std::filesystem;

Status DatabaseCorruption(std::string message) {
    return Status::Corruption("database: " + std::move(message));
}

std::string JoinPath(const std::string& directory, const std::string& name) {
    return (fs::path(directory) / name).string();
}

bool ParseGenerationFile(
    std::string_view name,
    std::string_view extension,
    std::uint64_t* generation
) {
    if (generation == nullptr || name.size() != 24 || name[20] != '.' ||
        name.substr(21) != extension) {
        return false;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 20; ++index) {
        const unsigned char character = static_cast<unsigned char>(name[index]);
        if (!std::isdigit(character)) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0) {
        return false;
    }
    *generation = value;
    return true;
}

bool ManifestMetadataMatches(
    const SSTableMetadata& expected,
    const SSTableMetadata& actual
) {
    return expected.generation == actual.generation &&
           expected.file_size == actual.file_size &&
           expected.record_count == actual.record_count &&
           expected.minimum_sequence == actual.minimum_sequence &&
           expected.maximum_sequence == actual.maximum_sequence &&
           expected.minimum_key == actual.minimum_key &&
           expected.maximum_key == actual.maximum_key;
}

bool IsStorageTemporaryFile(std::string_view name) {
    return name == "MANIFEST.tmp" ||
           (name.size() > 8 && name.substr(name.size() - 8) == ".sst.tmp");
}

void AccumulateTableStats(
    const SSTableReadStats& source,
    DatabaseReadStats* destination
) {
    destination->tables_considered += source.tables_considered;
    destination->range_rejections += source.range_rejections;
    destination->bloom_filter_checks += source.bloom_filter_checks;
    destination->bloom_filter_rejections += source.bloom_filter_rejections;
    destination->bloom_false_positives += source.bloom_false_positives;
    destination->data_blocks_read += source.data_blocks_read;
    destination->bytes_read += source.bytes_read;
}

void AccumulateDatabaseStats(
    const DatabaseReadStats& source,
    DatabaseReadStats* destination
) {
    destination->point_lookups += source.point_lookups;
    destination->mutable_hits += source.mutable_hits;
    destination->immutable_hits += source.immutable_hits;
    destination->tables_considered += source.tables_considered;
    destination->range_rejections += source.range_rejections;
    destination->bloom_filter_checks += source.bloom_filter_checks;
    destination->bloom_filter_rejections += source.bloom_filter_rejections;
    destination->bloom_false_positives += source.bloom_false_positives;
    destination->data_blocks_read += source.data_blocks_read;
    destination->bytes_read += source.bytes_read;
}

}  // namespace

Database::ImmutableGeneration::ImmutableGeneration(
    std::uint64_t immutable_generation,
    std::string immutable_wal_path,
    MemTable immutable_memtable
)
    : generation(immutable_generation),
      wal_path(std::move(immutable_wal_path)),
      memtable(std::move(immutable_memtable)) {
    for (const auto& record : memtable.Records()) {
        maximum_sequence = std::max(maximum_sequence, record.sequence);
    }
}

struct Database::CompactionState {
    std::set<std::uint64_t> input_file_numbers;
    std::vector<std::string> cleanup_paths;
    std::vector<MemTable> output_memtables;
    std::vector<std::unique_ptr<SSTableReader>> published_outputs;
    CompactionBuildStats build_stats;
    Version candidate;
    CompactionStats stats;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    std::uint64_t first_output_file_number = 0;
    std::size_t cleanup_index = 0;
    bool candidate_ready = false;
    bool version_published = false;
    bool cleanup_synced = false;
    bool wal_generation_advanced = false;
};

Status Database::Open(
    std::string directory,
    Options options,
    std::unique_ptr<Database>* output,
    DatabaseOpenResult* open_result
) {
    return OpenWithEnvironment(
        std::move(directory),
        options,
        std::make_shared<PosixFlushEnvironment>(),
        output,
        open_result
    );
}

Status Database::OpenWithEnvironment(
    std::string directory,
    Options options,
    std::shared_ptr<FlushEnvironment> environment,
    std::unique_ptr<Database>* output,
    DatabaseOpenResult* open_result
) {
    if (output == nullptr || open_result == nullptr) {
        return Status::InvalidArgument("database Open outputs must not be null");
    }
    output->reset();
    *open_result = {};
    if (directory.empty() || environment == nullptr ||
        options.memtable_size_limit == 0 ||
        options.compaction_output_size_limit == 0 ||
        options.level0_compaction_trigger == 0 ||
        options.maximum_scan_entries == 0 ||
        options.maximum_scan_entries >=
            std::numeric_limits<std::uint32_t>::max() ||
        options.max_key_size == 0 ||
        options.max_key_size > std::numeric_limits<std::uint32_t>::max() ||
        options.max_value_size > std::numeric_limits<std::uint32_t>::max() ||
        options.max_value_size >
            std::numeric_limits<std::uint32_t>::max() - kWalHeaderSize ||
        options.max_key_size >
            std::numeric_limits<std::uint32_t>::max() - kWalHeaderSize -
                options.max_value_size ||
        options.sstable_block_size < kTableBlockHeaderSize + kWalHeaderSize ||
        options.sstable_block_size >
            std::numeric_limits<std::uint32_t>::max() - kTableBlockHeaderSize ||
        !std::isfinite(options.bloom_false_positive_rate) ||
        options.bloom_false_positive_rate <= 0.0 ||
        options.bloom_false_positive_rate >= 1.0) {
        return Status::InvalidArgument(
            "database requires a directory, environment, and valid size limits"
        );
    }

    std::error_code filesystem_error;
    const bool created = fs::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        return Status::IOError(
            "create database directory '" + directory + "': " +
            filesystem_error.message()
        );
    }
    if (!fs::is_directory(directory, filesystem_error) || filesystem_error) {
        return Status::IOError("database path is not a directory: '" + directory + "'");
    }
    if (created) {
        fs::path parent = fs::path(directory).parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        const auto sync_status = environment->SyncDirectory(parent.string());
        if (!sync_status.ok()) {
            return sync_status;
        }
    }

    std::unique_ptr<DatabaseLock> lock;
    auto status = DatabaseLock::Acquire(directory, &lock);
    if (!status.ok()) {
        return status;
    }

    std::map<std::uint64_t, std::string> table_paths;
    std::map<std::uint64_t, std::string> wal_paths;
    std::vector<std::string> temporary_paths;
    bool manifest_exists = false;
    fs::directory_iterator iterator(directory, filesystem_error);
    const fs::directory_iterator end;
    while (!filesystem_error && iterator != end) {
        if (iterator->is_regular_file(filesystem_error)) {
            const std::string name = iterator->path().filename().string();
            std::uint64_t generation = 0;
            if (name == kManifestFileName) {
                manifest_exists = true;
            } else if (ParseGenerationFile(name, "sst", &generation)) {
                table_paths.emplace(generation, iterator->path().string());
            } else if (ParseGenerationFile(name, "wal", &generation)) {
                wal_paths.emplace(generation, iterator->path().string());
            } else if (IsStorageTemporaryFile(name)) {
                temporary_paths.push_back(iterator->path().string());
            }
        }
        iterator.increment(filesystem_error);
    }
    if (filesystem_error) {
        return Status::IOError(
            "scan database directory '" + directory + "': " +
            filesystem_error.message()
        );
    }

    Version version;
    if (!manifest_exists) {
        if (!table_paths.empty() || !wal_paths.empty()) {
            return Status::VersionMismatch(
                "database contains legacy files but no supported MANIFEST"
            );
        }
        version = Version::NewDatabase();
        status = PublishManifest(directory, version, options, *environment);
        if (!status.ok()) {
            return status;
        }
    } else {
        status = LoadManifest(directory, options, &version);
        if (!status.ok()) {
            return status;
        }
    }

    std::map<std::uint64_t, const VersionTable*> live_tables;
    for (const auto& table : version.tables()) {
        live_tables.emplace(table.metadata.generation, &table);
    }
    std::vector<std::unique_ptr<SSTableReader>> tables;
    tables.reserve(version.tables().size());
    for (const auto& expected : version.tables()) {
        const auto path = table_paths.find(expected.metadata.generation);
        if (path == table_paths.end()) {
            return DatabaseCorruption(
                "MANIFEST references missing table " +
                GenerationFileName(expected.metadata.generation, "sst")
            );
        }
        std::unique_ptr<SSTableReader> table;
        status = SSTableReader::Open(path->second, options, &table);
        if (!status.ok()) {
            return status;
        }
        if (!ManifestMetadataMatches(expected.metadata, table->metadata())) {
            return DatabaseCorruption(
                "MANIFEST and SSTable metadata disagree for file " +
                std::to_string(expected.metadata.generation)
            );
        }
        tables.push_back(std::move(table));
    }

    DatabaseOpenResult result;
    result.tables_loaded = tables.size();
    bool directory_changed = false;
    for (const auto& path : temporary_paths) {
        status = environment->RemoveFile(path);
        if (!status.ok()) {
            return status;
        }
        ++result.orphan_files_removed;
        directory_changed = true;
    }
    for (const auto& [generation, path] : table_paths) {
        if (live_tables.find(generation) == live_tables.end()) {
            status = environment->RemoveFile(path);
            if (!status.ok()) {
                return status;
            }
            ++result.orphan_files_removed;
            directory_changed = true;
        }
    }

    std::vector<std::pair<std::uint64_t, std::string>> unmatched_wals;
    for (const auto& [generation, path] : wal_paths) {
        if (generation < version.next_file_number()) {
            status = environment->RemoveFile(path);
            if (!status.ok()) {
                return status;
            }
            ++result.obsolete_wals_removed;
            directory_changed = true;
        } else if (
            generation == version.next_file_number() ||
            (version.next_file_number() !=
                 std::numeric_limits<std::uint64_t>::max() &&
             generation == version.next_file_number() + 1U)
        ) {
            unmatched_wals.emplace_back(generation, path);
        } else {
            return DatabaseCorruption(
                "WAL file number is ahead of the MANIFEST frontier"
            );
        }
    }
    if (directory_changed) {
        status = environment->SyncDirectory(directory);
        if (!status.ok()) {
            return status;
        }
    }
    if (unmatched_wals.size() > 2) {
        return DatabaseCorruption("too many unflushed WAL generations exist");
    }
    if (unmatched_wals.size() == 2 &&
        (unmatched_wals[0].first != version.next_file_number() ||
         unmatched_wals[1].first != version.next_file_number() + 1U)) {
        return DatabaseCorruption("unflushed WAL generations are not consecutive");
    }

    MemTable mutable_memtable(options);
    std::unique_ptr<ImmutableGeneration> immutable;
    std::uint64_t mutable_generation = version.next_file_number();
    std::uint64_t last_sequence = version.last_sequence();
    std::unique_ptr<WalWriter> wal;
    std::uint64_t previous_wal_max_sequence = version.last_sequence();
    for (std::size_t wal_index = 0; wal_index < unmatched_wals.size(); ++wal_index) {
        MemTable recovered(options);
        std::unique_ptr<RecoveryFile> recovery_file;
        status = PosixRecoveryFile::Open(
            unmatched_wals[wal_index].second,
            &recovery_file
        );
        if (!status.ok()) {
            return status;
        }
        WalRecoveryResult recovery_result;
        status = RecoverWal(
            *recovery_file,
            options,
            &recovered,
            &recovery_result
        );
        if (!status.ok()) {
            return status;
        }
        if (recovery_result.records_recovered != 0 &&
            recovery_result.min_sequence <= previous_wal_max_sequence) {
            return DatabaseCorruption(
                "WAL sequence range overlaps an older durable generation"
            );
        }
        last_sequence = std::max(last_sequence, recovery_result.max_sequence);
        previous_wal_max_sequence = recovery_result.max_sequence;
        ++result.wals_recovered;
        recovery_file.reset();
        if (unmatched_wals.size() == 2 && wal_index == 0) {
            if (recovered.empty()) {
                return DatabaseCorruption(
                    "an Immutable WAL generation must not be empty"
                );
            }
            immutable = std::make_unique<ImmutableGeneration>(
                unmatched_wals[wal_index].first,
                unmatched_wals[wal_index].second,
                std::move(recovered)
            );
        } else {
            mutable_generation = unmatched_wals[wal_index].first;
            mutable_memtable = std::move(recovered);
        }
    }

    const std::string active_wal_path = JoinPath(
        directory,
        GenerationFileName(mutable_generation, "wal")
    );
    status = WalWriter::Open(active_wal_path, options, &wal);
    if (!status.ok()) {
        return status;
    }
    status = environment->SyncDirectory(directory);
    if (!status.ok()) {
        return status;
    }

    output->reset(new Database(
        std::move(directory),
        options,
        std::move(environment),
        std::move(mutable_memtable),
        mutable_generation,
        std::move(wal),
        std::move(immutable),
        std::move(tables),
        last_sequence,
        std::move(version),
        std::move(lock)
    ));
    result.max_sequence = last_sequence;
    result.active_generation = mutable_generation;
    result.version_id = output->get()->version_id();
    result.storage_format_version = kStorageFormatVersion;
    *open_result = result;
    status = output->get()->StartBackgroundWorker();
    if (!status.ok()) {
        output->reset();
        *open_result = {};
        return status;
    }
    return Status::Ok();
}

Database::Database(
    std::string directory,
    Options options,
    std::shared_ptr<FlushEnvironment> environment,
    MemTable mutable_memtable,
    std::uint64_t mutable_generation,
    std::unique_ptr<WalWriter> wal,
    std::unique_ptr<ImmutableGeneration> immutable,
    std::vector<std::unique_ptr<SSTableReader>> tables,
    std::uint64_t last_sequence,
    Version version,
    std::unique_ptr<DatabaseLock> lock
)
    : directory_(std::move(directory)),
      options_(options),
      environment_(std::move(environment)),
      lock_(std::move(lock)),
      version_(std::move(version)),
      mutable_(std::move(mutable_memtable)),
      mutable_generation_(mutable_generation),
      wal_(std::move(wal)),
      immutable_(std::move(immutable)),
      tables_(std::move(tables)),
      last_sequence_(last_sequence) {}

Database::~Database() {
    static_cast<void>(Close());
}

Status Database::StartBackgroundWorker() {
    if (!options_.background_maintenance) {
        return Status::Ok();
    }
    if (immutable_ != nullptr) {
        background_work_requested_ = true;
        background_statistics_.work_scheduled = true;
    }
    try {
        background_thread_ = std::thread(&Database::BackgroundLoop, this);
    } catch (const std::system_error& error) {
        return Status::IOError(
            "start database background worker: " + std::string(error.what())
        );
    }
    background_cv_.notify_all();
    return Status::Ok();
}

void Database::ScheduleBackgroundWorkLocked() {
    if (!options_.background_maintenance || stop_background_) {
        return;
    }
    background_work_requested_ = true;
    background_statistics_.work_scheduled = true;
    background_cv_.notify_all();
}

Status Database::RunBackgroundWorkLocked(
    std::unique_lock<std::shared_mutex>& state_lock
) {
    const bool had_immutable = immutable_ != nullptr;
    if (immutable_ != nullptr && !immutable_->version_published &&
        immutable_->pending_table == nullptr) {
        ImmutableGeneration* const generation = immutable_.get();
        std::unique_ptr<SSTableReader> published;
        state_lock.unlock();
        const auto publish_status = PublishTable(
            directory_,
            generation->generation,
            generation->memtable,
            options_,
            *environment_,
            &published
        );
        state_lock.lock();
        if (!publish_status.ok()) {
            return publish_status;
        }
        if (immutable_.get() != generation) {
            return DatabaseCorruption(
                "Immutable generation changed during background Flush"
            );
        }
        immutable_->pending_table = std::move(published);
    }
    auto work_status = ContinueFlush();
    if (!work_status.ok()) {
        return work_status;
    }
    if (had_immutable) {
        ++background_statistics_.flushes_completed;
    }

    const std::size_t level_zero_tables = static_cast<std::size_t>(std::count_if(
        version_.tables().begin(),
        version_.tables().end(),
        [](const VersionTable& table) { return table.level == 0; }
    ));
    if (!stop_background_ && mutable_.empty() && immutable_ == nullptr &&
        level_zero_tables >= options_.level0_compaction_trigger) {
        work_status = PrepareCompaction();
        if (!work_status.ok()) {
            return work_status;
        }
        if (compaction_ != nullptr) {
            work_status = ContinueCompaction(nullptr);
            if (!work_status.ok()) {
                return work_status;
            }
            ++background_statistics_.compactions_completed;
        }
    }
    return Status::Ok();
}

void Database::BackgroundLoop() {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    for (;;) {
        background_cv_.wait(lock, [this] {
            return stop_background_ || background_work_requested_;
        });
        if (stop_background_) {
            return;
        }
        background_work_requested_ = false;
        background_work_running_ = true;
        background_statistics_.work_scheduled = false;
        background_statistics_.work_running = true;

        const auto work_status = RunBackgroundWorkLocked(lock);
        background_status_ = work_status;
        if (!work_status.ok()) {
            ++background_statistics_.failures;
        }

        background_work_running_ = false;
        background_statistics_.work_running = false;
        background_cv_.notify_all();
    }
}

Status Database::WaitForBackgroundWork() {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    if (!IsRunningLocked()) {
        return Status::Closed("database is not running");
    }
    background_cv_.wait(lock, [this] {
        return !background_work_requested_ && !background_work_running_;
    });
    return background_status_;
}

Status Database::Close() {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    if (state_ == DatabaseState::kClosed) {
        return close_status_;
    }
    if (state_ == DatabaseState::kClosing) {
        lifecycle_cv_.wait(lock, [this] {
            return state_ == DatabaseState::kClosed;
        });
        return close_status_;
    }

    state_ = DatabaseState::kClosing;
    stop_background_ = true;
    background_cv_.notify_all();
    lock.unlock();
    if (background_thread_.joinable()) {
        background_thread_.join();
    }
    lock.lock();

    Status result = Status::Ok();
    if (compaction_ != nullptr) {
        result = ContinueCompaction(nullptr);
    }
    if (result.ok() && immutable_ != nullptr) {
        result = ContinueFlush();
    }
    if (result.ok() && !mutable_.empty()) {
        result = FreezeMutable(false);
        if (result.ok()) {
            result = ContinueFlush();
        }
    }
    wal_.reset();
    lock_.reset();
    close_status_ = result;
    state_ = DatabaseState::kClosed;
    background_work_requested_ = false;
    background_work_running_ = false;
    background_statistics_.work_scheduled = false;
    background_statistics_.work_running = false;
    background_cv_.notify_all();
    lifecycle_cv_.notify_all();
    return close_status_;
}

Status Database::Put(
    std::string_view key,
    std::string_view value,
    WriteOptions write_options
) {
    return Write(ValueType::kValue, key, value, write_options);
}

Status Database::Delete(std::string_view key, WriteOptions write_options) {
    return Write(ValueType::kDeletion, key, {}, write_options);
}

LookupResult Database::Get(
    std::string_view key,
    DatabaseReadStats* operation_stats
) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    DatabaseReadStats operation;
    operation.point_lookups = 1;
    if (!IsRunningLocked()) {
        return FinishRead(
            {Status::Closed("database is not running"), 0, ValueType::kValue, {}},
            operation,
            operation_stats
        );
    }
    auto result = mutable_.Lookup(key);
    if (!result.status.IsNotFound()) {
        if (result.status.ok()) {
            operation.mutable_hits = 1;
        }
        return FinishRead(
            UserVisible(std::move(result)),
            operation,
            operation_stats
        );
    }
    if (immutable_ != nullptr) {
        result = immutable_->memtable.Lookup(key);
        if (!result.status.IsNotFound()) {
            if (result.status.ok()) {
                operation.immutable_hits = 1;
            }
            return FinishRead(
                UserVisible(std::move(result)),
                operation,
                operation_stats
            );
        }
    }
    LookupResult newest{Status::NotFound("key does not exist"), 0,
                        ValueType::kValue, {}};
    for (auto table = tables_.rbegin(); table != tables_.rend(); ++table) {
        SSTableReadStats table_stats;
        result = (*table)->Get(key, &table_stats);
        AccumulateTableStats(table_stats, &operation);
        if (!result.status.ok() && !result.status.IsNotFound()) {
            return FinishRead(std::move(result), operation, operation_stats);
        }
        if (result.status.ok() &&
            (!newest.status.ok() || result.sequence > newest.sequence)) {
            newest = std::move(result);
        }
    }
    if (newest.status.ok()) {
        return FinishRead(
            UserVisible(std::move(newest)),
            operation,
            operation_stats
        );
    }
    return FinishRead(
        {Status::NotFound("key does not exist"), 0, ValueType::kValue, {}},
        operation,
        operation_stats
    );
}

Status Database::Write(
    ValueType type,
    std::string_view key,
    std::string_view value,
    WriteOptions write_options
) {
    WalRecord record;
    record.type = type;
    record.sequence = 1;
    record.key.assign(key);
    record.value.assign(value);
    const auto validation = ValidateWalRecord(record, options_);
    if (!validation.ok()) {
        return validation;
    }
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    if (!IsRunningLocked()) {
        return Status::Closed("database is not running");
    }
    if (!status_.ok()) {
        return status_;
    }
    if (write_options.sync_mode != SyncMode::kSync &&
        write_options.sync_mode != SyncMode::kAsync) {
        return Status::InvalidArgument("write sync mode is invalid");
    }
    if (!options_.background_maintenance && compaction_ != nullptr) {
        const auto compaction_status = ContinueCompaction(nullptr);
        if (!compaction_status.ok()) {
            return compaction_status;
        }
    }
    if (!options_.background_maintenance && immutable_ != nullptr) {
        const auto flush_status = ContinueFlush();
        if (!flush_status.ok()) {
            return flush_status;
        }
    }
    if (!options_.background_maintenance && !mutable_.empty() &&
        mutable_.ApproximateDataSize() >= options_.memtable_size_limit) {
        const auto flush_status = FlushLocked();
        if (!flush_status.ok()) {
            return flush_status;
        }
    }
    while (options_.background_maintenance && immutable_ != nullptr &&
           mutable_.ApproximateDataSize() >= options_.memtable_size_limit &&
           background_status_.ok() && IsRunningLocked()) {
        background_cv_.wait(lock);
    }
    if (!IsRunningLocked()) {
        return Status::Closed("database is not running");
    }
    if (options_.background_maintenance && !background_status_.ok()) {
        return background_status_;
    }
    if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("sequence number space is exhausted");
    }
    if (wal_ == nullptr) {
        return Status::IOError("database has no active WAL");
    }

    ++last_sequence_;
    record.sequence = last_sequence_;
    const auto wal_status = wal_->Append(record, write_options);
    if (!wal_status.ok()) {
        status_ = wal_status;
        return status_;
    }
    const auto memtable_status = type == ValueType::kValue
                                     ? mutable_.Put(record.sequence, key, value)
                                     : mutable_.Delete(record.sequence, key);
    if (!memtable_status.ok()) {
        status_ = DatabaseCorruption(
            "WAL append succeeded but Mutable MemTable apply failed"
        );
        return status_;
    }

    if (mutable_.ApproximateDataSize() >= options_.memtable_size_limit) {
        if (!options_.background_maintenance) {
            return FlushLocked();
        }
        if (immutable_ == nullptr) {
            const auto freeze_status = FreezeMutable(true);
            if (!freeze_status.ok()) {
                background_status_ = freeze_status;
                ++background_statistics_.failures;
            }
            ScheduleBackgroundWorkLocked();
        }
    }
    return Status::Ok();
}

Status Database::Flush() {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    if (!IsRunningLocked()) {
        return Status::Closed("database is not running");
    }
    background_cv_.wait(lock, [this] {
        return !background_work_running_;
    });
    const auto result = FlushLocked();
    if (result.ok()) {
        background_status_ = Status::Ok();
    }
    background_cv_.notify_all();
    return result;
}

Status Database::FlushLocked() {
    if (!status_.ok()) {
        return status_;
    }
    if (compaction_ != nullptr) {
        const auto compaction_status = ContinueCompaction(nullptr);
        if (!compaction_status.ok()) {
            return compaction_status;
        }
    }
    if (immutable_ == nullptr) {
        if (mutable_.empty()) {
            return Status::Ok();
        }
        const auto freeze_status = FreezeMutable(false);
        if (!freeze_status.ok()) {
            return freeze_status;
        }
    }
    return ContinueFlush();
}

Status Database::Compact(CompactionStats* stats) {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    if (!IsRunningLocked()) {
        if (stats != nullptr) {
            *stats = {};
        }
        return Status::Closed("database is not running");
    }
    background_cv_.wait(lock, [this] {
        return !background_work_running_;
    });
    const auto result = CompactLocked(stats);
    if (result.ok()) {
        background_status_ = Status::Ok();
    }
    background_cv_.notify_all();
    return result;
}

Status Database::CompactLocked(CompactionStats* stats) {
    if (stats != nullptr) {
        *stats = {};
    }
    if (!status_.ok()) {
        return status_;
    }
    if (compaction_ != nullptr) {
        return ContinueCompaction(stats);
    }
    if (immutable_ != nullptr || !mutable_.empty()) {
        const auto flush_status = FlushLocked();
        if (!flush_status.ok()) {
            return flush_status;
        }
    }
    if (immutable_ != nullptr || !mutable_.empty() || wal_ == nullptr) {
        return DatabaseCorruption(
            "compaction requires an empty active WAL generation"
        );
    }
    const auto prepare_status = PrepareCompaction();
    if (!prepare_status.ok() || compaction_ == nullptr) {
        return prepare_status;
    }
    return ContinueCompaction(stats);
}

Status Database::PrepareCompaction() {
    if (compaction_ != nullptr || tables_.size() != version_.tables().size()) {
        return DatabaseCorruption("table readers and Version disagree");
    }

    bool have_level_zero = false;
    std::string minimum_key;
    std::string maximum_key;
    for (const auto& table : version_.tables()) {
        if (table.level == 0) {
            if (!have_level_zero) {
                minimum_key = table.metadata.minimum_key;
                maximum_key = table.metadata.maximum_key;
                have_level_zero = true;
            } else {
                minimum_key = std::min(minimum_key, table.metadata.minimum_key);
                maximum_key = std::max(maximum_key, table.metadata.maximum_key);
            }
        }
    }
    if (!have_level_zero) {
        last_compaction_statistics_ = {};
        return Status::Ok();
    }

    auto state = std::make_unique<CompactionState>();
    state->first_output_file_number = version_.next_file_number();
    std::vector<CompactionInput> inputs;
    for (std::size_t index = 0; index < version_.tables().size(); ++index) {
        const auto& version_table = version_.tables()[index];
        const auto& reader = tables_[index];
        if (reader == nullptr ||
            reader->metadata().generation != version_table.metadata.generation) {
            return DatabaseCorruption("table reader order is inconsistent");
        }
        const bool overlaps =
            version_table.level == 1 &&
            version_table.metadata.maximum_key >= minimum_key &&
            version_table.metadata.minimum_key <= maximum_key;
        if (version_table.level != 0 && !overlaps) {
            continue;
        }
        inputs.push_back({version_table.level, reader.get()});
        state->input_file_numbers.insert(version_table.metadata.generation);
        state->cleanup_paths.push_back(reader->path());
        if (state->stats.input_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                version_table.metadata.file_size) {
            return Status::InvalidArgument("compaction byte count overflows");
        }
        state->stats.input_bytes += version_table.metadata.file_size;
    }
    state->stats.input_files =
        static_cast<std::uint64_t>(state->input_file_numbers.size());

    const auto build_status = BuildCompactionOutputs(
        inputs,
        options_,
        true,
        &state->output_memtables,
        &state->build_stats
    );
    if (!build_status.ok()) {
        return build_status;
    }
    if (state->output_memtables.size() >
        std::numeric_limits<std::uint64_t>::max() -
            state->first_output_file_number) {
        return Status::InvalidArgument("compaction file numbers overflow");
    }
    state->stats.input_records = state->build_stats.input_records;
    state->stats.output_records = state->build_stats.output_records;
    state->stats.duplicate_records_dropped =
        state->build_stats.duplicate_records_dropped;
    state->stats.tombstones_dropped = state->build_stats.tombstones_dropped;
    compaction_ = std::move(state);
    return Status::Ok();
}

Status Database::ContinueCompaction(CompactionStats* stats) {
    if (compaction_ == nullptr) {
        return Status::Ok();
    }
    auto& state = *compaction_;

    if (!state.output_memtables.empty() && state.published_outputs.empty()) {
        const auto publish_status = PublishCompactionTables(
            directory_,
            state.first_output_file_number,
            state.output_memtables,
            options_,
            *environment_,
            &state.published_outputs
        );
        if (!publish_status.ok()) {
            return publish_status;
        }
    }

    if (!state.candidate_ready) {
        VersionEdit edit;
        edit.deleted_file_numbers.assign(
            state.input_file_numbers.begin(),
            state.input_file_numbers.end()
        );
        for (const auto& output : state.published_outputs) {
            VersionTable added;
            added.level = 1;
            added.metadata = output->metadata();
            edit.added_tables.push_back(std::move(added));
            if (state.stats.output_bytes >
                std::numeric_limits<std::uint64_t>::max() -
                    output->metadata().file_size) {
                return Status::InvalidArgument("compaction byte count overflows");
            }
            state.stats.output_bytes += output->metadata().file_size;
        }
        state.stats.output_files =
            static_cast<std::uint64_t>(state.published_outputs.size());
        if (!state.published_outputs.empty()) {
            edit.next_file_number =
                state.first_output_file_number + state.stats.output_files;
        }
        const auto apply_status = version_.Apply(edit, options_, &state.candidate);
        if (!apply_status.ok()) {
            return apply_status;
        }
        state.candidate_ready = true;
    }

    if (!state.version_published) {
        const auto manifest_status = PublishManifest(
            directory_,
            state.candidate,
            options_,
            *environment_
        );
        if (!manifest_status.ok()) {
            return manifest_status;
        }

        std::vector<std::unique_ptr<SSTableReader>> live_tables;
        live_tables.reserve(
            tables_.size() - state.input_file_numbers.size() +
            state.published_outputs.size()
        );
        for (auto& table : tables_) {
            if (state.input_file_numbers.find(table->metadata().generation) ==
                state.input_file_numbers.end()) {
                live_tables.push_back(std::move(table));
            }
        }
        for (auto& table : state.published_outputs) {
            live_tables.push_back(std::move(table));
        }
        std::sort(
            live_tables.begin(),
            live_tables.end(),
            [](const auto& left, const auto& right) {
                return left->metadata().generation < right->metadata().generation;
            }
        );
        tables_ = std::move(live_tables);
        version_ = state.candidate;
        state.version_published = true;

        if (!state.output_memtables.empty()) {
            state.cleanup_paths.push_back(WalPath(mutable_generation_));
            wal_.reset();
            mutable_generation_ = version_.next_file_number();
            state.wal_generation_advanced = true;
        }
    }

    while (state.cleanup_index < state.cleanup_paths.size()) {
        const auto remove_status = environment_->RemoveFile(
            state.cleanup_paths[state.cleanup_index]
        );
        if (!remove_status.ok()) {
            return remove_status;
        }
        ++state.cleanup_index;
    }
    if (!state.cleanup_synced) {
        const auto sync_status = environment_->SyncDirectory(directory_);
        if (!sync_status.ok()) {
            return sync_status;
        }
        state.cleanup_synced = true;
    }
    if (state.wal_generation_advanced) {
        const auto wal_status = OpenMutableWal();
        if (!wal_status.ok()) {
            return wal_status;
        }
    }

    state.stats.bytes_reclaimed = state.stats.input_bytes > state.stats.output_bytes
        ? state.stats.input_bytes - state.stats.output_bytes
        : 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - state.started
    ).count();
    state.stats.elapsed_microseconds = elapsed > 0
        ? static_cast<std::uint64_t>(elapsed)
        : 0;
    last_compaction_statistics_ = state.stats;
    if (stats != nullptr) {
        *stats = state.stats;
    }
    compaction_.reset();
    return Status::Ok();
}

std::size_t Database::level_table_count(std::uint32_t level) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return static_cast<std::size_t>(std::count_if(
        version_.tables().begin(),
        version_.tables().end(),
        [level](const VersionTable& table) { return table.level == level; }
    ));
}

std::uint64_t Database::last_sequence() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return last_sequence_;
}

std::uint64_t Database::mutable_generation() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return mutable_generation_;
}

std::size_t Database::mutable_size() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return mutable_.ApproximateDataSize();
}

bool Database::has_immutable() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return immutable_ != nullptr;
}

std::size_t Database::flushed_table_count() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return tables_.size();
}

std::uint64_t Database::version_id() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return version_.id();
}

DatabaseReadStats Database::read_statistics() const {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    return read_statistics_;
}

CompactionStats Database::last_compaction_statistics() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return last_compaction_statistics_;
}

BackgroundMaintenanceStats Database::background_statistics() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return background_statistics_;
}

Status Database::background_status() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return background_status_;
}

DatabaseState Database::state() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return state_;
}

Status Database::status() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return status_;
}

bool Database::IsRunningLocked() const noexcept {
    return state_ == DatabaseState::kRunning;
}

Status Database::FreezeMutable(bool open_next_wal) {
    if (immutable_ != nullptr || mutable_.empty() || wal_ == nullptr) {
        return Status::InvalidArgument("Mutable MemTable cannot be frozen");
    }
    if (mutable_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("MemTable generation space is exhausted");
    }
    const std::uint64_t frozen_generation = mutable_generation_;
    wal_.reset();
    immutable_ = std::make_unique<ImmutableGeneration>(
        frozen_generation,
        WalPath(frozen_generation),
        std::move(mutable_)
    );
    ++mutable_generation_;
    mutable_ = MemTable(options_);
    if (open_next_wal) {
        return OpenMutableWal();
    }
    return Status::Ok();
}

Status Database::ContinueFlush() {
    if (immutable_ == nullptr) {
        return Status::Ok();
    }

    if (!immutable_->version_published) {
        if (immutable_->pending_table == nullptr) {
            std::unique_ptr<SSTableReader> published;
            const auto publish_status = PublishTable(
                directory_,
                immutable_->generation,
                immutable_->memtable,
                options_,
                *environment_,
                &published
            );
            if (!publish_status.ok()) {
                return publish_status;
            }
            immutable_->pending_table = std::move(published);
        }

        VersionTable added;
        added.level = 0;
        added.metadata = immutable_->pending_table->metadata();
        VersionEdit edit;
        edit.added_tables.push_back(std::move(added));
        edit.next_file_number = mutable_generation_;
        edit.last_sequence = immutable_->maximum_sequence;
        Version candidate;
        auto status = version_.Apply(edit, options_, &candidate);
        if (!status.ok()) {
            return status;
        }
        status = PublishManifest(
            directory_,
            candidate,
            options_,
            *environment_
        );
        if (!status.ok()) {
            return status;
        }
        tables_.push_back(std::move(immutable_->pending_table));
        version_ = std::move(candidate);
        immutable_->version_published = true;
    }

    if (!immutable_->wal_removed_and_synced) {
        auto status = environment_->RemoveFile(immutable_->wal_path);
        if (!status.ok()) {
            return status;
        }
        status = environment_->SyncDirectory(directory_);
        if (!status.ok()) {
            return status;
        }
        immutable_->wal_removed_and_synced = true;
    }

    const auto open_status = OpenMutableWal();
    if (!open_status.ok()) {
        return open_status;
    }
    immutable_.reset();
    return Status::Ok();
}

Status Database::OpenMutableWal() {
    if (wal_ != nullptr) {
        return Status::Ok();
    }
    std::unique_ptr<WalWriter> new_wal;
    auto status = WalWriter::Open(
        WalPath(mutable_generation_),
        options_,
        &new_wal
    );
    if (!status.ok()) {
        return status;
    }
    status = environment_->SyncDirectory(directory_);
    if (!status.ok()) {
        return status;
    }
    wal_ = std::move(new_wal);
    return Status::Ok();
}

std::string Database::WalPath(std::uint64_t generation) const {
    return JoinPath(directory_, GenerationFileName(generation, "wal"));
}

LookupResult Database::UserVisible(LookupResult result) {
    if (result.deleted()) {
        result.status = Status::NotFound("key was deleted");
        result.value.clear();
    }
    return result;
}

LookupResult Database::FinishRead(
    LookupResult result,
    const DatabaseReadStats& operation,
    DatabaseReadStats* operation_stats
) const {
    std::lock_guard<std::mutex> lock(statistics_mutex_);
    AccumulateDatabaseStats(operation, &read_statistics_);
    if (operation_stats != nullptr) {
        *operation_stats = operation;
    }
    return result;
}

}  // namespace minikv
