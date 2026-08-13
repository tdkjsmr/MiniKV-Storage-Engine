#include "minikv/flush.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace minikv {
namespace {

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (!directory.empty() && directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

std::string IOErrorMessage(
    std::string_view operation,
    std::string_view path,
    int error
) {
    return std::string(operation) + " '" + std::string(path) + "': " +
           std::strerror(error);
}

void BestEffortRemove(FlushEnvironment& environment, const std::string& path) {
    const auto cleanup_status = environment.RemoveFile(path);
    (void)cleanup_status;
}

}  // namespace

Status PosixFlushEnvironment::CreateTruncated(
    const std::string& path,
    std::unique_ptr<WritableFile>* output
) {
    return PosixWritableFile::OpenTruncate(path, output);
}

Status PosixFlushEnvironment::Rename(
    const std::string& source,
    const std::string& destination
) {
    int result = -1;
    do {
        result = ::rename(source.c_str(), destination.c_str());
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return Status::IOError(IOErrorMessage("rename", source, errno));
    }
    return Status::Ok();
}

Status PosixFlushEnvironment::RemoveFile(const std::string& path) {
    int result = -1;
    do {
        result = ::unlink(path.c_str());
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != ENOENT) {
        return Status::IOError(IOErrorMessage("unlink", path, errno));
    }
    return Status::Ok();
}

Status PosixFlushEnvironment::SyncDirectory(const std::string& directory) {
    int descriptor = -1;
    do {
        descriptor = ::open(
            directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC
        );
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return Status::IOError(IOErrorMessage("open directory", directory, errno));
    }

    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result < 0 && errno == EINTR);
    const int sync_error = errno;
    const int close_result = ::close(descriptor);
    (void)close_result;
    if (sync_result < 0) {
        return Status::IOError(
            IOErrorMessage("fsync directory", directory, sync_error)
        );
    }
    return Status::Ok();
}

std::string GenerationFileName(
    std::uint64_t generation,
    std::string extension
) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(20) << generation << '.'
           << std::move(extension);
    return output.str();
}

Status PublishTable(
    const std::string& directory,
    std::uint64_t generation,
    const MemTable& memtable,
    const Options& options,
    FlushEnvironment& environment,
    std::unique_ptr<TableData>* published
) {
    if (published == nullptr) {
        return Status::InvalidArgument("published table output must not be null");
    }
    published->reset();
    if (directory.empty() || generation == 0 || memtable.empty()) {
        return Status::InvalidArgument(
            "flush requires a directory, generation, and non-empty MemTable"
        );
    }

    std::string encoded;
    const auto encode_status = EncodeTable(
        generation,
        memtable,
        options,
        &encoded
    );
    if (!encode_status.ok()) {
        return encode_status;
    }

    const std::string final_path = JoinPath(
        directory,
        GenerationFileName(generation, "sst")
    );
    const std::string temporary_path = final_path + ".tmp";
    std::unique_ptr<WritableFile> file;
    auto status = environment.CreateTruncated(temporary_path, &file);
    if (!status.ok()) {
        return status;
    }

    status = WriteAll(*file, encoded);
    if (!status.ok()) {
        file.reset();
        BestEffortRemove(environment, temporary_path);
        return status;
    }
    status = file->Sync();
    file.reset();
    if (!status.ok()) {
        BestEffortRemove(environment, temporary_path);
        return status;
    }
    status = environment.Rename(temporary_path, final_path);
    if (!status.ok()) {
        BestEffortRemove(environment, temporary_path);
        return status;
    }
    status = environment.SyncDirectory(directory);
    if (!status.ok()) {
        return status;
    }

    return DecodeTable(encoded, options, published);
}

}  // namespace minikv
