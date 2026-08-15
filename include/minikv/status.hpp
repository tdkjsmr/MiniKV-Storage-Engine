#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace minikv {

// Numeric values are part of the public API. External adapters may persist or
// transmit them, so append new codes instead of reordering existing entries.
enum class StatusCode : std::uint8_t {
    kOk = 0,
    kNotFound = 1,
    kIncomplete = 2,
    kInvalidArgument = 3,
    kIOError = 4,
    kCorruption = 5,
    kVersionMismatch = 6,
    kClosed = 7,
};

[[nodiscard]] std::string_view StatusCodeName(StatusCode code) noexcept;

class Status {
public:
    Status() = default;

    static Status Ok();
    static Status NotFound(std::string message = {});
    static Status Incomplete(std::string message);
    static Status InvalidArgument(std::string message);
    static Status IOError(std::string message);
    static Status Corruption(std::string message);
    static Status VersionMismatch(std::string message);
    static Status Closed(std::string message = {});

    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
    [[nodiscard]] bool IsNotFound() const noexcept {
        return code_ == StatusCode::kNotFound;
    }
    [[nodiscard]] bool IsIncomplete() const noexcept {
        return code_ == StatusCode::kIncomplete;
    }
    [[nodiscard]] bool IsInvalidArgument() const noexcept {
        return code_ == StatusCode::kInvalidArgument;
    }
    [[nodiscard]] bool IsIOError() const noexcept {
        return code_ == StatusCode::kIOError;
    }
    [[nodiscard]] bool IsCorruption() const noexcept {
        return code_ == StatusCode::kCorruption;
    }
    [[nodiscard]] bool IsVersionMismatch() const noexcept {
        return code_ == StatusCode::kVersionMismatch;
    }
    [[nodiscard]] bool IsClosed() const noexcept {
        return code_ == StatusCode::kClosed;
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
