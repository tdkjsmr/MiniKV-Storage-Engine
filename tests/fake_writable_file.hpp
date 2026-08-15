#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minikv/file.hpp"

namespace minikv::test {

class ScriptedWritableFile final : public WritableFile {
public:
    enum class ActionKind {
        kWrite,
        kInterrupt,
        kError,
        kZeroProgress,
        kOverReport,
    };

    struct Action {
        ActionKind kind = ActionKind::kWrite;
        std::size_t byte_count = 0;
        Status status;

        static Action Write(std::size_t byte_count) {
            return {ActionKind::kWrite, byte_count, Status::Ok()};
        }
        static Action Interrupt() {
            return {ActionKind::kInterrupt, 0, Status::Ok()};
        }
        static Action Error(std::string message) {
            return {
                ActionKind::kError,
                0,
                Status::IOError(std::move(message)),
            };
        }
        static Action ZeroProgress() {
            return {ActionKind::kZeroProgress, 0, Status::Ok()};
        }
        static Action OverReport() {
            return {ActionKind::kOverReport, 0, Status::Ok()};
        }
    };

    explicit ScriptedWritableFile(std::vector<Action> actions = {})
        : actions_(std::move(actions)) {}

    WriteResult WriteSome(std::string_view data) override {
        ++write_calls_;
        if (next_action_ >= actions_.size()) {
            contents_.append(data);
            return WriteResult::Written(data.size());
        }

        const Action& action = actions_[next_action_++];
        switch (action.kind) {
            case ActionKind::kInterrupt:
                return WriteResult::Interrupted();
            case ActionKind::kError:
                return WriteResult::Error(action.status);
            case ActionKind::kZeroProgress:
                return WriteResult::Written(0);
            case ActionKind::kOverReport:
                return WriteResult::Written(data.size() + 1U);
            case ActionKind::kWrite: {
                const std::size_t bytes = std::min(action.byte_count, data.size());
                contents_.append(data.substr(0, bytes));
                return WriteResult::Written(bytes);
            }
        }
        return WriteResult::Error(Status::IOError("unknown fake write action"));
    }

    Status Sync() override {
        ++sync_calls_;
        return sync_status_;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "scripted.wal";
    }

    void set_sync_status(Status status) { sync_status_ = std::move(status); }

    [[nodiscard]] const std::string& contents() const noexcept { return contents_; }
    [[nodiscard]] std::size_t write_calls() const noexcept { return write_calls_; }
    [[nodiscard]] std::size_t sync_calls() const noexcept { return sync_calls_; }

private:
    std::vector<Action> actions_;
    std::size_t next_action_ = 0;
    std::string contents_;
    std::size_t write_calls_ = 0;
    std::size_t sync_calls_ = 0;
    Status sync_status_;
};

}  // namespace minikv::test
