#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "minikv/file.hpp"
#include "minikv/memtable.hpp"
#include "minikv/options.hpp"
#include "minikv/status.hpp"
#include "minikv/table.hpp"

namespace minikv {

class FlushEnvironment {
public:
    virtual ~FlushEnvironment() = default;

    virtual Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<WritableFile>* output
    ) = 0;
    virtual Status Rename(
        const std::string& source,
        const std::string& destination
    ) = 0;
    virtual Status RemoveFile(const std::string& path) = 0;
    virtual Status SyncDirectory(const std::string& directory) = 0;
};

class PosixFlushEnvironment final : public FlushEnvironment {
public:
    Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<WritableFile>* output
    ) override;
    Status Rename(
        const std::string& source,
        const std::string& destination
    ) override;
    Status RemoveFile(const std::string& path) override;
    Status SyncDirectory(const std::string& directory) override;
};

[[nodiscard]] std::string GenerationFileName(
    std::uint64_t generation,
    std::string extension
);

// Publishes one immutable generation in crash-safe file order:
// temp write -> file sync -> rename -> directory sync.
Status PublishTable(
    const std::string& directory,
    std::uint64_t generation,
    const MemTable& memtable,
    const Options& options,
    FlushEnvironment& environment,
    std::unique_ptr<SSTableReader>* published
);

}  // namespace minikv
