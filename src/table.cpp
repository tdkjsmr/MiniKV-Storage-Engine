#include "minikv/table.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "minikv/coding.hpp"
#include "minikv/wal.hpp"

namespace minikv {
namespace {

constexpr std::array<char, 4> kTableMagic = {'M', 'K', 'S', 'T'};
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kHeaderSizeOffset = 6;
constexpr std::size_t kGenerationOffset = 8;
constexpr std::size_t kRecordCountOffset = 16;
constexpr std::size_t kPayloadSizeOffset = 24;
constexpr std::size_t kChecksumOffset = 28;

Status TableCorruption(std::string message) {
    return Status::Corruption("table: " + std::move(message));
}

std::uint32_t TableChecksum(
    std::string_view header_prefix,
    std::string_view payload
) {
    std::string input;
    input.reserve(header_prefix.size() + payload.size());
    input.append(header_prefix);
    input.append(payload);
    return Crc32c(input);
}

}  // namespace

TableData::TableData(std::uint64_t table_generation, Options options)
    : generation(table_generation), records(options) {}

Status EncodeTable(
    std::uint64_t generation,
    const MemTable& memtable,
    const Options& options,
    std::string* destination
) {
    if (destination == nullptr) {
        return Status::InvalidArgument("table destination must not be null");
    }
    destination->clear();
    if (generation == 0) {
        return Status::InvalidArgument("table generation must be greater than zero");
    }
    if (memtable.empty()) {
        return Status::InvalidArgument("table must contain at least one record");
    }

    const auto records = memtable.Records();
    std::string payload;
    for (const auto& entry : records) {
        WalRecord record;
        record.type = entry.type;
        record.sequence = entry.sequence;
        record.key = entry.key;
        record.value = entry.value;
        std::string encoded;
        const auto encode_status = EncodeWalRecord(record, options, &encoded);
        if (!encode_status.ok()) {
            return encode_status;
        }
        if (encoded.size() > std::numeric_limits<std::uint32_t>::max() ||
            payload.size() >
                std::numeric_limits<std::uint32_t>::max() - encoded.size()) {
            return Status::InvalidArgument("table payload exceeds V3 format limit");
        }
        payload.append(encoded);
    }

    std::string header_prefix;
    header_prefix.reserve(kChecksumOffset);
    header_prefix.append(kTableMagic.data(), kTableMagic.size());
    header_prefix.push_back(static_cast<char>(kTableFormatVersion));
    header_prefix.push_back('\0');
    PutFixed16(header_prefix, static_cast<std::uint16_t>(kTableHeaderSize));
    PutFixed64(header_prefix, generation);
    PutFixed64(header_prefix, static_cast<std::uint64_t>(records.size()));
    PutFixed32(header_prefix, static_cast<std::uint32_t>(payload.size()));

    destination->reserve(kTableHeaderSize + payload.size());
    destination->append(header_prefix);
    PutFixed32(*destination, TableChecksum(header_prefix, payload));
    destination->append(payload);
    return Status::Ok();
}

Status DecodeTable(
    std::string_view input,
    const Options& options,
    std::unique_ptr<TableData>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("table output pointer must not be null");
    }
    output->reset();
    if (input.size() < kTableHeaderSize) {
        return TableCorruption("header is incomplete");
    }
    if (!std::equal(kTableMagic.begin(), kTableMagic.end(), input.begin())) {
        return TableCorruption("magic does not match");
    }
    if (static_cast<std::uint8_t>(input[kVersionOffset]) != kTableFormatVersion) {
        return TableCorruption("format version is unsupported");
    }

    std::uint16_t header_size = 0;
    std::uint64_t generation = 0;
    std::uint64_t record_count = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t expected_checksum = 0;
    if (!DecodeFixed16(input.substr(kHeaderSizeOffset), &header_size) ||
        !DecodeFixed64(input.substr(kGenerationOffset), &generation) ||
        !DecodeFixed64(input.substr(kRecordCountOffset), &record_count) ||
        !DecodeFixed32(input.substr(kPayloadSizeOffset), &payload_size) ||
        !DecodeFixed32(input.substr(kChecksumOffset), &expected_checksum)) {
        return TableCorruption("fixed header fields cannot be decoded");
    }
    if (header_size != kTableHeaderSize || generation == 0 || record_count == 0) {
        return TableCorruption("header fields are invalid");
    }
    if (payload_size != input.size() - kTableHeaderSize) {
        return TableCorruption("payload size does not match file size");
    }

    const std::string_view payload = input.substr(kTableHeaderSize);
    if (TableChecksum(input.substr(0, kChecksumOffset), payload) !=
        expected_checksum) {
        return TableCorruption("checksum mismatch");
    }

    auto table = std::make_unique<TableData>(generation, options);
    std::string previous_key;
    std::string_view remaining = payload;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        const auto decoded = DecodeWalRecord(remaining, options);
        if (!decoded.status.ok()) {
            return TableCorruption(
                "record " + std::to_string(index) + " is invalid: " +
                decoded.status.ToString()
            );
        }
        if (index != 0 && decoded.record.key <= previous_key) {
            return TableCorruption("records are not strictly ordered by key");
        }
        const auto apply_status = decoded.record.type == ValueType::kValue
                                      ? table->records.Put(
                                            decoded.record.sequence,
                                            decoded.record.key,
                                            decoded.record.value
                                        )
                                      : table->records.Delete(
                                            decoded.record.sequence,
                                            decoded.record.key
                                        );
        if (!apply_status.ok()) {
            return TableCorruption("record cannot be applied");
        }
        previous_key = decoded.record.key;
        table->max_sequence = std::max(
            table->max_sequence,
            decoded.record.sequence
        );
        remaining.remove_prefix(decoded.bytes_consumed);
    }
    if (!remaining.empty()) {
        return TableCorruption("payload has trailing records or bytes");
    }

    *output = std::move(table);
    return Status::Ok();
}

}  // namespace minikv
