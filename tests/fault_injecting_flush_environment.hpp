#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "minikv/flush.hpp"

namespace minikv::test {

enum class FlushFailurePoint {
    kNone,
    kCreate,
    kWrite,
    kFileSync,
    kRename,
    kDirectorySync,
    kRemoveWal,
};

class FaultInjectingFlushEnvironment final : public FlushEnvironment {
private:
    class FaultInjectingFile final : public WritableFile {
    public:
        FaultInjectingFile(
            std::unique_ptr<WritableFile> inner,
            FlushFailurePoint* active_failure
        )
            : inner_(std::move(inner)), active_failure_(active_failure) {}

        WriteResult WriteSome(std::string_view data) override {
            if (*active_failure_ == FlushFailurePoint::kWrite) {
                return WriteResult::Error(Status::IOError("injected table write"));
            }
            return inner_->WriteSome(data);
        }

        Status Sync() override {
            if (*active_failure_ == FlushFailurePoint::kFileSync) {
                return Status::IOError("injected table sync");
            }
            return inner_->Sync();
        }

        [[nodiscard]] std::string_view name() const noexcept override {
            return inner_->name();
        }

    private:
        std::unique_ptr<WritableFile> inner_;
        FlushFailurePoint* active_failure_;
    };

public:
    void Arm(FlushFailurePoint failure) { active_failure_ = failure; }
    void Disarm() { active_failure_ = FlushFailurePoint::kNone; }

    Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<WritableFile>* output
    ) override {
        if (active_failure_ == FlushFailurePoint::kCreate) {
            return Status::IOError("injected table create");
        }
        std::unique_ptr<WritableFile> inner;
        const auto status = delegate_.CreateTruncated(path, &inner);
        if (!status.ok()) {
            return status;
        }
        output->reset(new FaultInjectingFile(std::move(inner), &active_failure_));
        return Status::Ok();
    }

    Status Rename(
        const std::string& source,
        const std::string& destination
    ) override {
        if (active_failure_ == FlushFailurePoint::kRename) {
            return Status::IOError("injected table rename");
        }
        return delegate_.Rename(source, destination);
    }

    Status RemoveFile(const std::string& path) override {
        if (active_failure_ == FlushFailurePoint::kRemoveWal &&
            path.size() >= 4 && path.substr(path.size() - 4) == ".wal") {
            return Status::IOError("injected WAL removal");
        }
        return delegate_.RemoveFile(path);
    }

    Status SyncDirectory(const std::string& directory) override {
        if (active_failure_ == FlushFailurePoint::kDirectorySync) {
            return Status::IOError("injected directory sync");
        }
        return delegate_.SyncDirectory(directory);
    }

private:
    PosixFlushEnvironment delegate_;
    FlushFailurePoint active_failure_ = FlushFailurePoint::kNone;
};

}  // namespace minikv::test
