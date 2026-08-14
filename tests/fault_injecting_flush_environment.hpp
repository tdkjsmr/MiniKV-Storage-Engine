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
    kManifestCreate,
    kManifestWrite,
    kManifestFileSync,
    kManifestRename,
    kManifestDirectorySync,
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
            if (*active_failure_ == FlushFailurePoint::kWrite ||
                (*active_failure_ == FlushFailurePoint::kManifestWrite &&
                 IsManifestTemporary(inner_->name()))) {
                return WriteResult::Error(Status::IOError("injected table write"));
            }
            return inner_->WriteSome(data);
        }

        Status Sync() override {
            if (*active_failure_ == FlushFailurePoint::kFileSync ||
                (*active_failure_ == FlushFailurePoint::kManifestFileSync &&
                 IsManifestTemporary(inner_->name()))) {
                return Status::IOError("injected table sync");
            }
            return inner_->Sync();
        }

        [[nodiscard]] std::string_view name() const noexcept override {
            return inner_->name();
        }

    private:
        static bool IsManifestTemporary(std::string_view path) {
            constexpr std::string_view suffix = "MANIFEST.tmp";
            return path.size() >= suffix.size() &&
                   path.substr(path.size() - suffix.size()) == suffix;
        }

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
        const bool manifest =
            path.size() >= 12 && path.substr(path.size() - 12) == "MANIFEST.tmp";
        if (active_failure_ == FlushFailurePoint::kCreate ||
            (active_failure_ == FlushFailurePoint::kManifestCreate && manifest)) {
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
        const bool manifest = destination.size() >= 8 &&
            destination.substr(destination.size() - 8) == "MANIFEST";
        if (active_failure_ == FlushFailurePoint::kRename ||
            (active_failure_ == FlushFailurePoint::kManifestRename && manifest)) {
            return Status::IOError("injected table rename");
        }
        const auto status = delegate_.Rename(source, destination);
        if (status.ok() && manifest) {
            manifest_renamed_ = true;
        }
        return status;
    }

    Status RemoveFile(const std::string& path) override {
        if (active_failure_ == FlushFailurePoint::kRemoveWal &&
            path.size() >= 4 && path.substr(path.size() - 4) == ".wal") {
            return Status::IOError("injected WAL removal");
        }
        return delegate_.RemoveFile(path);
    }

    Status SyncDirectory(const std::string& directory) override {
        if (active_failure_ == FlushFailurePoint::kDirectorySync ||
            (active_failure_ == FlushFailurePoint::kManifestDirectorySync &&
             manifest_renamed_)) {
            return Status::IOError("injected directory sync");
        }
        return delegate_.SyncDirectory(directory);
    }

private:
    PosixFlushEnvironment delegate_;
    FlushFailurePoint active_failure_ = FlushFailurePoint::kNone;
    bool manifest_renamed_ = false;
};

}  // namespace minikv::test
