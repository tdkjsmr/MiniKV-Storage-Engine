#include "minikv/wal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "minikv/coding.hpp"

namespace minikv {
namespace {

constexpr std::array<char, 4> kWalMagic = {'M', 'K', 'V', 'W'};
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 5;
constexpr std::size_t kHeaderSizeOffset = 6;
constexpr std::size_t kRecordSizeOffset = 8;
constexpr std::size_t kSequenceOffset = 12;
constexpr std::size_t kKeySizeOffset = 20;
constexpr std::size_t kValueSizeOffset = 24;
constexpr std::size_t kChecksumOffset = 28;

bool IsKnownType(ValueType type) {
    return type == ValueType::kValue || type == ValueType::kDeletion;
}

Status ValidateLengths(
    std::size_t key_size,
    std::size_t value_size,
    ValueType type,
    const Options& options
) {
    if (key_size == 0) {
        return Status::InvalidArgument("WAL key must not be empty");
    }
    if (key_size > options.max_key_size) {
        return Status::InvalidArgument("WAL key exceeds configured maximum size");
    }
    if (value_size > options.max_value_size) {
        return Status::InvalidArgument("WAL value exceeds configured maximum size");
    }
    if (key_size > std::numeric_limits<std::uint32_t>::max() ||
        value_size > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("WAL key or value cannot fit in the file format");
    }
    if (type == ValueType::kDeletion && value_size != 0) {
        return Status::InvalidArgument("WAL deletion record must not contain a value");
    }
    return Status::Ok();
}

std::uint32_t RecordChecksum(std::string_view header_prefix, std::string_view payload) {
    std::string checksum_input;
    checksum_input.reserve(header_prefix.size() + payload.size());
    checksum_input.append(header_prefix);
    checksum_input.append(payload);
    return Crc32c(checksum_input);
}

Status Corrupt(std::string message) {
    return Status::Corruption("WAL record: " + std::move(message));
}

}  // namespace

Status ValidateWalRecord(const WalRecord& record, const Options& options) {
    if (!IsKnownType(record.type)) {
        return Status::InvalidArgument("WAL record type is invalid");
    }
    if (record.sequence == 0) {
        return Status::InvalidArgument("WAL sequence number must be greater than zero");
    }
    return ValidateLengths(record.key.size(), record.value.size(), record.type, options);
}

Status EncodeWalRecord(
    const WalRecord& record,
    const Options& options,
    std::string* destination
) {
    if (destination == nullptr) {
        return Status::InvalidArgument("WAL encoding destination must not be null");
    }
    destination->clear();

    const auto validation = ValidateWalRecord(record, options);
    if (!validation.ok()) {
        return validation;
    }

    if (record.key.size() >
        std::numeric_limits<std::size_t>::max() - record.value.size()) {
        return Status::InvalidArgument("WAL payload size overflows size_t");
    }
    const std::size_t payload_size = record.key.size() + record.value.size();
    if (payload_size > std::numeric_limits<std::uint32_t>::max() - kWalHeaderSize) {
        return Status::InvalidArgument("WAL record cannot fit in the file format");
    }
    const auto record_size = static_cast<std::uint32_t>(kWalHeaderSize + payload_size);

    std::string header_prefix;
    header_prefix.reserve(kChecksumOffset);
    header_prefix.append(kWalMagic.data(), kWalMagic.size());
    header_prefix.push_back(static_cast<char>(kWalFormatVersion));
    header_prefix.push_back(static_cast<char>(record.type));
    PutFixed16(header_prefix, static_cast<std::uint16_t>(kWalHeaderSize));
    PutFixed32(header_prefix, record_size);
    PutFixed64(header_prefix, record.sequence);
    PutFixed32(header_prefix, static_cast<std::uint32_t>(record.key.size()));
    PutFixed32(header_prefix, static_cast<std::uint32_t>(record.value.size()));

    std::string payload;
    payload.reserve(payload_size);
    payload.append(record.key);
    payload.append(record.value);
    const std::uint32_t checksum = RecordChecksum(header_prefix, payload);

    destination->reserve(record_size);
    destination->append(header_prefix);
    PutFixed32(*destination, checksum);
    destination->append(payload);
    return Status::Ok();
}

WalHeaderDecodeResult DecodeWalRecordHeader(
    std::string_view input,
    const Options& options
) {
    if (input.size() < kWalHeaderSize) {
        return {
            Status::Incomplete("WAL record header is incomplete"),
        };
    }

    if (!std::equal(kWalMagic.begin(), kWalMagic.end(), input.begin())) {
        return {Corrupt("magic does not match")};
    }

    const auto version = static_cast<std::uint8_t>(input[kVersionOffset]);
    if (version != kWalFormatVersion) {
        return {Corrupt("format version is unsupported")};
    }

    const auto type = static_cast<ValueType>(static_cast<std::uint8_t>(input[kTypeOffset]));
    if (!IsKnownType(type)) {
        return {Corrupt("record type is invalid")};
    }

    std::uint16_t header_size = 0;
    std::uint32_t record_size = 0;
    std::uint64_t sequence = 0;
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;
    std::uint32_t expected_checksum = 0;
    if (!DecodeFixed16(input.substr(kHeaderSizeOffset), &header_size) ||
        !DecodeFixed32(input.substr(kRecordSizeOffset), &record_size) ||
        !DecodeFixed64(input.substr(kSequenceOffset), &sequence) ||
        !DecodeFixed32(input.substr(kKeySizeOffset), &key_size) ||
        !DecodeFixed32(input.substr(kValueSizeOffset), &value_size) ||
        !DecodeFixed32(input.substr(kChecksumOffset), &expected_checksum)) {
        return {Corrupt("fixed header fields cannot be decoded")};
    }

    if (header_size != kWalHeaderSize) {
        return {Corrupt("header size is invalid")};
    }
    if (sequence == 0) {
        return {Corrupt("sequence number is zero")};
    }

    const auto length_validation = ValidateLengths(key_size, value_size, type, options);
    if (!length_validation.ok()) {
        return {Corrupt(length_validation.message())};
    }

    const auto key_size_in_memory = static_cast<std::size_t>(key_size);
    const auto value_size_in_memory = static_cast<std::size_t>(value_size);
    if (key_size_in_memory >
        std::numeric_limits<std::size_t>::max() - value_size_in_memory) {
        return {Corrupt("payload size overflows size_t")};
    }
    const std::size_t payload_size = key_size_in_memory + value_size_in_memory;
    const std::size_t calculated_size = kWalHeaderSize + payload_size;
    if (record_size != calculated_size) {
        return {Corrupt("record size does not match key and value lengths")};
    }

    return {
        Status::Ok(),
        type,
        record_size,
        sequence,
        key_size,
        value_size,
        expected_checksum,
    };
}

WalDecodeResult DecodeWalRecord(std::string_view input, const Options& options) {
    const auto header = DecodeWalRecordHeader(input, options);
    if (!header.status.ok()) {
        return {header.status, 0, {}};
    }

    const auto calculated_size = static_cast<std::size_t>(header.record_size);
    if (input.size() < calculated_size) {
        return {Status::Incomplete("WAL record payload is incomplete"), 0, {}};
    }

    const auto payload_size = calculated_size - kWalHeaderSize;
    const std::string_view payload = input.substr(kWalHeaderSize, payload_size);
    const std::uint32_t actual_checksum = RecordChecksum(
        input.substr(0, kChecksumOffset),
        payload
    );
    if (actual_checksum != header.checksum) {
        return {Corrupt("checksum mismatch"), 0, {}};
    }

    WalRecord record;
    record.type = header.type;
    record.sequence = header.sequence;
    record.key.assign(payload.substr(0, header.key_size));
    record.value.assign(payload.substr(header.key_size, header.value_size));
    return {Status::Ok(), calculated_size, std::move(record)};
}

WalWriter::WalWriter(Options options, std::unique_ptr<WritableFile> file)
    : options_(options), file_(std::move(file)) {
    if (file_ == nullptr) {
        status_ = Status::InvalidArgument("WAL file must not be null");
    }
}

Status WalWriter::Open(
    std::string path,
    Options options,
    std::unique_ptr<WalWriter>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("WAL writer output pointer must not be null");
    }
    output->reset();

    std::unique_ptr<WritableFile> file;
    const auto open_status = PosixWritableFile::OpenAppend(std::move(path), &file);
    if (!open_status.ok()) {
        return open_status;
    }

    output->reset(new WalWriter(options, std::move(file)));
    return Status::Ok();
}

Status WalWriter::Append(const WalRecord& record, WriteOptions write_options) {
    if (failed()) {
        return status_;
    }
    if (write_options.sync_mode != SyncMode::kSync &&
        write_options.sync_mode != SyncMode::kAsync) {
        return Status::InvalidArgument("WAL sync mode is invalid");
    }

    std::string encoded;
    const auto encoding_status = EncodeWalRecord(record, options_, &encoded);
    if (!encoding_status.ok()) {
        return encoding_status;
    }

    const auto write_status = WriteAll(*file_, encoded);
    if (!write_status.ok()) {
        return RememberFailure(write_status);
    }

    if (write_options.sync_mode == SyncMode::kSync) {
        const auto sync_status = file_->Sync();
        if (!sync_status.ok()) {
            return RememberFailure(sync_status);
        }
    }
    return Status::Ok();
}

std::string_view WalWriter::file_name() const noexcept {
    if (file_ == nullptr) {
        return {};
    }
    return file_->name();
}

Status WalWriter::RememberFailure(Status status) {
    if (status.ok()) {
        status = Status::IOError("WAL entered a failed state without an error");
    }
    status_ = std::move(status);
    return status_;
}

}  // namespace minikv
