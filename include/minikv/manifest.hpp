#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/options.hpp"
#include "minikv/status.hpp"
#include "minikv/table.hpp"

namespace minikv {

class FlushEnvironment;

inline constexpr std::uint8_t kManifestFormatVersion = 1;
inline constexpr std::uint8_t kStorageFormatVersion = 2;
inline constexpr std::size_t kManifestHeaderSize = 64;
inline constexpr std::size_t kManifestTableFixedSize = 56;
inline constexpr std::size_t kMaximumManifestSize = 64U * 1024U * 1024U;
inline constexpr char kManifestFileName[] = "MANIFEST";

struct VersionTable {
    std::uint32_t level = 0;
    SSTableMetadata metadata;
};

struct VersionEdit {
    std::vector<VersionTable> added_tables;
    std::vector<std::uint64_t> deleted_file_numbers;
    std::optional<std::uint64_t> next_file_number;
    std::optional<std::uint64_t> last_sequence;
};

class Version {
public:
    static Version NewDatabase();

    [[nodiscard]] Status Apply(
        const VersionEdit& edit,
        const Options& options,
        Version* output
    ) const;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t next_file_number() const noexcept {
        return next_file_number_;
    }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept {
        return last_sequence_;
    }
    [[nodiscard]] const std::vector<VersionTable>& tables() const noexcept {
        return tables_;
    }

private:
    friend Status DecodeManifest(
        std::string_view encoded,
        const Options& options,
        Version* output
    );

    std::uint64_t id_ = 0;
    std::uint64_t next_file_number_ = 0;
    std::uint64_t last_sequence_ = 0;
    std::vector<VersionTable> tables_;
};

[[nodiscard]] Status EncodeManifest(
    const Version& version,
    const Options& options,
    std::string* destination
);

[[nodiscard]] Status DecodeManifest(
    std::string_view encoded,
    const Options& options,
    Version* output
);

[[nodiscard]] Status LoadManifest(
    const std::string& directory,
    const Options& options,
    Version* output
);

// Atomically replaces the full current Version:
// temp write -> file sync -> rename -> directory sync.
[[nodiscard]] Status PublishManifest(
    const std::string& directory,
    const Version& version,
    const Options& options,
    FlushEnvironment& environment
);

}  // namespace minikv
