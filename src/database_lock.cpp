#include "database_lock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

namespace minikv {
namespace {

std::string IOErrorMessage(
    std::string_view operation,
    std::string_view path,
    int error
) {
    return std::string(operation) + " '" + std::string(path) + "': " +
           std::strerror(error);
}

}  // namespace

Status DatabaseLock::Acquire(
    const std::string& directory,
    std::unique_ptr<DatabaseLock>* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("database lock output must not be null");
    }
    output->reset();
    if (directory.empty()) {
        return Status::InvalidArgument("database lock directory must not be empty");
    }
    const std::string path = directory + "/LOCK";
    int descriptor = -1;
    do {
        descriptor = ::open(
            path.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC,
            static_cast<mode_t>(0644)
        );
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return Status::IOError(IOErrorMessage("open lock", path, errno));
    }

    int result = -1;
    do {
        result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        const int lock_error = errno;
        const int close_result = ::close(descriptor);
        (void)close_result;
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
            return Status::IOError(
                "database directory is already locked: '" + path + "'"
            );
        }
        return Status::IOError(IOErrorMessage("flock", path, lock_error));
    }

    output->reset(new DatabaseLock(descriptor));
    return Status::Ok();
}

DatabaseLock::DatabaseLock(int descriptor) : descriptor_(descriptor) {}

DatabaseLock::~DatabaseLock() {
    if (descriptor_ >= 0) {
        const int unlock_result = ::flock(descriptor_, LOCK_UN);
        (void)unlock_result;
        const int close_result = ::close(descriptor_);
        (void)close_result;
    }
}

}  // namespace minikv
