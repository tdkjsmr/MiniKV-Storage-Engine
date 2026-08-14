#include "minikv/database.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "minikv/file.hpp"
#include "minikv/recovery.hpp"

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

Status ValidateTableSequenceOrder(
    const std::vector<std::unique_ptr<SSTableReader>>& tables,
    std::uint64_t* max_sequence
) {
    if (max_sequence == nullptr) {
        return Status::InvalidArgument("sequence output must not be null");
    }
    *max_sequence = 0;
    for (const auto& table : tables) {
        if (table->metadata().minimum_sequence <= *max_sequence) {
            return DatabaseCorruption(
                "table generations contain overlapping sequence ranges"
            );
        }
        *max_sequence = table->metadata().maximum_sequence;
    }
    return Status::Ok();
}

}  // namespace

Database::ImmutableGeneration::ImmutableGeneration(
    std::uint64_t immutable_generation,
    std::string immutable_wal_path,
    MemTable immutable_memtable
)
    : generation(immutable_generation),
      wal_path(std::move(immutable_wal_path)),
      memtable(std::move(immutable_memtable)) {}

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
            std::numeric_limits<std::uint32_t>::max() - kTableBlockHeaderSize) {
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

    std::map<std::uint64_t, std::string> table_paths;
    std::map<std::uint64_t, std::string> wal_paths;
    fs::directory_iterator iterator(directory, filesystem_error);
    const fs::directory_iterator end;
    while (!filesystem_error && iterator != end) {
        if (iterator->is_regular_file(filesystem_error)) {
            const std::string name = iterator->path().filename().string();
            std::uint64_t generation = 0;
            if (ParseGenerationFile(name, "sst", &generation)) {
                table_paths.emplace(generation, iterator->path().string());
            } else if (ParseGenerationFile(name, "wal", &generation)) {
                wal_paths.emplace(generation, iterator->path().string());
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

    std::vector<std::unique_ptr<SSTableReader>> tables;
    tables.reserve(table_paths.size());
    for (const auto& [generation, path] : table_paths) {
        std::unique_ptr<SSTableReader> table;
        auto status = SSTableReader::Open(path, options, &table);
        if (!status.ok()) {
            return status;
        }
        if (table->metadata().generation != generation) {
            return DatabaseCorruption(
                "table filename and encoded generation disagree"
            );
        }
        tables.push_back(std::move(table));
    }

    std::uint64_t last_sequence = 0;
    auto status = ValidateTableSequenceOrder(tables, &last_sequence);
    if (!status.ok()) {
        return status;
    }

    DatabaseOpenResult result;
    result.tables_loaded = tables.size();
    std::vector<std::pair<std::uint64_t, std::string>> unmatched_wals;
    for (const auto& [generation, path] : wal_paths) {
        if (table_paths.find(generation) != table_paths.end()) {
            status = environment->RemoveFile(path);
            if (!status.ok()) {
                return status;
            }
            ++result.obsolete_wals_removed;
        } else {
            unmatched_wals.emplace_back(generation, path);
        }
    }
    if (result.obsolete_wals_removed != 0) {
        status = environment->SyncDirectory(directory);
        if (!status.ok()) {
            return status;
        }
    }
    if (unmatched_wals.size() > 1) {
        return DatabaseCorruption("multiple unflushed WAL generations exist");
    }

    MemTable mutable_memtable(options);
    if (!tables.empty() &&
        tables.back()->metadata().generation ==
            std::numeric_limits<std::uint64_t>::max()) {
        return DatabaseCorruption("table generation space is exhausted");
    }
    std::uint64_t mutable_generation =
        tables.empty() ? 1 : tables.back()->metadata().generation + 1;
    std::unique_ptr<WalWriter> wal;
    if (!unmatched_wals.empty()) {
        mutable_generation = unmatched_wals.front().first;
        if (!tables.empty() &&
            mutable_generation <= tables.back()->metadata().generation) {
            return DatabaseCorruption("unflushed WAL generation is stale");
        }
        std::unique_ptr<RecoveryFile> recovery_file;
        status = PosixRecoveryFile::Open(unmatched_wals.front().second, &recovery_file);
        if (!status.ok()) {
            return status;
        }
        WalRecoveryResult recovery_result;
        status = RecoverWal(
            *recovery_file,
            options,
            &mutable_memtable,
            &recovery_result
        );
        if (!status.ok()) {
            return status;
        }
        for (const auto& record : mutable_memtable.Records()) {
            if (record.sequence <= last_sequence) {
                return DatabaseCorruption(
                    "WAL sequence range overlaps published tables"
                );
            }
        }
        last_sequence = std::max(last_sequence, recovery_result.max_sequence);
        ++result.wals_recovered;
        recovery_file.reset();
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
        std::move(tables),
        last_sequence
    ));
    result.max_sequence = last_sequence;
    result.active_generation = mutable_generation;
    *open_result = result;
    return Status::Ok();
}

Database::Database(
    std::string directory,
    Options options,
    std::shared_ptr<FlushEnvironment> environment,
    MemTable mutable_memtable,
    std::uint64_t mutable_generation,
    std::unique_ptr<WalWriter> wal,
    std::vector<std::unique_ptr<SSTableReader>> tables,
    std::uint64_t last_sequence
)
    : directory_(std::move(directory)),
      options_(options),
      environment_(std::move(environment)),
      mutable_(std::move(mutable_memtable)),
      mutable_generation_(mutable_generation),
      wal_(std::move(wal)),
      tables_(std::move(tables)),
      last_sequence_(last_sequence) {}

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

LookupResult Database::Get(std::string_view key) const {
    auto result = mutable_.Lookup(key);
    if (!result.status.IsNotFound()) {
        return UserVisible(std::move(result));
    }
    if (immutable_ != nullptr) {
        result = immutable_->memtable.Lookup(key);
        if (!result.status.IsNotFound()) {
            return UserVisible(std::move(result));
        }
    }
    LookupResult newest{Status::NotFound("key does not exist"), 0,
                        ValueType::kValue, {}};
    for (auto table = tables_.rbegin(); table != tables_.rend(); ++table) {
        result = (*table)->Get(key);
        if (!result.status.ok() && !result.status.IsNotFound()) {
            return result;
        }
        if (result.status.ok() &&
            (!newest.status.ok() || result.sequence > newest.sequence)) {
            newest = std::move(result);
        }
    }
    if (newest.status.ok()) {
        return UserVisible(std::move(newest));
    }
    return {
        Status::NotFound("key does not exist"),
        0,
        ValueType::kValue,
        {},
    };
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
    if (!status_.ok()) {
        return status_;
    }
    if (write_options.sync_mode != SyncMode::kSync &&
        write_options.sync_mode != SyncMode::kAsync) {
        return Status::InvalidArgument("write sync mode is invalid");
    }
    if (immutable_ != nullptr) {
        const auto flush_status = ContinueFlush();
        if (!flush_status.ok()) {
            return flush_status;
        }
    }
    if (!mutable_.empty() &&
        mutable_.ApproximateDataSize() >= options_.memtable_size_limit) {
        const auto flush_status = Flush();
        if (!flush_status.ok()) {
            return flush_status;
        }
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
        return Flush();
    }
    return Status::Ok();
}

Status Database::Flush() {
    if (!status_.ok()) {
        return status_;
    }
    if (immutable_ == nullptr) {
        if (mutable_.empty()) {
            return Status::Ok();
        }
        const auto freeze_status = FreezeMutable();
        if (!freeze_status.ok()) {
            return freeze_status;
        }
    }
    return ContinueFlush();
}

Status Database::FreezeMutable() {
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
    return Status::Ok();
}

Status Database::ContinueFlush() {
    if (immutable_ == nullptr) {
        return Status::Ok();
    }

    if (!immutable_->table_published) {
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
        tables_.push_back(std::move(published));
        immutable_->table_published = true;
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

}  // namespace minikv
