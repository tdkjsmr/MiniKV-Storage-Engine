#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "minikv/file.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"
#include "minikv/wal.hpp"

namespace minikv {

// WritePath is the V1 write coordinator. It deliberately exposes only an
// in-process API; Open and crash recovery are added in V2.
class WritePath {
public:
    static Status Create(
        Options options,
        std::unique_ptr<WritableFile> wal_file,
        std::unique_ptr<WritePath>* output
    );

    Status Put(
        std::string_view key,
        std::string_view value,
        WriteOptions write_options = {}
    );
    Status Delete(std::string_view key, WriteOptions write_options = {});

    [[nodiscard]] LookupResult Get(std::string_view key) const;
    [[nodiscard]] std::uint64_t last_sequence() const noexcept {
        return last_sequence_;
    }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    WritePath(Options options, std::unique_ptr<WritableFile> wal_file);

    Status Write(
        ValueType type,
        std::string_view key,
        std::string_view value,
        WriteOptions write_options
    );

    Options options_;
    WalWriter wal_;
    MemTable memtable_;
    std::uint64_t last_sequence_ = 0;
    Status status_;
};

}  // namespace minikv
