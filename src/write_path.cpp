#include "minikv/write_path.hpp"

#include <limits>
#include <string>
#include <utility>

namespace minikv {

Status WritePath::Create(
    Options options,
    std::unique_ptr<WritableFile> wal_file,
    std::unique_ptr<WritePath>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("WritePath output pointer must not be null");
    }
    output->reset();
    if (wal_file == nullptr) {
        return Status::InvalidArgument("WritePath WAL file must not be null");
    }

    output->reset(new WritePath(options, std::move(wal_file)));
    return Status::Ok();
}

Status WritePath::Open(
    std::string path,
    Options options,
    std::unique_ptr<WritePath>* output,
    WalRecoveryResult* recovery_result
) {
    if (output == nullptr || recovery_result == nullptr) {
        return Status::InvalidArgument("WritePath Open outputs must not be null");
    }
    output->reset();
    *recovery_result = {};
    if (path.empty()) {
        return Status::InvalidArgument("WritePath WAL path must not be empty");
    }

    std::unique_ptr<RecoveryFile> recovery_file;
    auto open_status = PosixRecoveryFile::Open(path, &recovery_file);
    if (!open_status.ok()) {
        return open_status;
    }

    MemTable recovered_memtable(options);
    WalRecoveryResult recovered_result;
    const auto recovery_status = RecoverWal(
        *recovery_file,
        options,
        &recovered_memtable,
        &recovered_result
    );
    if (!recovery_status.ok()) {
        return recovery_status;
    }
    recovery_file.reset();

    std::unique_ptr<WritableFile> wal_file;
    open_status = PosixWritableFile::OpenAppend(path, &wal_file);
    if (!open_status.ok()) {
        return open_status;
    }

    output->reset(new WritePath(
        options,
        std::move(wal_file),
        std::move(recovered_memtable),
        recovered_result.max_sequence
    ));
    *recovery_result = recovered_result;
    return Status::Ok();
}

WritePath::WritePath(Options options, std::unique_ptr<WritableFile> wal_file)
    : options_(options), wal_(options, std::move(wal_file)), memtable_(options) {}

WritePath::WritePath(
    Options options,
    std::unique_ptr<WritableFile> wal_file,
    MemTable recovered_memtable,
    std::uint64_t last_sequence
)
    : options_(options),
      wal_(options, std::move(wal_file)),
      memtable_(std::move(recovered_memtable)),
      last_sequence_(last_sequence) {}

Status WritePath::Put(
    std::string_view key,
    std::string_view value,
    WriteOptions write_options
) {
    return Write(ValueType::kValue, key, value, write_options);
}

Status WritePath::Delete(std::string_view key, WriteOptions write_options) {
    return Write(ValueType::kDeletion, key, {}, write_options);
}

LookupResult WritePath::Get(std::string_view key) const {
    return memtable_.Get(key);
}

Status WritePath::Write(
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
    if (write_options.sync_mode != SyncMode::kSync &&
        write_options.sync_mode != SyncMode::kAsync) {
        return Status::InvalidArgument("write sync mode is invalid");
    }

    if (!status_.ok()) {
        return status_;
    }
    if (wal_.failed()) {
        status_ = wal_.status();
        return status_;
    }
    if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("sequence number space is exhausted");
    }

    // Sequence numbers are never reused after an I/O attempt. A failed sync
    // can leave a complete record in the kernel page cache, so reusing its
    // sequence number would make later recovery ambiguous.
    ++last_sequence_;
    record.sequence = last_sequence_;

    const auto wal_status = wal_.Append(record, write_options);
    if (!wal_status.ok()) {
        status_ = wal_status;
        return status_;
    }

    const auto memtable_status = type == ValueType::kValue
                                     ? memtable_.Put(record.sequence, key, value)
                                     : memtable_.Delete(record.sequence, key);
    if (!memtable_status.ok()) {
        status_ = Status::Corruption(
            "WAL append succeeded but MemTable apply failed: " +
            memtable_status.ToString()
        );
        return status_;
    }
    return Status::Ok();
}

}  // namespace minikv
