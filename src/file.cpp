#include "minikv/file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
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

ReadResult ReadResult::Read(std::size_t bytes_read) {
    return {ReadResultKind::kRead, bytes_read, Status::Ok()};
}

ReadResult ReadResult::EndOfFile() {
    return {ReadResultKind::kEndOfFile, 0, Status::Ok()};
}

ReadResult ReadResult::Interrupted() {
    return {ReadResultKind::kInterrupted, 0, Status::Ok()};
}

ReadResult ReadResult::Error(Status status) {
    return {ReadResultKind::kError, 0, std::move(status)};
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

Status ReadAllAt(RecoveryFile& file, std::uint64_t offset, std::string* data) {
    if (data == nullptr) {
        return Status::InvalidArgument("read destination must not be null");
    }

    std::size_t bytes_read = 0;
    while (bytes_read < data->size()) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - bytes_read) {
            return Status::IOError(
                "read offset overflows for '" + std::string(file.name()) + "'"
            );
        }
        const auto result = file.ReadSome(
            offset + bytes_read,
            data->data() + bytes_read,
            data->size() - bytes_read
        );
        if (result.kind == ReadResultKind::kInterrupted) {
            continue;
        }
        if (result.kind == ReadResultKind::kEndOfFile) {
            return Status::Incomplete(
                "unexpected EOF while reading '" + std::string(file.name()) + "'"
            );
        }
        if (result.kind == ReadResultKind::kError) {
            if (result.status.ok()) {
                return Status::IOError(
                    "read failed for '" + std::string(file.name()) + "' without an error"
                );
            }
            return result.status;
        }
        if (result.bytes_read == 0) {
            return Status::IOError(
                "read made no progress for '" + std::string(file.name()) + "'"
            );
        }
        if (result.bytes_read > data->size() - bytes_read) {
            return Status::IOError(
                "read reported too many bytes for '" + std::string(file.name()) + "'"
            );
        }
        bytes_read += result.bytes_read;
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

Status PosixWritableFile::OpenTruncate(
    std::string path,
    std::unique_ptr<WritableFile>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("file output pointer must not be null");
    }
    output->reset();
    if (path.empty()) {
        return Status::InvalidArgument("file path must not be empty");
    }

    int file_descriptor = -1;
    do {
        file_descriptor = ::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
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

Status PosixRecoveryFile::Open(
    std::string path,
    std::unique_ptr<RecoveryFile>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("recovery file output pointer must not be null");
    }
    output->reset();
    if (path.empty()) {
        return Status::InvalidArgument("recovery file path must not be empty");
    }

    int file_descriptor = -1;
    do {
        file_descriptor = ::open(
            path.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC,
            static_cast<mode_t>(0644)
        );
    } while (file_descriptor < 0 && errno == EINTR);

    if (file_descriptor < 0) {
        return Status::IOError(IOErrorMessage("open", path, errno));
    }

    output->reset(new PosixRecoveryFile(file_descriptor, std::move(path)));
    return Status::Ok();
}

PosixRecoveryFile::PosixRecoveryFile(int file_descriptor, std::string path)
    : file_descriptor_(file_descriptor), path_(std::move(path)) {}

PosixRecoveryFile::~PosixRecoveryFile() {
    if (file_descriptor_ >= 0) {
        const int close_result = ::close(file_descriptor_);
        (void)close_result;
    }
}

Status PosixRecoveryFile::Size(std::uint64_t* size) {
    if (size == nullptr) {
        return Status::InvalidArgument("file size destination must not be null");
    }

    struct stat file_status {};
    int result = -1;
    do {
        result = ::fstat(file_descriptor_, &file_status);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return Status::IOError(IOErrorMessage("fstat", path_, errno));
    }
    if (file_status.st_size < 0) {
        return Status::IOError("fstat returned a negative size for '" + path_ + "'");
    }
    *size = static_cast<std::uint64_t>(file_status.st_size);
    return Status::Ok();
}

ReadResult PosixRecoveryFile::ReadSome(
    std::uint64_t offset,
    char* destination,
    std::size_t size
) {
    if (size == 0) {
        return ReadResult::Read(0);
    }
    if (destination == nullptr) {
        return ReadResult::Error(
            Status::InvalidArgument("pread destination must not be null")
        );
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return ReadResult::Error(Status::IOError("pread offset exceeds off_t range"));
    }

    const std::size_t maximum_read = static_cast<std::size_t>(
        std::numeric_limits<ssize_t>::max()
    );
    const std::size_t requested = std::min(size, maximum_read);
    const ssize_t result = ::pread(
        file_descriptor_,
        destination,
        requested,
        static_cast<off_t>(offset)
    );
    if (result < 0) {
        if (errno == EINTR) {
            return ReadResult::Interrupted();
        }
        return ReadResult::Error(
            Status::IOError(IOErrorMessage("pread", path_, errno))
        );
    }
    if (result == 0) {
        return ReadResult::EndOfFile();
    }
    return ReadResult::Read(static_cast<std::size_t>(result));
}

Status PosixRecoveryFile::Truncate(std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return Status::IOError("truncate size exceeds off_t range");
    }

    int result = -1;
    do {
        result = ::ftruncate(file_descriptor_, static_cast<off_t>(size));
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return Status::IOError(IOErrorMessage("ftruncate", path_, errno));
    }
    return Status::Ok();
}

Status PosixRecoveryFile::Sync() {
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
