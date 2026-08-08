#include "minikv/status.hpp"

#include <string_view>

namespace minikv {
namespace {

std::string_view CodeName(StatusCode code) {
    switch (code) {
        case StatusCode::kOk:
            return "OK";
        case StatusCode::kNotFound:
            return "NotFound";
        case StatusCode::kIncomplete:
            return "Incomplete";
        case StatusCode::kInvalidArgument:
            return "InvalidArgument";
        case StatusCode::kIOError:
            return "IOError";
        case StatusCode::kCorruption:
            return "Corruption";
        case StatusCode::kClosed:
            return "Closed";
    }
    return "Unknown";
}

}  // namespace

Status Status::Ok() {
    return {};
}

Status Status::NotFound(std::string message) {
    return {StatusCode::kNotFound, std::move(message)};
}

Status Status::Incomplete(std::string message) {
    return {StatusCode::kIncomplete, std::move(message)};
}

Status Status::InvalidArgument(std::string message) {
    return {StatusCode::kInvalidArgument, std::move(message)};
}

Status Status::IOError(std::string message) {
    return {StatusCode::kIOError, std::move(message)};
}

Status Status::Corruption(std::string message) {
    return {StatusCode::kCorruption, std::move(message)};
}

Status Status::Closed(std::string message) {
    return {StatusCode::kClosed, std::move(message)};
}

std::string Status::ToString() const {
    const std::string name(CodeName(code_));
    if (message_.empty()) {
        return name;
    }
    return name + ": " + message_;
}

}  // namespace minikv
