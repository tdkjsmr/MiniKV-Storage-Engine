#include "minikv/table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "minikv/coding.hpp"
#include "minikv/wal.hpp"

namespace minikv {
namespace {

constexpr std::array<char, 4> kTableMagic = {'M', 'K', 'S', 'T'};
constexpr std::array<char, 4> kBlockMagic = {'M', 'K', 'D', 'B'};
constexpr std::array<char, 4> kIndexMagic = {'M', 'K', 'I', 'X'};
constexpr std::array<char, 4> kFooterMagic = {'M', 'K', 'S', 'F'};
constexpr std::size_t kIndexEntryFixedSize = 16;
constexpr std::size_t kFooterChecksumOffset = 100;

struct BuiltIndexEntry {
    std::string first_key;
    std::uint64_t block_offset = 0;
    std::uint32_t block_size = 0;
};

Status TableCorruption(std::string message) {
    return Status::Corruption("sstable: " + std::move(message));
}

bool CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* result
) {
    if (result == nullptr ||
        left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool RegionWithin(
    std::uint64_t offset,
    std::uint64_t size,
    std::uint64_t region_offset,
    std::uint64_t region_size
) {
    std::uint64_t end = 0;
    std::uint64_t region_end = 0;
    return CheckedAdd(offset, size, &end) &&
           CheckedAdd(region_offset, region_size, &region_end) &&
           offset >= region_offset && end <= region_end;
}

std::uint32_t ChecksumParts(
    std::string_view prefix,
    std::string_view payload
) {
    std::string bytes;
    bytes.reserve(prefix.size() + payload.size());
    bytes.append(prefix);
    bytes.append(payload);
    return Crc32c(bytes);
}

Status ReadRegion(
    RecoveryFile& file,
    std::uint64_t offset,
    std::uint64_t size,
    std::string* bytes
) {
    if (bytes == nullptr) {
        return Status::InvalidArgument("SSTable read output must not be null");
    }
    if (size > std::numeric_limits<std::size_t>::max()) {
        return TableCorruption("region size exceeds addressable memory");
    }
    bytes->assign(static_cast<std::size_t>(size), '\0');
    const auto status = ReadAllAt(file, offset, bytes);
    if (status.IsIncomplete()) {
        return TableCorruption("region reaches beyond the physical file");
    }
    return status;
}

Status ValidateTableOptions(const Options& options) {
    if (options.max_key_size == 0 ||
        options.max_key_size > std::numeric_limits<std::uint32_t>::max() ||
        options.max_value_size > std::numeric_limits<std::uint32_t>::max() ||
        options.sstable_block_size < kTableBlockHeaderSize + kWalHeaderSize ||
        options.sstable_block_size >
            std::numeric_limits<std::uint32_t>::max() - kTableBlockHeaderSize ||
        !std::isfinite(options.bloom_false_positive_rate) ||
        options.bloom_false_positive_rate <= 0.0 ||
        options.bloom_false_positive_rate >= 1.0) {
        return Status::InvalidArgument("SSTable size options are invalid");
    }
    std::uint64_t maximum_record = kWalHeaderSize;
    if (!CheckedAdd(maximum_record, options.max_key_size, &maximum_record) ||
        !CheckedAdd(maximum_record, options.max_value_size, &maximum_record) ||
        maximum_record > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("configured record sizes exceed format limits");
    }
    return Status::Ok();
}

Status MaximumBlockSize(const Options& options, std::uint64_t* maximum) {
    if (maximum == nullptr) {
        return Status::InvalidArgument("maximum block size output must not be null");
    }
    std::uint64_t record_limit = kWalHeaderSize;
    if (!CheckedAdd(record_limit, options.max_key_size, &record_limit) ||
        !CheckedAdd(record_limit, options.max_value_size, &record_limit)) {
        return Status::InvalidArgument("configured SSTable limits overflow");
    }
    const std::uint64_t payload_limit = std::max(
        static_cast<std::uint64_t>(options.sstable_block_size),
        record_limit
    );
    if (!CheckedAdd(
            static_cast<std::uint64_t>(kTableBlockHeaderSize),
            payload_limit,
            maximum
        )) {
        return Status::InvalidArgument("configured SSTable limits overflow");
    }
    return Status::Ok();
}

Status FinishBlock(
    std::string* data,
    std::vector<BuiltIndexEntry>* index,
    std::string* payload,
    std::uint32_t* record_count,
    std::string* first_key
) {
    if (data == nullptr || index == nullptr || payload == nullptr ||
        record_count == nullptr || first_key == nullptr) {
        return Status::InvalidArgument("SSTable block builder output is null");
    }
    if (*record_count == 0) {
        return Status::Ok();
    }
    const std::uint64_t block_size_64 =
        static_cast<std::uint64_t>(kTableBlockHeaderSize) + payload->size();
    if (block_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("SSTable data block exceeds format limit");
    }
    if (index->size() == std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("SSTable block count exceeds format limit");
    }

    std::string prefix;
    prefix.append(kBlockMagic.data(), kBlockMagic.size());
    PutFixed32(prefix, static_cast<std::uint32_t>(block_size_64));
    PutFixed32(prefix, *record_count);
    PutFixed32(prefix, static_cast<std::uint32_t>(payload->size()));

    BuiltIndexEntry entry;
    entry.first_key = *first_key;
    entry.block_offset = static_cast<std::uint64_t>(kTableHeaderSize) + data->size();
    entry.block_size = static_cast<std::uint32_t>(block_size_64);
    index->push_back(std::move(entry));

    data->append(prefix);
    PutFixed32(*data, ChecksumParts(prefix, *payload));
    data->append(*payload);
    payload->clear();
    first_key->clear();
    *record_count = 0;
    return Status::Ok();
}

}  // namespace

SSTableReader::SSTableReader(
    std::string path,
    Options options,
    std::unique_ptr<RecoveryFile> file
)
    : path_(std::move(path)),
      options_(options),
      file_(std::move(file)) {}

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
    if (generation == 0 || memtable.empty()) {
        return Status::InvalidArgument(
            "SSTable requires a non-zero generation and at least one record"
        );
    }
    const auto options_status = ValidateTableOptions(options);
    if (!options_status.ok()) {
        return options_status;
    }

    const auto records = memtable.Records();
    std::string data;
    std::vector<BuiltIndexEntry> index_entries;
    std::string block_payload;
    std::string first_key;
    std::uint32_t block_records = 0;
    std::uint64_t minimum_sequence = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum_sequence = 0;

    for (const auto& entry : records) {
        WalRecord record;
        record.type = entry.type;
        record.sequence = entry.sequence;
        record.key = entry.key;
        record.value = entry.value;
        std::string encoded;
        auto status = EncodeWalRecord(record, options, &encoded);
        if (!status.ok()) {
            return status;
        }

        const std::uint64_t candidate_size =
            static_cast<std::uint64_t>(kTableBlockHeaderSize) +
            block_payload.size() + encoded.size();
        if (block_records != 0 && candidate_size > options.sstable_block_size) {
            status = FinishBlock(
                &data,
                &index_entries,
                &block_payload,
                &block_records,
                &first_key
            );
            if (!status.ok()) {
                return status;
            }
        }
        if (block_records == 0) {
            first_key = entry.key;
        }
        if (block_records == std::numeric_limits<std::uint32_t>::max() ||
            block_payload.size() >
                std::numeric_limits<std::uint32_t>::max() - encoded.size()) {
            return Status::InvalidArgument("SSTable data block exceeds format limit");
        }
        block_payload.append(encoded);
        ++block_records;
        minimum_sequence = std::min(minimum_sequence, entry.sequence);
        maximum_sequence = std::max(maximum_sequence, entry.sequence);
    }
    auto status = FinishBlock(
        &data,
        &index_entries,
        &block_payload,
        &block_records,
        &first_key
    );
    if (!status.ok()) {
        return status;
    }

    std::string index_payload;
    for (const auto& entry : index_entries) {
        if (entry.first_key.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument("SSTable index key exceeds format limit");
        }
        PutFixed32(index_payload, static_cast<std::uint32_t>(entry.first_key.size()));
        PutFixed64(index_payload, entry.block_offset);
        PutFixed32(index_payload, entry.block_size);
        index_payload.append(entry.first_key);
    }
    const std::uint64_t index_size_64 =
        static_cast<std::uint64_t>(kTableIndexHeaderSize) + index_payload.size();
    if (index_size_64 > std::numeric_limits<std::uint32_t>::max() ||
        index_size_64 > kMaximumTableIndexSize) {
        return Status::InvalidArgument("SSTable sparse index exceeds format limit");
    }
    std::string index;
    index.append(kIndexMagic.data(), kIndexMagic.size());
    PutFixed32(index, static_cast<std::uint32_t>(index_size_64));
    PutFixed32(index, static_cast<std::uint32_t>(index_entries.size()));
    PutFixed32(index, static_cast<std::uint32_t>(index_payload.size()));
    PutFixed32(index, ChecksumParts(index, index_payload));
    index.append(index_payload);

    std::string bloom;
    if (options.bloom_filter_enabled) {
        BloomFilter filter;
        status = BloomFilter::Create(
            records.size(),
            options.bloom_false_positive_rate,
            &filter
        );
        if (!status.ok()) {
            return status;
        }
        for (const auto& record : records) {
            status = filter.Add(record.key);
            if (!status.ok()) {
                return status;
            }
        }
        status = EncodeBloomFilter(filter, &bloom);
        if (!status.ok()) {
            return status;
        }
    }

    std::string header;
    header.append(kTableMagic.data(), kTableMagic.size());
    header.push_back(static_cast<char>(kTableFormatVersion));
    header.push_back('\0');
    PutFixed16(header, static_cast<std::uint16_t>(kTableHeaderSize));
    PutFixed64(header, generation);
    PutFixed64(header, static_cast<std::uint64_t>(records.size()));
    PutFixed32(header, static_cast<std::uint32_t>(index_entries.size()));
    PutFixed32(header, Crc32c(header));

    const std::uint64_t data_offset = kTableHeaderSize;
    const std::uint64_t data_size = data.size();
    std::uint64_t index_offset = 0;
    const std::uint64_t index_size = index.size();
    std::uint64_t bloom_offset = 0;
    const std::uint64_t bloom_size = bloom.size();
    std::uint64_t footer_offset = 0;
    std::uint64_t file_size = 0;
    if (!CheckedAdd(data_offset, data_size, &index_offset) ||
        !CheckedAdd(index_offset, index_size, &bloom_offset) ||
        !CheckedAdd(bloom_offset, bloom_size, &footer_offset) ||
        !CheckedAdd(footer_offset, kTableFooterSize, &file_size) ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        return Status::InvalidArgument("SSTable physical size overflows");
    }

    std::string footer;
    footer.append(kFooterMagic.data(), kFooterMagic.size());
    footer.push_back(static_cast<char>(kTableFormatVersion));
    footer.push_back('\0');
    PutFixed16(footer, static_cast<std::uint16_t>(kTableFooterSize));
    PutFixed64(footer, file_size);
    PutFixed64(footer, generation);
    PutFixed64(footer, static_cast<std::uint64_t>(records.size()));
    PutFixed64(footer, minimum_sequence);
    PutFixed64(footer, maximum_sequence);
    PutFixed64(footer, data_offset);
    PutFixed64(footer, data_size);
    PutFixed64(footer, index_offset);
    PutFixed64(footer, index_size);
    PutFixed64(footer, bloom_offset);
    PutFixed64(footer, bloom_size);
    PutFixed32(footer, static_cast<std::uint32_t>(index_entries.size()));
    PutFixed32(footer, Crc32c(footer));

    destination->reserve(static_cast<std::size_t>(file_size));
    destination->append(header);
    destination->append(data);
    destination->append(index);
    destination->append(bloom);
    destination->append(footer);
    return Status::Ok();
}

Status SSTableReader::Open(
    std::string path,
    Options options,
    std::unique_ptr<SSTableReader>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("SSTable reader output must not be null");
    }
    output->reset();
    if (path.empty()) {
        return Status::InvalidArgument("SSTable path must not be empty");
    }
    const auto options_status = ValidateTableOptions(options);
    if (!options_status.ok()) {
        return options_status;
    }

    std::unique_ptr<RecoveryFile> file;
    auto status = PosixRecoveryFile::OpenReadOnly(path, &file);
    if (!status.ok()) {
        return status;
    }
    std::uint64_t file_size = 0;
    status = file->Size(&file_size);
    if (!status.ok()) {
        return status;
    }
    if (file_size < kTableHeaderSize + kTableIndexHeaderSize + kTableFooterSize) {
        return TableCorruption("file is too small for required regions");
    }

    std::string header;
    status = ReadRegion(*file, 0, kTableHeaderSize, &header);
    if (!status.ok()) {
        return status;
    }
    if (!std::equal(kTableMagic.begin(), kTableMagic.end(), header.begin()) ||
        static_cast<std::uint8_t>(header[4]) != kTableFormatVersion) {
        return TableCorruption("header magic or format version is unsupported");
    }
    std::uint16_t header_size = 0;
    std::uint64_t header_generation = 0;
    std::uint64_t header_records = 0;
    std::uint32_t header_blocks = 0;
    std::uint32_t header_checksum = 0;
    if (!DecodeFixed16(header.substr(6), &header_size) ||
        !DecodeFixed64(header.substr(8), &header_generation) ||
        !DecodeFixed64(header.substr(16), &header_records) ||
        !DecodeFixed32(header.substr(24), &header_blocks) ||
        !DecodeFixed32(header.substr(28), &header_checksum) ||
        header_size != kTableHeaderSize || header_generation == 0 ||
        header_records == 0 || header_blocks == 0 ||
        Crc32c(std::string_view(header).substr(0, 28)) != header_checksum) {
        return TableCorruption("header fields or checksum are invalid");
    }

    std::string footer;
    status = ReadRegion(
        *file,
        file_size - kTableFooterSize,
        kTableFooterSize,
        &footer
    );
    if (!status.ok()) {
        return status;
    }
    if (!std::equal(kFooterMagic.begin(), kFooterMagic.end(), footer.begin()) ||
        static_cast<std::uint8_t>(footer[4]) != kTableFormatVersion) {
        return TableCorruption("Footer magic or format version is unsupported");
    }

    std::uint16_t footer_size = 0;
    std::uint64_t encoded_file_size = 0;
    std::uint64_t generation = 0;
    std::uint64_t record_count = 0;
    std::uint64_t minimum_sequence = 0;
    std::uint64_t maximum_sequence = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t bloom_size = 0;
    std::uint32_t block_count = 0;
    std::uint32_t footer_checksum = 0;
    if (!DecodeFixed16(footer.substr(6), &footer_size) ||
        !DecodeFixed64(footer.substr(8), &encoded_file_size) ||
        !DecodeFixed64(footer.substr(16), &generation) ||
        !DecodeFixed64(footer.substr(24), &record_count) ||
        !DecodeFixed64(footer.substr(32), &minimum_sequence) ||
        !DecodeFixed64(footer.substr(40), &maximum_sequence) ||
        !DecodeFixed64(footer.substr(48), &data_offset) ||
        !DecodeFixed64(footer.substr(56), &data_size) ||
        !DecodeFixed64(footer.substr(64), &index_offset) ||
        !DecodeFixed64(footer.substr(72), &index_size) ||
        !DecodeFixed64(footer.substr(80), &bloom_offset) ||
        !DecodeFixed64(footer.substr(88), &bloom_size) ||
        !DecodeFixed32(footer.substr(96), &block_count) ||
        !DecodeFixed32(footer.substr(100), &footer_checksum)) {
        return TableCorruption("Footer fields cannot be decoded");
    }
    if (footer_size != kTableFooterSize || encoded_file_size != file_size ||
        generation != header_generation || record_count != header_records ||
        block_count != header_blocks || minimum_sequence == 0 ||
        minimum_sequence > maximum_sequence ||
        Crc32c(std::string_view(footer).substr(0, kFooterChecksumOffset)) !=
            footer_checksum) {
        return TableCorruption("Footer fields or checksum are invalid");
    }

    std::uint64_t expected_index_offset = 0;
    std::uint64_t expected_bloom_offset = 0;
    std::uint64_t expected_file_size = 0;
    if (data_offset != kTableHeaderSize || data_size == 0 ||
        index_size < kTableIndexHeaderSize ||
        index_size > kMaximumTableIndexSize ||
        (bloom_size != 0 && bloom_size < kBloomFilterHeaderSize) ||
        bloom_size > kBloomFilterHeaderSize + kMaximumBloomFilterSize ||
        !CheckedAdd(data_offset, data_size, &expected_index_offset) ||
        index_offset != expected_index_offset ||
        !CheckedAdd(index_offset, index_size, &expected_bloom_offset) ||
        bloom_offset != expected_bloom_offset ||
        !CheckedAdd(bloom_offset, bloom_size, &expected_file_size) ||
        !CheckedAdd(expected_file_size, kTableFooterSize, &expected_file_size) ||
        expected_file_size != file_size) {
        return TableCorruption("Footer regions overlap, contain gaps, or escape the file");
    }

    std::uint64_t max_index_entry = kIndexEntryFixedSize;
    if (!CheckedAdd(max_index_entry, options.max_key_size, &max_index_entry)) {
        return Status::InvalidArgument("configured index key limit overflows");
    }
    if (block_count >
            (std::numeric_limits<std::uint64_t>::max() - kTableIndexHeaderSize) /
                max_index_entry ||
        index_size > kTableIndexHeaderSize +
            static_cast<std::uint64_t>(block_count) * max_index_entry) {
        return TableCorruption("sparse index size exceeds configured limits");
    }

    std::string encoded_index;
    status = ReadRegion(*file, index_offset, index_size, &encoded_index);
    if (!status.ok()) {
        return status;
    }
    if (!std::equal(kIndexMagic.begin(), kIndexMagic.end(), encoded_index.begin())) {
        return TableCorruption("sparse index magic does not match");
    }
    std::uint32_t encoded_index_size = 0;
    std::uint32_t index_entries = 0;
    std::uint32_t index_payload_size = 0;
    std::uint32_t index_checksum = 0;
    if (!DecodeFixed32(encoded_index.substr(4), &encoded_index_size) ||
        !DecodeFixed32(encoded_index.substr(8), &index_entries) ||
        !DecodeFixed32(encoded_index.substr(12), &index_payload_size) ||
        !DecodeFixed32(encoded_index.substr(16), &index_checksum) ||
        encoded_index_size != index_size || index_entries != block_count ||
        index_payload_size != index_size - kTableIndexHeaderSize ||
        ChecksumParts(
            std::string_view(encoded_index).substr(0, 16),
            std::string_view(encoded_index).substr(kTableIndexHeaderSize)
        ) != index_checksum) {
        return TableCorruption("sparse index fields or checksum are invalid");
    }

    auto reader = std::unique_ptr<SSTableReader>(
        new SSTableReader(std::move(path), options, std::move(file))
    );
    reader->metadata_.generation = generation;
    reader->metadata_.record_count = record_count;
    reader->metadata_.minimum_sequence = minimum_sequence;
    reader->metadata_.maximum_sequence = maximum_sequence;
    reader->metadata_.file_size = file_size;
    reader->metadata_.data_offset = data_offset;
    reader->metadata_.data_size = data_size;
    reader->metadata_.index_offset = index_offset;
    reader->metadata_.index_size = index_size;
    reader->metadata_.bloom_offset = bloom_offset;
    reader->metadata_.bloom_size = bloom_size;
    reader->metadata_.block_count = block_count;
    reader->index_.reserve(block_count);

    if (bloom_size != 0) {
        std::string encoded_bloom;
        status = ReadRegion(*reader->file_, bloom_offset, bloom_size, &encoded_bloom);
        if (!status.ok()) {
            return status;
        }
        auto filter = std::make_unique<BloomFilter>();
        status = DecodeBloomFilter(encoded_bloom, filter.get());
        if (!status.ok()) {
            return TableCorruption(status.ToString());
        }
        if (filter->key_count() != record_count) {
            return TableCorruption("Bloom key count disagrees with the Footer");
        }
        reader->metadata_.bloom_bit_count = filter->bit_count();
        reader->metadata_.bloom_hash_count = filter->hash_count();
        reader->bloom_filter_ = std::move(filter);
    }

    std::string_view remaining =
        std::string_view(encoded_index).substr(kTableIndexHeaderSize);
    std::uint64_t expected_block_offset = data_offset;
    std::uint64_t maximum_block_size = 0;
    status = MaximumBlockSize(options, &maximum_block_size);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t entry_number = 0; entry_number < block_count; ++entry_number) {
        std::uint32_t key_size = 0;
        std::uint64_t block_offset = 0;
        std::uint32_t block_size = 0;
        if (remaining.size() < kIndexEntryFixedSize ||
            !DecodeFixed32(remaining, &key_size) ||
            !DecodeFixed64(remaining.substr(4), &block_offset) ||
            !DecodeFixed32(remaining.substr(12), &block_size)) {
            return TableCorruption("sparse index entry header is incomplete");
        }
        remaining.remove_prefix(kIndexEntryFixedSize);
        if (key_size == 0 || key_size > options.max_key_size ||
            key_size > remaining.size() ||
            block_size < kTableBlockHeaderSize ||
            block_size > maximum_block_size ||
            block_offset != expected_block_offset ||
            !RegionWithin(block_offset, block_size, data_offset, data_size)) {
            return TableCorruption("sparse index entry is out of bounds");
        }
        IndexEntry entry;
        entry.first_key.assign(remaining.substr(0, key_size));
        entry.block_offset = block_offset;
        entry.block_size = block_size;
        remaining.remove_prefix(key_size);
        if (!reader->index_.empty() &&
            entry.first_key <= reader->index_.back().first_key) {
            return TableCorruption("sparse index keys are not strictly ordered");
        }
        if (!CheckedAdd(expected_block_offset, block_size, &expected_block_offset)) {
            return TableCorruption("data block offset overflows");
        }
        reader->index_.push_back(std::move(entry));
    }
    if (!remaining.empty() || expected_block_offset != index_offset) {
        return TableCorruption("sparse index has trailing bytes or misses data");
    }

    std::uint64_t verified_records = 0;
    std::uint64_t verified_minimum_sequence =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t verified_maximum_sequence = 0;
    std::string previous_key;
    for (std::size_t block = 0; block < reader->index_.size(); ++block) {
        std::vector<MemTableRecord> block_records_vector;
        status = reader->ReadBlock(block, &block_records_vector, nullptr);
        if (!status.ok()) {
            return status;
        }
        if (block_records_vector.empty() ||
            block_records_vector.front().key != reader->index_[block].first_key ||
            (!previous_key.empty() &&
             block_records_vector.front().key <= previous_key)) {
            return TableCorruption("data blocks disagree with the sparse index");
        }
        for (const auto& record : block_records_vector) {
            if (reader->bloom_filter_ != nullptr &&
                !reader->bloom_filter_->MayContain(record.key)) {
                return TableCorruption(
                    "Bloom filter has a false negative for a stored key"
                );
            }
            verified_minimum_sequence = std::min(
                verified_minimum_sequence,
                record.sequence
            );
            verified_maximum_sequence = std::max(
                verified_maximum_sequence,
                record.sequence
            );
        }
        if (!CheckedAdd(
                verified_records,
                block_records_vector.size(),
                &verified_records
            )) {
            return TableCorruption("verified record count overflows");
        }
        if (block == 0) {
            reader->metadata_.minimum_key = block_records_vector.front().key;
        }
        previous_key = block_records_vector.back().key;
    }
    reader->metadata_.maximum_key = previous_key;
    if (verified_records != record_count ||
        verified_minimum_sequence != minimum_sequence ||
        verified_maximum_sequence != maximum_sequence) {
        return TableCorruption("Footer record or sequence summary is incorrect");
    }

    *output = std::move(reader);
    return Status::Ok();
}

Status SSTableReader::ReadBlock(
    std::size_t index,
    std::vector<MemTableRecord>* records,
    SSTableReadStats* stats
) const {
    if (records == nullptr || index >= index_.size()) {
        return Status::InvalidArgument("SSTable block read arguments are invalid");
    }
    records->clear();
    const auto& entry = index_[index];
    std::string block;
    auto status = ReadRegion(*file_, entry.block_offset, entry.block_size, &block);
    if (stats != nullptr) {
        ++stats->data_blocks_read;
        stats->bytes_read += static_cast<std::uint64_t>(block.size());
    }
    if (!status.ok()) {
        return status;
    }
    if (!std::equal(kBlockMagic.begin(), kBlockMagic.end(), block.begin())) {
        return TableCorruption("data block magic does not match");
    }
    std::uint32_t block_size = 0;
    std::uint32_t record_count = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t checksum = 0;
    if (!DecodeFixed32(block.substr(4), &block_size) ||
        !DecodeFixed32(block.substr(8), &record_count) ||
        !DecodeFixed32(block.substr(12), &payload_size) ||
        !DecodeFixed32(block.substr(16), &checksum) ||
        block_size != entry.block_size || record_count == 0 ||
        payload_size != block.size() - kTableBlockHeaderSize ||
        record_count > payload_size / kWalHeaderSize ||
        ChecksumParts(
            std::string_view(block).substr(0, 16),
            std::string_view(block).substr(kTableBlockHeaderSize)
        ) != checksum) {
        return TableCorruption("data block fields or checksum are invalid");
    }

    records->reserve(record_count);
    std::string_view remaining =
        std::string_view(block).substr(kTableBlockHeaderSize);
    std::string previous_key;
    for (std::uint32_t record_number = 0;
         record_number < record_count;
         ++record_number) {
        const auto decoded = DecodeWalRecord(remaining, options_);
        if (!decoded.status.ok()) {
            return TableCorruption(
                "data record " + std::to_string(record_number) +
                " is invalid: " + decoded.status.ToString()
            );
        }
        if (record_number != 0 && decoded.record.key <= previous_key) {
            return TableCorruption("data block keys are not strictly ordered");
        }
        previous_key = decoded.record.key;
        records->push_back({
            decoded.record.key,
            decoded.record.sequence,
            decoded.record.type,
            decoded.record.value,
        });
        remaining.remove_prefix(decoded.bytes_consumed);
    }
    if (!remaining.empty()) {
        return TableCorruption("data block has trailing records or bytes");
    }
    return Status::Ok();
}

LookupResult SSTableReader::Get(
    std::string_view key,
    SSTableReadStats* stats
) const {
    if (stats != nullptr) {
        *stats = {};
        stats->tables_considered = 1;
    }
    if (key.empty()) {
        return {
            Status::InvalidArgument("key must not be empty"),
            0,
            ValueType::kValue,
            {},
        };
    }
    if (key.size() > options_.max_key_size) {
        return {
            Status::InvalidArgument("key exceeds configured maximum size"),
            0,
            ValueType::kValue,
            {},
        };
    }
    if (key < metadata_.minimum_key || key > metadata_.maximum_key) {
        if (stats != nullptr) {
            ++stats->range_rejections;
        }
        return {Status::NotFound("key is outside table range"), 0,
                ValueType::kValue, {}};
    }
    const bool bloom_checked =
        options_.bloom_filter_enabled && bloom_filter_ != nullptr;
    if (bloom_checked) {
        if (stats != nullptr) {
            ++stats->bloom_filter_checks;
        }
        if (!bloom_filter_->MayContain(key)) {
            if (stats != nullptr) {
                ++stats->bloom_filter_rejections;
            }
            return {Status::NotFound("Bloom filter excludes key"), 0,
                    ValueType::kValue, {}};
        }
    }

    const auto upper = std::upper_bound(
        index_.begin(),
        index_.end(),
        key,
        [](std::string_view target, const IndexEntry& entry) {
            return target < entry.first_key;
        }
    );
    if (upper == index_.begin()) {
        return {Status::NotFound("key precedes sparse index"), 0,
                ValueType::kValue, {}};
    }
    const std::size_t block_index = static_cast<std::size_t>(
        std::distance(index_.begin(), std::prev(upper))
    );
    std::vector<MemTableRecord> records;
    const auto status = ReadBlock(block_index, &records, stats);
    if (!status.ok()) {
        return {status, 0, ValueType::kValue, {}};
    }
    const auto found = std::lower_bound(
        records.begin(),
        records.end(),
        key,
        [](const MemTableRecord& record, std::string_view target) {
            return record.key < target;
        }
    );
    if (found == records.end() || found->key != key) {
        if (stats != nullptr && bloom_checked) {
            ++stats->bloom_false_positives;
        }
        return {Status::NotFound("key does not exist in candidate block"), 0,
                ValueType::kValue, {}};
    }
    return {Status::Ok(), found->sequence, found->type, found->value};
}

Status SSTableReader::ReadRecords(
    std::size_t limit,
    std::vector<MemTableRecord>* records
) const {
    if (records == nullptr) {
        return Status::InvalidArgument("SSTable record output must not be null");
    }
    records->clear();
    if (limit == 0) {
        return Status::Ok();
    }
    records->reserve(std::min<std::size_t>(
        limit,
        static_cast<std::size_t>(metadata_.record_count)
    ));
    for (std::size_t block = 0; block < index_.size(); ++block) {
        std::vector<MemTableRecord> block_records;
        const auto status = ReadBlock(block, &block_records, nullptr);
        if (!status.ok()) {
            records->clear();
            return status;
        }
        for (auto& record : block_records) {
            records->push_back(std::move(record));
            if (records->size() == limit) {
                return Status::Ok();
            }
        }
    }
    return Status::Ok();
}

SSTableIterator::SSTableIterator(
    const SSTableReader* reader,
    std::string begin,
    std::optional<std::string> end
)
    : reader_(reader), begin_(std::move(begin)), end_(std::move(end)) {
    statistics_.tables_considered = 1;
}

const MemTableRecord* SSTableIterator::record() const noexcept {
    if (!valid_ || record_ >= records_.size()) {
        return nullptr;
    }
    return &records_[record_];
}

Status SSTableIterator::LoadBlock(std::size_t block) {
    records_.clear();
    record_ = 0;
    if (reader_ == nullptr || block >= reader_->index_.size()) {
        valid_ = false;
        return Status::Ok();
    }
    block_ = block;
    const auto status = reader_->ReadBlock(block_, &records_, &statistics_);
    if (!status.ok()) {
        valid_ = false;
        records_.clear();
        return status;
    }
    valid_ = !records_.empty();
    return Status::Ok();
}

void SSTableIterator::EnforceUpperBound() {
    const auto* current = record();
    if (current != nullptr && end_.has_value() && current->key >= *end_) {
        valid_ = false;
        records_.clear();
        record_ = 0;
    }
}

Status SSTableIterator::Initialize() {
    if (reader_ == nullptr || reader_->index_.empty()) {
        return Status::InvalidArgument("SSTable iterator has no reader");
    }
    if (reader_->metadata_.maximum_key < begin_ ||
        (end_.has_value() && reader_->metadata_.minimum_key >= *end_) ||
        (end_.has_value() && begin_ == *end_)) {
        valid_ = false;
        return Status::Ok();
    }

    std::size_t first_block = 0;
    if (!begin_.empty()) {
        const auto upper = std::upper_bound(
            reader_->index_.begin(),
            reader_->index_.end(),
            begin_,
            [](std::string_view target, const SSTableReader::IndexEntry& entry) {
                return target < entry.first_key;
            }
        );
        if (upper != reader_->index_.begin()) {
            first_block = static_cast<std::size_t>(
                std::distance(reader_->index_.begin(), std::prev(upper))
            );
        }
    }

    auto status = LoadBlock(first_block);
    if (!status.ok()) {
        return status;
    }
    record_ = static_cast<std::size_t>(std::distance(
        records_.begin(),
        std::lower_bound(
            records_.begin(),
            records_.end(),
            begin_,
            [](const MemTableRecord& record, std::string_view target) {
                return record.key < target;
            }
        )
    ));
    while (record_ >= records_.size()) {
        status = LoadBlock(block_ + 1U);
        if (!status.ok() || !valid_) {
            return status;
        }
    }
    valid_ = true;
    EnforceUpperBound();
    return Status::Ok();
}

Status SSTableIterator::Next() {
    if (!valid_) {
        return Status::Ok();
    }
    ++record_;
    while (record_ >= records_.size()) {
        const auto status = LoadBlock(block_ + 1U);
        if (!status.ok() || !valid_) {
            return status;
        }
    }
    EnforceUpperBound();
    return Status::Ok();
}

Status SSTableReader::NewIterator(
    std::string_view begin,
    std::optional<std::string_view> end,
    std::unique_ptr<SSTableIterator>* output
) const {
    if (output == nullptr) {
        return Status::InvalidArgument("SSTable iterator output must not be null");
    }
    output->reset();
    if (begin.size() > options_.max_key_size ||
        (end.has_value() && end->size() > options_.max_key_size)) {
        return Status::InvalidArgument("SSTable iterator bound is too large");
    }
    if (end.has_value() && begin > *end) {
        return Status::InvalidArgument("SSTable iterator range is reversed");
    }
    std::optional<std::string> owned_end;
    if (end.has_value()) {
        owned_end = std::string(*end);
    }
    auto iterator = std::unique_ptr<SSTableIterator>(new SSTableIterator(
        this,
        std::string(begin),
        std::move(owned_end)
    ));
    const auto status = iterator->Initialize();
    if (!status.ok()) {
        return status;
    }
    *output = std::move(iterator);
    return Status::Ok();
}

}  // namespace minikv
