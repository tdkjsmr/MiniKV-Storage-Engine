#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "minikv/file.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"

namespace minikv {

inline constexpr std::uint8_t kWalFormatVersion = 1;
inline constexpr std::size_t kWalHeaderSize = 32;

enum class SyncMode {
    kSync,
    kAsync,
};

struct WriteOptions {
    SyncMode sync_mode = SyncMode::kSync;
};

struct WalRecord {
    ValueType type = ValueType::kValue;
    std::uint64_t sequence = 0;
    std::string key;
    std::string value;
};

struct WalDecodeResult {
    Status status;
    std::size_t bytes_consumed = 0;
    WalRecord record;
};

struct WalHeaderDecodeResult {
    Status status;
    ValueType type = ValueType::kValue;
    std::uint32_t record_size = 0;
    std::uint64_t sequence = 0;
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;
    std::uint32_t checksum = 0;
};

Status ValidateWalRecord(const WalRecord& record, const Options& options);
Status EncodeWalRecord(
    const WalRecord& record,
    const Options& options,
    std::string* destination
);
[[nodiscard]] WalHeaderDecodeResult DecodeWalRecordHeader(
    std::string_view input,
    const Options& options
);
[[nodiscard]] WalDecodeResult DecodeWalRecord(
    std::string_view input,
    const Options& options
);

class WalWriter {
public:
    WalWriter(Options options, std::unique_ptr<WritableFile> file);

    static Status Open(
        std::string path,
        Options options,
        std::unique_ptr<WalWriter>* output
    );

    Status Append(const WalRecord& record, WriteOptions write_options = {});

    [[nodiscard]] bool failed() const noexcept { return !status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] std::string_view file_name() const noexcept;

private:
    Status RememberFailure(Status status);

    Options options_;
    std::unique_ptr<WritableFile> file_;
    Status status_;
};

}  // namespace minikv
