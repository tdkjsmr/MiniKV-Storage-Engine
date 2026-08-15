#include "minikv/recovery.hpp"

#include <limits>
#include <string>
#include <utility>

#include "minikv/wal.hpp"

namespace minikv {
namespace {

Status TruncateIncompleteTail(
    RecoveryFile& file,
    std::uint64_t file_size,
    std::uint64_t valid_bytes,
    WalRecoveryResult* result
) {
    const auto truncate_status = file.Truncate(valid_bytes);
    if (!truncate_status.ok()) {
        return truncate_status;
    }
    const auto sync_status = file.Sync();
    if (!sync_status.ok()) {
        return sync_status;
    }

    result->valid_bytes = valid_bytes;
    result->discarded_tail_bytes = file_size - valid_bytes;
    result->tail_truncated = true;
    return Status::Ok();
}

Status RecoveryCorruption(std::uint64_t offset, std::string message) {
    return Status::Corruption(
        "WAL recovery at offset " + std::to_string(offset) + ": " + std::move(message)
    );
}

}  // namespace

Status RecoverWal(
    RecoveryFile& file,
    const Options& options,
    MemTable* memtable,
    WalRecoveryResult* result
) {
    if (memtable == nullptr || result == nullptr) {
        return Status::InvalidArgument("WAL recovery outputs must not be null");
    }

    std::uint64_t file_size = 0;
    const auto size_status = file.Size(&file_size);
    if (!size_status.ok()) {
        return size_status;
    }

    MemTable recovered(options);
    WalRecoveryResult recovered_result;
    std::uint64_t offset = 0;

    while (offset < file_size) {
        const std::uint64_t remaining = file_size - offset;
        if (remaining < kWalHeaderSize) {
            const auto tail_status = TruncateIncompleteTail(
                file,
                file_size,
                offset,
                &recovered_result
            );
            if (!tail_status.ok()) {
                return tail_status;
            }
            *memtable = std::move(recovered);
            *result = recovered_result;
            return Status::Ok();
        }

        std::string header(kWalHeaderSize, '\0');
        const auto header_read_status = ReadAllAt(file, offset, &header);
        if (header_read_status.IsIncomplete()) {
            const auto tail_status = TruncateIncompleteTail(
                file,
                file_size,
                offset,
                &recovered_result
            );
            if (!tail_status.ok()) {
                return tail_status;
            }
            *memtable = std::move(recovered);
            *result = recovered_result;
            return Status::Ok();
        }
        if (!header_read_status.ok()) {
            return header_read_status;
        }

        const auto decoded_header = DecodeWalRecordHeader(header, options);
        if (!decoded_header.status.ok()) {
            return RecoveryCorruption(offset, decoded_header.status.ToString());
        }

        const auto record_size = static_cast<std::uint64_t>(decoded_header.record_size);
        if (record_size > remaining) {
            const auto tail_status = TruncateIncompleteTail(
                file,
                file_size,
                offset,
                &recovered_result
            );
            if (!tail_status.ok()) {
                return tail_status;
            }
            *memtable = std::move(recovered);
            *result = recovered_result;
            return Status::Ok();
        }

        std::string encoded_record(
            static_cast<std::size_t>(decoded_header.record_size),
            '\0'
        );
        const auto record_read_status = ReadAllAt(file, offset, &encoded_record);
        if (record_read_status.IsIncomplete()) {
            const auto tail_status = TruncateIncompleteTail(
                file,
                file_size,
                offset,
                &recovered_result
            );
            if (!tail_status.ok()) {
                return tail_status;
            }
            *memtable = std::move(recovered);
            *result = recovered_result;
            return Status::Ok();
        }
        if (!record_read_status.ok()) {
            return record_read_status;
        }

        const auto decoded_record = DecodeWalRecord(encoded_record, options);
        if (!decoded_record.status.ok()) {
            return RecoveryCorruption(offset, decoded_record.status.ToString());
        }
        if (decoded_record.record.sequence <= recovered_result.max_sequence) {
            return RecoveryCorruption(
                offset,
                "sequence numbers are not strictly increasing"
            );
        }

        const auto apply_status = decoded_record.record.type == ValueType::kValue
                                      ? recovered.Put(
                                            decoded_record.record.sequence,
                                            decoded_record.record.key,
                                            decoded_record.record.value
                                        )
                                      : recovered.Delete(
                                            decoded_record.record.sequence,
                                            decoded_record.record.key
                                        );
        if (!apply_status.ok()) {
            return RecoveryCorruption(
                offset,
                "record cannot be applied: " + apply_status.ToString()
            );
        }

        if (recovered_result.records_recovered ==
            std::numeric_limits<std::size_t>::max()) {
            return Status::Corruption("WAL recovery record count overflows size_t");
        }
        if (recovered_result.records_recovered == 0) {
            recovered_result.min_sequence = decoded_record.record.sequence;
        }
        ++recovered_result.records_recovered;
        recovered_result.max_sequence = decoded_record.record.sequence;
        offset += record_size;
        recovered_result.valid_bytes = offset;
    }

    *memtable = std::move(recovered);
    *result = recovered_result;
    return Status::Ok();
}

}  // namespace minikv
