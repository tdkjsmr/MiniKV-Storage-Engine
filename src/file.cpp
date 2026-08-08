#include "minikv/file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace minikv {
namespace {

std::string IOErrorMessage(std::string_view operation, std::string_view path, int error) {
    std::string message(operation);
    message += " '";
    message += path;
    message += "': ";
    message += std::strerror(error);
    return message;
}

}  // namespace

WriteResult WriteResult::Written(std::size_t bytes_written) {
    return {WriteResultKind::kWritten, bytes_written, Status::Ok()};
}

WriteResult WriteResult::Interrupted() {
    return {WriteResultKind::kInterrupted, 0, Status::Ok()};
}

WriteResult WriteResult::Error(Status status) {
    return {WriteResultKind::kError, 0, std::move(status)};
}

Status WriteAll(WritableFile& file, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto result = file.WriteSome(data.substr(offset));
        if (result.kind == WriteResultKind::kInterrupted) {
            continue;
        }
        if (result.kind == WriteResultKind::kError) {
            if (result.status.ok()) {
                return Status::IOError(
                    "write failed for '" + std::string(file.name()) + "' without an error"
                );
            }
            return result.status;
        }
        if (result.bytes_written == 0) {
            return Status::IOError(
                "write made no progress for '" + std::string(file.name()) + "'"
            );
        }
        if (result.bytes_written > data.size() - offset) {
            return Status::IOError(
                "write reported too many bytes for '" + std::string(file.name()) + "'"
            );
        }
        offset += result.bytes_written;
    }
    return Status::Ok();
}

Status PosixWritableFile::OpenAppend(
    std::string path,
    std::unique_ptr<WritableFile>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("WAL output pointer must not be null");
    }
    output->reset();
    if (path.empty()) {
        return Status::InvalidArgument("WAL path must not be empty");
    }

    int file_descriptor = -1;
    do {
        file_descriptor = ::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            static_cast<mode_t>(0644)
        );
    } while (file_descriptor < 0 && errno == EINTR);

    if (file_descriptor < 0) {
        return Status::IOError(IOErrorMessage("open", path, errno));
    }

    output->reset(new PosixWritableFile(file_descriptor, std::move(path)));
    return Status::Ok();
}

PosixWritableFile::PosixWritableFile(int file_descriptor, std::string path)
    : file_descriptor_(file_descriptor), path_(std::move(path)) {}

PosixWritableFile::~PosixWritableFile() {
    if (file_descriptor_ >= 0) {
        // Do not retry close after EINTR: on Linux the descriptor has already
        // been released and could be reused by another thread.
        const int close_result = ::close(file_descriptor_);
        (void)close_result;
    }
}

WriteResult PosixWritableFile::WriteSome(std::string_view data) {
    if (data.empty()) {
        return WriteResult::Written(0);
    }

    const std::size_t maximum_write = static_cast<std::size_t>(
        std::numeric_limits<ssize_t>::max()
    );
    const std::size_t requested = std::min(data.size(), maximum_write);
    const ssize_t result = ::write(file_descriptor_, data.data(), requested);

    if (result < 0) {
        if (errno == EINTR) {
            return WriteResult::Interrupted();
        }
        return WriteResult::Error(
            Status::IOError(IOErrorMessage("write", path_, errno))
        );
    }
    return WriteResult::Written(static_cast<std::size_t>(result));
}

Status PosixWritableFile::Sync() {
    int result = -1;
    do {
        result = ::fdatasync(file_descriptor_);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return Status::IOError(IOErrorMessage("fdatasync", path_, errno));
    }
    return Status::Ok();
}

}  // namespace minikv
