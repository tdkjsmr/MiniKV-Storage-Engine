#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "minikv/file.hpp"

namespace minikv::test {

class ScriptedRecoveryFile final : public RecoveryFile {
public:
    explicit ScriptedRecoveryFile(std::string contents)
        : contents_(std::move(contents)) {}

    Status Size(std::uint64_t* size) override {
        if (!size_status_.ok()) {
            return size_status_;
        }
        if (size == nullptr) {
            return Status::InvalidArgument("injected size destination is null");
        }
        *size = static_cast<std::uint64_t>(contents_.size());
        return Status::Ok();
    }

    ReadResult ReadSome(
        std::uint64_t offset,
        char* destination,
        std::size_t size
    ) override {
        ++read_calls_;
        if (interrupts_remaining_ > 0) {
            --interrupts_remaining_;
            return ReadResult::Interrupted();
        }
        if (!read_status_.ok()) {
            return ReadResult::Error(read_status_);
        }
        if (over_report_) {
            return ReadResult::Read(size + 1U);
        }
        if (zero_progress_) {
            return ReadResult::Read(0);
        }
        if (offset >= contents_.size()) {
            return ReadResult::EndOfFile();
        }
        const auto available =
            contents_.size() - static_cast<std::size_t>(offset);
        const std::size_t count = std::min(
            {size, available, maximum_chunk_size_}
        );
        std::memcpy(
            destination,
            contents_.data() + static_cast<std::size_t>(offset),
            count
        );
        return ReadResult::Read(count);
    }

    Status Truncate(std::uint64_t size) override {
        ++truncate_calls_;
        if (!truncate_status_.ok()) {
            return truncate_status_;
        }
        if (size > contents_.size()) {
            return Status::IOError("injected truncate would extend file");
        }
        contents_.resize(static_cast<std::size_t>(size));
        return Status::Ok();
    }

    Status Sync() override {
        ++sync_calls_;
        return sync_status_;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "scripted-recovery-file";
    }

    void set_maximum_chunk_size(std::size_t size) {
        maximum_chunk_size_ = size;
    }
    void set_interrupts(std::size_t count) { interrupts_remaining_ = count; }
    void set_size_status(Status status) { size_status_ = std::move(status); }
    void set_read_status(Status status) { read_status_ = std::move(status); }
    void set_truncate_status(Status status) {
        truncate_status_ = std::move(status);
    }
    void set_sync_status(Status status) { sync_status_ = std::move(status); }
    void set_zero_progress(bool enabled) { zero_progress_ = enabled; }
    void set_over_report(bool enabled) { over_report_ = enabled; }

    [[nodiscard]] const std::string& contents() const noexcept {
        return contents_;
    }
    [[nodiscard]] std::size_t read_calls() const noexcept { return read_calls_; }
    [[nodiscard]] std::size_t truncate_calls() const noexcept {
        return truncate_calls_;
    }
    [[nodiscard]] std::size_t sync_calls() const noexcept { return sync_calls_; }

private:
    std::string contents_;
    std::size_t maximum_chunk_size_ = std::numeric_limits<std::size_t>::max();
    std::size_t interrupts_remaining_ = 0;
    std::size_t read_calls_ = 0;
    std::size_t truncate_calls_ = 0;
    std::size_t sync_calls_ = 0;
    Status size_status_;
    Status read_status_;
    Status truncate_status_;
    Status sync_status_;
    bool zero_progress_ = false;
    bool over_report_ = false;
};

}  // namespace minikv::test
