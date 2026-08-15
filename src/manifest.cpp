#include "minikv/manifest.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include "minikv/coding.hpp"
#include "minikv/file.hpp"
#include "minikv/flush.hpp"

namespace minikv {
namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 4> kManifestMagic = {'M', 'K', 'M', 'F'};
constexpr std::size_t kManifestChecksumOffset = 56;

Status ManifestCorruption(std::string message) {
    return Status::Corruption("manifest: " + std::move(message));
}

std::string JoinPath(const std::string& directory, std::string_view name) {
    return (fs::path(directory) / name).string();
}

bool CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output
) {
    if (output == nullptr ||
        left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *output = left + right;
    return true;
}

Status ValidateVersion(const Version& version, const Options& options) {
    if (version.id() == 0 || version.next_file_number() == 0 ||
        options.max_key_size == 0) {
        return ManifestCorruption("global Version fields are invalid");
    }

    std::set<std::uint64_t> file_numbers;
    std::vector<const VersionTable*> level_one_tables;
    std::uint64_t maximum_sequence = 0;
    for (const auto& table : version.tables()) {
        const auto& metadata = table.metadata;
        if (table.level > 1 || metadata.generation == 0 ||
            metadata.generation >= version.next_file_number() ||
            metadata.file_size == 0 || metadata.record_count == 0 ||
            metadata.minimum_sequence == 0 ||
            metadata.minimum_sequence > metadata.maximum_sequence ||
            metadata.minimum_key.size() > options.max_key_size ||
            metadata.maximum_key.size() > options.max_key_size ||
            metadata.minimum_key > metadata.maximum_key ||
            !file_numbers.insert(metadata.generation).second) {
            return ManifestCorruption("table metadata is invalid");
        }
        if (table.level == 1) {
            level_one_tables.push_back(&table);
        }
        maximum_sequence = std::max(maximum_sequence, metadata.maximum_sequence);
    }
    std::sort(
        level_one_tables.begin(),
        level_one_tables.end(),
        [](const VersionTable* left, const VersionTable* right) {
            return left->metadata.minimum_key < right->metadata.minimum_key;
        }
    );
    for (std::size_t index = 1; index < level_one_tables.size(); ++index) {
        if (level_one_tables[index - 1]->metadata.maximum_key >=
            level_one_tables[index]->metadata.minimum_key) {
            return ManifestCorruption("level-one table ranges overlap");
        }
    }
    if (version.last_sequence() < maximum_sequence) {
        return ManifestCorruption("sequence frontier precedes a live table");
    }
    return Status::Ok();
}

}  // namespace

Version Version::NewDatabase() {
    Version version;
    version.id_ = 1;
    version.next_file_number_ = 1;
    version.last_sequence_ = 0;
    return version;
}

Status Version::Apply(
    const VersionEdit& edit,
    const Options& options,
    Version* output
) const {
    if (output == nullptr) {
        return Status::InvalidArgument("Version output must not be null");
    }
    const auto current_status = ValidateVersion(*this, options);
    if (!current_status.ok()) {
        return current_status;
    }
    if (id_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("Version id space is exhausted");
    }

    Version candidate = *this;
    ++candidate.id_;
    std::set<std::uint64_t> deletions;
    for (const auto file_number : edit.deleted_file_numbers) {
        if (file_number == 0 || !deletions.insert(file_number).second) {
            return Status::InvalidArgument("VersionEdit deletion is invalid");
        }
    }
    for (const auto file_number : deletions) {
        const bool existed = std::any_of(
            tables_.begin(),
            tables_.end(),
            [file_number](const VersionTable& table) {
                return table.metadata.generation == file_number;
            }
        );
        if (!existed) {
            return Status::InvalidArgument("VersionEdit deletes a non-live file");
        }
    }
    candidate.tables_.erase(
        std::remove_if(
            candidate.tables_.begin(),
            candidate.tables_.end(),
            [&deletions](const VersionTable& table) {
                return deletions.find(table.metadata.generation) != deletions.end();
            }
        ),
        candidate.tables_.end()
    );

    for (const auto& added : edit.added_tables) {
        if (added.metadata.generation < next_file_number_) {
            return Status::InvalidArgument(
                "VersionEdit reuses an allocated file number"
            );
        }
        const auto duplicate = std::find_if(
            candidate.tables_.begin(),
            candidate.tables_.end(),
            [&added](const VersionTable& table) {
                return table.metadata.generation == added.metadata.generation;
            }
        );
        if (duplicate != candidate.tables_.end() ||
            deletions.find(added.metadata.generation) != deletions.end()) {
            return Status::InvalidArgument("VersionEdit adds a duplicate file");
        }
        candidate.tables_.push_back(added);
    }
    if (edit.next_file_number.has_value()) {
        candidate.next_file_number_ = *edit.next_file_number;
    }
    if (edit.last_sequence.has_value()) {
        candidate.last_sequence_ = *edit.last_sequence;
    }
    if (candidate.next_file_number_ < next_file_number_ ||
        candidate.last_sequence_ < last_sequence_) {
        return Status::InvalidArgument("VersionEdit regresses a monotonic frontier");
    }
    std::sort(
        candidate.tables_.begin(),
        candidate.tables_.end(),
        [](const VersionTable& left, const VersionTable& right) {
            return left.metadata.generation < right.metadata.generation;
        }
    );
    const auto status = ValidateVersion(candidate, options);
    if (!status.ok()) {
        return Status::InvalidArgument(status.message());
    }
    *output = std::move(candidate);
    return Status::Ok();
}

Status EncodeManifest(
    const Version& version,
    const Options& options,
    std::string* destination
) {
    if (destination == nullptr) {
        return Status::InvalidArgument("Manifest destination must not be null");
    }
    destination->clear();
    const auto validation = ValidateVersion(version, options);
    if (!validation.ok()) {
        return validation;
    }
    if (version.tables().size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("Manifest has too many tables");
    }

    std::string payload;
    for (const auto& table : version.tables()) {
        const auto& metadata = table.metadata;
        std::uint64_t record_size = kManifestTableFixedSize;
        if (!CheckedAdd(record_size, metadata.minimum_key.size(), &record_size) ||
            !CheckedAdd(record_size, metadata.maximum_key.size(), &record_size) ||
            record_size > std::numeric_limits<std::uint32_t>::max() ||
            metadata.minimum_key.size() > std::numeric_limits<std::uint32_t>::max() ||
            metadata.maximum_key.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument("Manifest table record is too large");
        }
        PutFixed32(payload, static_cast<std::uint32_t>(record_size));
        PutFixed32(payload, table.level);
        PutFixed64(payload, metadata.generation);
        PutFixed64(payload, metadata.file_size);
        PutFixed64(payload, metadata.record_count);
        PutFixed64(payload, metadata.minimum_sequence);
        PutFixed64(payload, metadata.maximum_sequence);
        PutFixed32(payload, static_cast<std::uint32_t>(metadata.minimum_key.size()));
        PutFixed32(payload, static_cast<std::uint32_t>(metadata.maximum_key.size()));
        payload.append(metadata.minimum_key);
        payload.append(metadata.maximum_key);
        if (payload.size() > kMaximumManifestSize - kManifestHeaderSize) {
            return Status::InvalidArgument("Manifest exceeds the format size limit");
        }
    }
    const std::uint64_t file_size =
        static_cast<std::uint64_t>(kManifestHeaderSize) + payload.size();
    if (file_size > kMaximumManifestSize ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("Manifest exceeds the format size limit");
    }

    std::string header;
    header.append(kManifestMagic.data(), kManifestMagic.size());
    header.push_back(static_cast<char>(kManifestFormatVersion));
    header.push_back(static_cast<char>(kStorageFormatVersion));
    PutFixed16(header, static_cast<std::uint16_t>(kManifestHeaderSize));
    PutFixed64(header, file_size);
    PutFixed64(header, version.id());
    PutFixed64(header, version.next_file_number());
    PutFixed64(header, version.last_sequence());
    PutFixed32(header, static_cast<std::uint32_t>(version.tables().size()));
    PutFixed32(header, static_cast<std::uint32_t>(payload.size()));
    PutFixed64(header, 0);
    std::string checksum_input = header;
    checksum_input.append(payload);
    PutFixed32(header, Crc32c(checksum_input));
    PutFixed32(header, 0);

    destination->reserve(static_cast<std::size_t>(file_size));
    destination->append(header);
    destination->append(payload);
    return Status::Ok();
}

Status DecodeManifest(
    std::string_view encoded,
    const Options& options,
    Version* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("Manifest output must not be null");
    }
    *output = {};
    if (encoded.size() < kManifestHeaderSize) {
        return ManifestCorruption("file is smaller than its header");
    }
    if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), encoded.begin())) {
        return ManifestCorruption("magic is invalid");
    }
    const auto manifest_version = static_cast<std::uint8_t>(encoded[4]);
    const auto storage_version = static_cast<std::uint8_t>(encoded[5]);
    if (manifest_version != kManifestFormatVersion ||
        storage_version != kStorageFormatVersion) {
        return Status::VersionMismatch(
            "manifest format " + std::to_string(manifest_version) +
            " / storage format " + std::to_string(storage_version) +
            " is unsupported"
        );
    }

    std::uint16_t header_size = 0;
    std::uint64_t file_size = 0;
    std::uint64_t version_id = 0;
    std::uint64_t next_file_number = 0;
    std::uint64_t last_sequence = 0;
    std::uint32_t table_count = 0;
    std::uint32_t payload_size = 0;
    std::uint64_t reserved64 = 0;
    std::uint32_t checksum = 0;
    std::uint32_t reserved32 = 0;
    if (!DecodeFixed16(encoded.substr(6), &header_size) ||
        !DecodeFixed64(encoded.substr(8), &file_size) ||
        !DecodeFixed64(encoded.substr(16), &version_id) ||
        !DecodeFixed64(encoded.substr(24), &next_file_number) ||
        !DecodeFixed64(encoded.substr(32), &last_sequence) ||
        !DecodeFixed32(encoded.substr(40), &table_count) ||
        !DecodeFixed32(encoded.substr(44), &payload_size) ||
        !DecodeFixed64(encoded.substr(48), &reserved64) ||
        !DecodeFixed32(encoded.substr(56), &checksum) ||
        !DecodeFixed32(encoded.substr(60), &reserved32) ||
        header_size != kManifestHeaderSize || file_size != encoded.size() ||
        payload_size != encoded.size() - kManifestHeaderSize ||
        encoded.size() > kMaximumManifestSize || reserved64 != 0 ||
        reserved32 != 0 ||
        table_count > payload_size / kManifestTableFixedSize) {
        return ManifestCorruption("header fields are invalid");
    }
    std::string checksum_input(encoded.substr(0, kManifestChecksumOffset));
    checksum_input.append(encoded.substr(kManifestHeaderSize));
    if (Crc32c(checksum_input) != checksum) {
        return ManifestCorruption("checksum mismatch");
    }

    Version version;
    version.id_ = version_id;
    version.next_file_number_ = next_file_number;
    version.last_sequence_ = last_sequence;
    version.tables_.reserve(table_count);
    std::size_t offset = kManifestHeaderSize;
    for (std::uint32_t index = 0; index < table_count; ++index) {
        if (encoded.size() - offset < kManifestTableFixedSize) {
            return ManifestCorruption("table record is truncated");
        }
        const auto record = encoded.substr(offset);
        std::uint32_t record_size = 0;
        VersionTable table;
        std::uint32_t minimum_key_size = 0;
        std::uint32_t maximum_key_size = 0;
        if (!DecodeFixed32(record, &record_size) ||
            !DecodeFixed32(record.substr(4), &table.level) ||
            !DecodeFixed64(record.substr(8), &table.metadata.generation) ||
            !DecodeFixed64(record.substr(16), &table.metadata.file_size) ||
            !DecodeFixed64(record.substr(24), &table.metadata.record_count) ||
            !DecodeFixed64(record.substr(32), &table.metadata.minimum_sequence) ||
            !DecodeFixed64(record.substr(40), &table.metadata.maximum_sequence) ||
            !DecodeFixed32(record.substr(48), &minimum_key_size) ||
            !DecodeFixed32(record.substr(52), &maximum_key_size)) {
            return ManifestCorruption("table record fields cannot be decoded");
        }
        const std::uint64_t expected_size =
            static_cast<std::uint64_t>(kManifestTableFixedSize) +
            minimum_key_size + maximum_key_size;
        if (record_size != expected_size || record_size > encoded.size() - offset ||
            minimum_key_size > options.max_key_size ||
            maximum_key_size > options.max_key_size) {
            return ManifestCorruption("table record length is invalid");
        }
        const auto keys = record.substr(kManifestTableFixedSize);
        table.metadata.minimum_key.assign(keys.substr(0, minimum_key_size));
        table.metadata.maximum_key.assign(
            keys.substr(minimum_key_size, maximum_key_size)
        );
        version.tables_.push_back(std::move(table));
        offset += record_size;
    }
    if (offset != encoded.size()) {
        return ManifestCorruption("unreferenced trailing bytes exist");
    }
    const auto validation = ValidateVersion(version, options);
    if (!validation.ok()) {
        return validation;
    }
    *output = std::move(version);
    return Status::Ok();
}

Status LoadManifest(
    const std::string& directory,
    const Options& options,
    Version* output
) {
    if (directory.empty() || output == nullptr) {
        return Status::InvalidArgument("Manifest load arguments are invalid");
    }
    *output = {};
    std::unique_ptr<RecoveryFile> file;
    auto status = PosixRecoveryFile::OpenReadOnly(
        JoinPath(directory, kManifestFileName),
        &file
    );
    if (!status.ok()) {
        return status;
    }
    std::uint64_t file_size = 0;
    status = file->Size(&file_size);
    if (!status.ok()) {
        return status;
    }
    if (file_size > kMaximumManifestSize ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        return ManifestCorruption("file exceeds the format size limit");
    }
    std::string encoded(static_cast<std::size_t>(file_size), '\0');
    status = ReadAllAt(*file, 0, &encoded);
    if (status.IsIncomplete()) {
        return ManifestCorruption("file was truncated while reading");
    }
    if (!status.ok()) {
        return status;
    }
    return DecodeManifest(encoded, options, output);
}

Status PublishManifest(
    const std::string& directory,
    const Version& version,
    const Options& options,
    FlushEnvironment& environment
) {
    if (directory.empty()) {
        return Status::InvalidArgument("Manifest directory must not be empty");
    }
    std::string encoded;
    auto status = EncodeManifest(version, options, &encoded);
    if (!status.ok()) {
        return status;
    }

    const std::string final_path = JoinPath(directory, kManifestFileName);
    const std::string temporary_path = final_path + ".tmp";
    std::unique_ptr<WritableFile> file;
    status = environment.CreateTruncated(temporary_path, &file);
    if (!status.ok()) {
        return status;
    }
    status = WriteAll(*file, encoded);
    if (!status.ok()) {
        file.reset();
        const auto cleanup = environment.RemoveFile(temporary_path);
        (void)cleanup;
        return status;
    }
    status = file->Sync();
    file.reset();
    if (!status.ok()) {
        const auto cleanup = environment.RemoveFile(temporary_path);
        (void)cleanup;
        return status;
    }
    status = environment.Rename(temporary_path, final_path);
    if (!status.ok()) {
        const auto cleanup = environment.RemoveFile(temporary_path);
        (void)cleanup;
        return status;
    }
    return environment.SyncDirectory(directory);
}

}  // namespace minikv
