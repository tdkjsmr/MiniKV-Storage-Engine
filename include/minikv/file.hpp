#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "minikv/status.hpp"

namespace minikv {

enum class WriteResultKind {
    kWritten,
    kInterrupted,
    kError,
};

enum class ReadResultKind {
    kRead,
    kEndOfFile,
    kInterrupted,
    kError,
};

struct ReadResult {
    ReadResultKind kind = ReadResultKind::kError;
    std::size_t bytes_read = 0;
    Status status;

    static ReadResult Read(std::size_t bytes_read);
    static ReadResult EndOfFile();
    static ReadResult Interrupted();
    static ReadResult Error(Status status);
};

struct WriteResult {
    WriteResultKind kind = WriteResultKind::kError;
    std::size_t bytes_written = 0;
    Status status;

    static WriteResult Written(std::size_t bytes_written);
    static WriteResult Interrupted();
    static WriteResult Error(Status status);
};

class WritableFile {
public:
    virtual ~WritableFile() = default;

    virtual WriteResult WriteSome(std::string_view data) = 0;
    virtual Status Sync() = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// Repeats WriteSome until all bytes are written. Interrupted calls and short
// writes are normal; a zero-byte successful write is treated as an I/O error
// to prevent an infinite loop.
Status WriteAll(WritableFile& file, std::string_view data);

class RecoveryFile {
public:
    virtual ~RecoveryFile() = default;

    virtual Status Size(std::uint64_t* size) = 0;
    virtual ReadResult ReadSome(
        std::uint64_t offset,
        char* destination,
        std::size_t size
    ) = 0;
    virtual Status Truncate(std::uint64_t size) = 0;
    virtual Status Sync() = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// Reads exactly data.size() bytes at offset. Reaching EOF before the requested
// length returns Incomplete; interrupted and short pread calls are retried.
Status ReadAllAt(RecoveryFile& file, std::uint64_t offset, std::string* data);

class PosixWritableFile final : public WritableFile {
public:
    static Status OpenAppend(
        std::string path,
        std::unique_ptr<WritableFile>* output
    );
    static Status OpenTruncate(
        std::string path,
        std::unique_ptr<WritableFile>* output
    );

    ~PosixWritableFile() override;

    PosixWritableFile(const PosixWritableFile&) = delete;
    PosixWritableFile& operator=(const PosixWritableFile&) = delete;

    WriteResult WriteSome(std::string_view data) override;
    Status Sync() override;
    [[nodiscard]] std::string_view name() const noexcept override { return path_; }

private:
    PosixWritableFile(int file_descriptor, std::string path);

    int file_descriptor_ = -1;
    std::string path_;
};

class PosixRecoveryFile final : public RecoveryFile {
public:
    static Status Open(
        std::string path,
        std::unique_ptr<RecoveryFile>* output
    );

    ~PosixRecoveryFile() override;

    PosixRecoveryFile(const PosixRecoveryFile&) = delete;
    PosixRecoveryFile& operator=(const PosixRecoveryFile&) = delete;

    Status Size(std::uint64_t* size) override;
    ReadResult ReadSome(
        std::uint64_t offset,
        char* destination,
        std::size_t size
    ) override;
    Status Truncate(std::uint64_t size) override;
    Status Sync() override;
    [[nodiscard]] std::string_view name() const noexcept override { return path_; }

private:
    PosixRecoveryFile(int file_descriptor, std::string path);

    int file_descriptor_ = -1;
    std::string path_;
};

}  // namespace minikv
