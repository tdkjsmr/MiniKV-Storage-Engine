#pragma once

#include <cstddef>
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

class PosixWritableFile final : public WritableFile {
public:
    static Status OpenAppend(
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

}  // namespace minikv
