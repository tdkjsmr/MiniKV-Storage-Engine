#include "minikv/database.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minikv/coding.hpp"

namespace minikv {
namespace {

constexpr std::array<char, 4> kScanTokenMagic = {'M', 'K', 'C', 'T'};
constexpr std::uint8_t kScanTokenFormatVersion = 1;
constexpr std::size_t kScanTokenHeaderSize = 44;
constexpr std::size_t kScanTokenChecksumOffset = 40;
constexpr std::uint8_t kScanTokenHasEnd = 0x01;

struct ScanBounds {
    std::string begin;
    std::optional<std::string> end;
};

struct DecodedScanToken {
    std::uint64_t version_id = 0;
    std::uint64_t last_sequence = 0;
    std::string last_key;
};

ScanResult ScanError(Status status) {
    ScanResult result;
    result.status = std::move(status);
    return result;
}

Status InvalidToken(std::string message) {
    return Status::InvalidArgument("scan continuation token: " + std::move(message));
}

bool CheckedAddSize(
    std::size_t left,
    std::size_t right,
    std::size_t* output
) {
    if (output == nullptr ||
        left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    *output = left + right;
    return true;
}

Status EncodeScanToken(
    const ScanBounds& bounds,
    std::uint64_t version_id,
    std::uint64_t last_sequence,
    std::string_view last_key,
    std::string* output
) {
    if (output == nullptr || last_key.empty() ||
        bounds.begin.size() > std::numeric_limits<std::uint32_t>::max() ||
        (bounds.end.has_value() &&
         bounds.end->size() > std::numeric_limits<std::uint32_t>::max()) ||
        last_key.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("scan token fields are invalid");
    }
    std::size_t total_size = kScanTokenHeaderSize;
    if (!CheckedAddSize(total_size, bounds.begin.size(), &total_size) ||
        !CheckedAddSize(
            total_size,
            bounds.end.has_value() ? bounds.end->size() : 0,
            &total_size
        ) ||
        !CheckedAddSize(total_size, last_key.size(), &total_size) ||
        total_size > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("scan token is too large");
    }

    std::string payload;
    payload.reserve(total_size - kScanTokenHeaderSize);
    payload.append(bounds.begin);
    if (bounds.end.has_value()) {
        payload.append(*bounds.end);
    }
    payload.append(last_key);

    std::string header;
    header.reserve(kScanTokenHeaderSize);
    header.append(kScanTokenMagic.data(), kScanTokenMagic.size());
    header.push_back(static_cast<char>(kScanTokenFormatVersion));
    header.push_back(static_cast<char>(
        bounds.end.has_value() ? kScanTokenHasEnd : 0
    ));
    PutFixed16(header, 0);
    PutFixed32(header, static_cast<std::uint32_t>(total_size));
    PutFixed64(header, version_id);
    PutFixed64(header, last_sequence);
    PutFixed32(header, static_cast<std::uint32_t>(bounds.begin.size()));
    PutFixed32(
        header,
        static_cast<std::uint32_t>(
            bounds.end.has_value() ? bounds.end->size() : 0
        )
    );
    PutFixed32(header, static_cast<std::uint32_t>(last_key.size()));
    std::string checksum_input = header;
    checksum_input.append(payload);
    PutFixed32(header, Crc32c(checksum_input));
    header.append(payload);
    *output = std::move(header);
    return Status::Ok();
}

Status DecodeScanToken(
    std::string_view encoded,
    const ScanBounds& expected_bounds,
    const Options& options,
    std::uint64_t expected_version_id,
    std::uint64_t expected_last_sequence,
    DecodedScanToken* output
) {
    if (output == nullptr || encoded.size() < kScanTokenHeaderSize) {
        return InvalidToken("header is incomplete");
    }
    if (!std::equal(kScanTokenMagic.begin(), kScanTokenMagic.end(), encoded.begin())) {
        return InvalidToken("magic is invalid");
    }
    const auto format_version = static_cast<std::uint8_t>(encoded[4]);
    const auto flags = static_cast<std::uint8_t>(encoded[5]);
    std::uint16_t reserved = 0;
    std::uint32_t total_size = 0;
    std::uint64_t version_id = 0;
    std::uint64_t last_sequence = 0;
    std::uint32_t begin_size = 0;
    std::uint32_t end_size = 0;
    std::uint32_t last_key_size = 0;
    std::uint32_t checksum = 0;
    if (!DecodeFixed16(encoded.substr(6), &reserved) ||
        !DecodeFixed32(encoded.substr(8), &total_size) ||
        !DecodeFixed64(encoded.substr(12), &version_id) ||
        !DecodeFixed64(encoded.substr(20), &last_sequence) ||
        !DecodeFixed32(encoded.substr(28), &begin_size) ||
        !DecodeFixed32(encoded.substr(32), &end_size) ||
        !DecodeFixed32(encoded.substr(36), &last_key_size) ||
        !DecodeFixed32(encoded.substr(kScanTokenChecksumOffset), &checksum)) {
        return InvalidToken("header fields cannot be decoded");
    }
    const bool has_end = (flags & kScanTokenHasEnd) != 0;
    if (format_version != kScanTokenFormatVersion ||
        (flags & static_cast<std::uint8_t>(~kScanTokenHasEnd)) != 0 ||
        reserved != 0 || total_size != encoded.size() ||
        has_end != expected_bounds.end.has_value() ||
        (!has_end && end_size != 0) || begin_size > options.max_key_size ||
        end_size > options.max_key_size || last_key_size == 0 ||
        last_key_size > options.max_key_size) {
        return InvalidToken("header fields are invalid");
    }
    std::size_t payload_size = begin_size;
    if (!CheckedAddSize(payload_size, end_size, &payload_size) ||
        !CheckedAddSize(payload_size, last_key_size, &payload_size) ||
        payload_size != encoded.size() - kScanTokenHeaderSize) {
        return InvalidToken("payload lengths are invalid");
    }
    std::string checksum_input(encoded.substr(0, kScanTokenChecksumOffset));
    checksum_input.append(encoded.substr(kScanTokenHeaderSize));
    if (Crc32c(checksum_input) != checksum) {
        return InvalidToken("checksum mismatch");
    }

    std::string_view payload = encoded.substr(kScanTokenHeaderSize);
    const std::string_view begin = payload.substr(0, begin_size);
    payload.remove_prefix(begin_size);
    const std::string_view end = payload.substr(0, end_size);
    payload.remove_prefix(end_size);
    const std::string_view last_key = payload.substr(0, last_key_size);
    if (begin != expected_bounds.begin ||
        (has_end && end != *expected_bounds.end)) {
        return InvalidToken("does not match the requested range");
    }
    if (version_id != expected_version_id ||
        last_sequence != expected_last_sequence) {
        return InvalidToken("is stale because the database changed");
    }
    if (last_key < expected_bounds.begin ||
        (expected_bounds.end.has_value() && last_key >= *expected_bounds.end)) {
        return InvalidToken("resume key is outside the requested range");
    }
    output->version_id = version_id;
    output->last_sequence = last_sequence;
    output->last_key.assign(last_key);
    return Status::Ok();
}

std::optional<std::string> PrefixUpperBound(std::string_view prefix) {
    if (prefix.empty()) {
        return std::nullopt;
    }
    std::string upper(prefix);
    for (std::size_t offset = upper.size(); offset > 0; --offset) {
        const auto byte = static_cast<std::uint8_t>(upper[offset - 1U]);
        if (byte != std::numeric_limits<std::uint8_t>::max()) {
            upper[offset - 1U] = static_cast<char>(byte + 1U);
            upper.resize(offset);
            return upper;
        }
    }
    return std::nullopt;
}

struct ScanSource {
    std::vector<MemTableRecord> memory_records;
    std::size_t memory_position = 0;
    std::optional<std::string> memory_end;
    std::unique_ptr<SSTableIterator> table_iterator;

    [[nodiscard]] const MemTableRecord* current() const noexcept {
        if (table_iterator != nullptr) {
            return table_iterator->record();
        }
        if (memory_position >= memory_records.size()) {
            return nullptr;
        }
        const auto& record = memory_records[memory_position];
        if (memory_end.has_value() && record.key >= *memory_end) {
            return nullptr;
        }
        return &record;
    }

    Status Next() {
        if (table_iterator != nullptr) {
            return table_iterator->Next();
        }
        if (memory_position < memory_records.size()) {
            ++memory_position;
        }
        return Status::Ok();
    }
};

struct SourceLater {
    const std::vector<ScanSource>* sources = nullptr;

    bool operator()(std::size_t left, std::size_t right) const {
        const auto* left_record = (*sources)[left].current();
        const auto* right_record = (*sources)[right].current();
        if (left_record->key != right_record->key) {
            return left_record->key > right_record->key;
        }
        return left_record->sequence < right_record->sequence;
    }
};

void AddMemorySource(
    const MemTable& memtable,
    const ScanBounds& bounds,
    std::string_view resume_after,
    std::vector<ScanSource>* sources,
    ScanStatistics* statistics
) {
    if (memtable.empty()) {
        return;
    }
    ScanSource source;
    source.memory_records = memtable.Records();
    source.memory_end = bounds.end;
    const std::string_view lower = resume_after.empty()
        ? std::string_view(bounds.begin)
        : resume_after;
    source.memory_position = static_cast<std::size_t>(std::distance(
        source.memory_records.begin(),
        std::lower_bound(
            source.memory_records.begin(),
            source.memory_records.end(),
            lower,
            [](const MemTableRecord& record, std::string_view key) {
                return record.key < key;
            }
        )
    ));
    if (!resume_after.empty() && source.current() != nullptr &&
        source.current()->key == resume_after) {
        ++source.memory_position;
    }
    ++statistics->sources_considered;
    sources->push_back(std::move(source));
}

void AccumulateIteratorStatistics(
    const std::vector<ScanSource>& sources,
    ScanStatistics* statistics
) {
    for (const auto& source : sources) {
        if (source.table_iterator == nullptr) {
            continue;
        }
        const auto& table = source.table_iterator->statistics();
        statistics->tables_considered += table.tables_considered;
        statistics->data_blocks_read += table.data_blocks_read;
        statistics->bytes_read += table.bytes_read;
    }
}

}  // namespace

ScanResult Database::Scan(const ScanOptions& scan_options) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    if (!IsRunningLocked()) {
        return ScanError(Status::Closed("database is not running"));
    }
    if (scan_options.limit == 0 ||
        scan_options.limit > options_.maximum_scan_entries) {
        return ScanError(Status::InvalidArgument(
            "scan limit must be between one and the configured maximum"
        ));
    }
    if (scan_options.begin.size() > options_.max_key_size ||
        (scan_options.end.has_value() &&
         scan_options.end->size() > options_.max_key_size)) {
        return ScanError(Status::InvalidArgument(
            "scan bound exceeds configured maximum key size"
        ));
    }
    if (scan_options.end.has_value() &&
        scan_options.begin > *scan_options.end) {
        return ScanError(Status::InvalidArgument("scan range is reversed"));
    }

    const ScanBounds bounds{scan_options.begin, scan_options.end};
    std::string resume_after;
    if (!scan_options.continuation_token.empty()) {
        DecodedScanToken token;
        const auto token_status = DecodeScanToken(
            scan_options.continuation_token,
            bounds,
            options_,
            version_.id(),
            last_sequence_,
            &token
        );
        if (!token_status.ok()) {
            return ScanError(token_status);
        }
        resume_after = std::move(token.last_key);
    }

    ScanResult result;
    if (bounds.end.has_value() && bounds.begin == *bounds.end) {
        return result;
    }

    std::vector<ScanSource> sources;
    sources.reserve(tables_.size() + 2U);
    AddMemorySource(
        mutable_, bounds, resume_after, &sources, &result.statistics
    );
    if (immutable_ != nullptr) {
        AddMemorySource(
            immutable_->memtable,
            bounds,
            resume_after,
            &sources,
            &result.statistics
        );
    }

    const std::string_view effective_begin = resume_after.empty()
        ? std::string_view(bounds.begin)
        : std::string_view(resume_after);
    for (const auto& table : tables_) {
        if (table == nullptr || table->metadata().maximum_key < effective_begin ||
            (bounds.end.has_value() &&
             table->metadata().minimum_key >= *bounds.end)) {
            continue;
        }
        ScanSource source;
        std::optional<std::string_view> end;
        if (bounds.end.has_value()) {
            end = *bounds.end;
        }
        const auto iterator_status = table->NewIterator(
            effective_begin,
            end,
            &source.table_iterator
        );
        if (!iterator_status.ok()) {
            return ScanError(iterator_status);
        }
        if (!resume_after.empty() && source.current() != nullptr &&
            source.current()->key == resume_after) {
            const auto next_status = source.Next();
            if (!next_status.ok()) {
                return ScanError(next_status);
            }
        }
        ++result.statistics.sources_considered;
        sources.push_back(std::move(source));
    }

    SourceLater comparison{&sources};
    std::priority_queue<std::size_t, std::vector<std::size_t>, SourceLater> heap(
        comparison
    );
    for (std::size_t source = 0; source < sources.size(); ++source) {
        if (sources[source].current() != nullptr) {
            heap.push(source);
        }
    }

    result.entries.reserve(scan_options.limit + 1U);
    while (!heap.empty() && result.entries.size() <= scan_options.limit) {
        const auto* first = sources[heap.top()].current();
        if (first == nullptr) {
            return ScanError(Status::Corruption("scan heap contains an empty source"));
        }
        const std::string key = first->key;
        const MemTableRecord* winner = nullptr;
        std::vector<std::size_t> same_key;
        while (!heap.empty()) {
            const std::size_t source = heap.top();
            const auto* record = sources[source].current();
            if (record == nullptr || record->key != key) {
                break;
            }
            heap.pop();
            same_key.push_back(source);
            if (winner == nullptr || record->sequence > winner->sequence) {
                winner = record;
            } else if (record->sequence == winner->sequence) {
                return ScanError(Status::Corruption(
                    "scan found one Sequence number in multiple sources"
                ));
            }
        }
        if (winner == nullptr) {
            return ScanError(Status::Corruption("scan merge lost a user key"));
        }
        const MemTableRecord visible = *winner;
        result.statistics.records_examined +=
            static_cast<std::uint64_t>(same_key.size());
        result.statistics.obsolete_versions_skipped +=
            static_cast<std::uint64_t>(same_key.size() - 1U);
        for (const auto source : same_key) {
            const auto next_status = sources[source].Next();
            if (!next_status.ok()) {
                return ScanError(next_status);
            }
            if (sources[source].current() != nullptr) {
                heap.push(source);
            }
        }
        if (visible.type == ValueType::kDeletion) {
            ++result.statistics.tombstones_skipped;
            continue;
        }
        result.entries.push_back({visible.key, visible.value});
    }

    AccumulateIteratorStatistics(sources, &result.statistics);
    if (result.entries.size() > scan_options.limit) {
        result.entries.resize(scan_options.limit);
        result.truncated = true;
        const auto token_status = EncodeScanToken(
            bounds,
            version_.id(),
            last_sequence_,
            result.entries.back().key,
            &result.continuation_token
        );
        if (!token_status.ok()) {
            return ScanError(token_status);
        }
    }
    return result;
}

ScanResult Database::ScanPrefix(
    std::string_view prefix,
    std::size_t limit,
    std::string_view continuation_token
) const {
    ScanOptions scan_options;
    scan_options.begin.assign(prefix);
    scan_options.end = PrefixUpperBound(prefix);
    scan_options.limit = limit;
    scan_options.continuation_token.assign(continuation_token);
    return Scan(scan_options);
}

ScanResult Database::LoadAll(
    std::size_t limit,
    std::string_view continuation_token
) const {
    return ScanPrefix({}, limit, continuation_token);
}

}  // namespace minikv
