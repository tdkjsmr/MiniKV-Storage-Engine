#pragma once

#include <string>
#include <utility>

namespace minikv {

enum class StatusCode {
    kOk = 0,
    kNotFound,
    kIncomplete,
    kInvalidArgument,
    kIOError,
    kCorruption,
    kClosed,
};

class Status {
public:
    Status() = default;

    static Status Ok();
    static Status NotFound(std::string message = {});
    static Status Incomplete(std::string message);
    static Status InvalidArgument(std::string message);
    static Status IOError(std::string message);
    static Status Corruption(std::string message);
    static Status Closed(std::string message = {});

    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
    [[nodiscard]] bool IsNotFound() const noexcept {
        return code_ == StatusCode::kNotFound;
    }
    [[nodiscard]] bool IsIncomplete() const noexcept {
        return code_ == StatusCode::kIncomplete;
    }
    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string ToString() const;

private:
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    StatusCode code_ = StatusCode::kOk;
    std::string message_;
};

}  // namespace minikv
