#pragma once

#include <memory>
#include <string>

#include "minikv/status.hpp"

namespace minikv {

class DatabaseLock {
public:
    static Status Acquire(
        const std::string& directory,
        std::unique_ptr<DatabaseLock>* output
    );

    ~DatabaseLock();

    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;

private:
    explicit DatabaseLock(int descriptor);

    int descriptor_ = -1;
};

}  // namespace minikv
