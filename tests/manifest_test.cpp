#include "minikv/database.hpp"
#include "minikv/flush.hpp"
#include "minikv/manifest.hpp"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fault_injecting_flush_environment.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) {
        path_ = pattern;
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(result != nullptr, "temporary directory must exist");
        valid_ = result != nullptr;
    }

    ~TemporaryDirectory() {
        if (valid_) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

bool ReadFile(const fs::path& path, std::string* bytes) {
    if (bytes == nullptr) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    bytes->assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
    return input.good() || input.eof();
}

std::size_t CountExtension(const std::string& directory, std::string extension) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

minikv::VersionTable SampleTable(std::uint64_t file_number) {
    minikv::VersionTable table;
    table.level = 0;
    table.metadata.generation = file_number;
    table.metadata.file_size = 4096;
    table.metadata.record_count = 3;
    table.metadata.minimum_sequence = 4;
    table.metadata.maximum_sequence = 7;
    table.metadata.minimum_key = "alpha";
    table.metadata.maximum_key = "omega";
    return table;
}

minikv::Version SampleVersion() {
    const auto initial = minikv::Version::NewDatabase();
    minikv::VersionEdit edit;
    edit.added_tables.push_back(SampleTable(1));
    edit.next_file_number = 2;
    edit.last_sequence = 7;
    minikv::Version version;
    minikv::test::Expect(
        initial.Apply(edit, {}, &version).ok(),
        "sample VersionEdit must apply"
    );
    return version;
}

void TestVersionEditAndManifestRoundTrip() {
    const auto version = SampleVersion();
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeManifest(version, {}, &encoded).ok(),
        "Manifest must encode"
    );
    minikv::test::Expect(
        encoded.size() > minikv::kManifestHeaderSize &&
            encoded.substr(0, 4) == "MKMF" &&
            static_cast<std::uint8_t>(encoded[4]) ==
                minikv::kManifestFormatVersion &&
            static_cast<std::uint8_t>(encoded[5]) ==
                minikv::kStorageFormatVersion,
        "Manifest golden identity must be stable"
    );

    minikv::Version decoded;
    minikv::test::Expect(
        minikv::DecodeManifest(encoded, {}, &decoded).ok() &&
            decoded.id() == version.id() &&
            decoded.next_file_number() == 2 &&
            decoded.last_sequence() == 7 && decoded.tables().size() == 1 &&
            decoded.tables().front().metadata.minimum_key == "alpha",
        "Manifest must preserve the complete Version snapshot"
    );

    minikv::VersionEdit replacement;
    replacement.deleted_file_numbers.push_back(1);
    replacement.added_tables.push_back(SampleTable(2));
    replacement.added_tables.front().metadata.minimum_sequence = 8;
    replacement.added_tables.front().metadata.maximum_sequence = 9;
    replacement.next_file_number = 3;
    replacement.last_sequence = 9;
    minikv::Version next;
    minikv::test::Expect(
        decoded.Apply(replacement, {}, &next).ok() && next.id() == 3 &&
            next.tables().size() == 1 &&
            next.tables().front().metadata.generation == 2,
        "one VersionEdit must atomically add and delete files"
    );
}

void TestManifestCorruptionAndVersionRejection() {
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeManifest(SampleVersion(), {}, &encoded).ok(),
        "corruption fixture must encode"
    );
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        std::string damaged = encoded;
        damaged[index] ^= static_cast<char>(0x01);
        minikv::Version decoded;
        minikv::test::Expect(
            !minikv::DecodeManifest(damaged, {}, &decoded).ok(),
            "every one-byte Manifest mutation must be rejected"
        );
    }

    std::string unsupported = encoded;
    unsupported[5] = static_cast<char>(minikv::kStorageFormatVersion + 1);
    minikv::Version decoded;
    minikv::test::Expect(
        minikv::DecodeManifest(unsupported, {}, &decoded).code() ==
            minikv::StatusCode::kVersionMismatch,
        "unsupported storage-format golden file must report VersionMismatch"
    );

    const auto version = SampleVersion();
    minikv::VersionEdit unsupported_level;
    auto level_two = SampleTable(2);
    level_two.level = 2;
    unsupported_level.added_tables.push_back(level_two);
    unsupported_level.next_file_number = 3;
    minikv::Version rejected;
    minikv::test::Expect(
        !version.Apply(unsupported_level, {}, &rejected).ok(),
        "V7 must reject levels outside L0 and L1"
    );

    minikv::VersionEdit overlapping_level_one;
    auto left = SampleTable(2);
    left.level = 1;
    left.metadata.maximum_key = "middle";
    auto right = SampleTable(3);
    right.level = 1;
    right.metadata.minimum_key = "middle";
    right.metadata.maximum_key = "zulu";
    overlapping_level_one.added_tables.push_back(left);
    overlapping_level_one.added_tables.push_back(right);
    overlapping_level_one.next_file_number = 4;
    minikv::test::Expect(
        !version.Apply(overlapping_level_one, {}, &rejected).ok(),
        "L1 ranges sharing a boundary key must be rejected as overlapping"
    );
}

void TestManifestIsLiveSetAndOrphansAreRemoved() {
    TemporaryDirectory directory("/tmp/minikv-v5-live-set-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(directory.path(), options, &database, &result).ok() &&
            result.storage_format_version == minikv::kStorageFormatVersion &&
            database->Put("live", "value").ok() && database->version_id() == 2,
        "a Flush must publish Version 2"
    );
    database.reset();

    const fs::path orphan = fs::path(directory.path()) /
        minikv::GenerationFileName(999, "sst");
    const fs::path table_temp = fs::path(directory.path()) / "unused.sst.tmp";
    const fs::path manifest_temp = fs::path(directory.path()) / "MANIFEST.tmp";
    minikv::test::Expect(
        WriteFile(orphan, "orphan") && WriteFile(table_temp, "temporary") &&
            WriteFile(manifest_temp, "temporary"),
        "orphan fixtures must be created"
    );
    minikv::test::Expect(
        minikv::Database::Open(directory.path(), options, &database, &result).ok() &&
            result.orphan_files_removed == 3 &&
            database->Get("live").value == "value" && !fs::exists(orphan) &&
            !fs::exists(table_temp) && !fs::exists(manifest_temp),
        "Open must load only MANIFEST tables and remove unreferenced files"
    );
}

void TestMissingReferenceAndLegacyDirectoryAreRejected() {
    TemporaryDirectory missing("/tmp/minikv-v5-missing-XXXXXX");
    if (!missing.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(missing.path(), options, &database, &result).ok() &&
            database->Put("key", "value").ok(),
        "missing-table fixture must flush"
    );
    database.reset();
    for (const auto& entry : fs::directory_iterator(missing.path())) {
        if (entry.path().extension() == ".sst") {
            fs::remove(entry.path());
        }
    }
    minikv::test::Expect(
        minikv::Database::Open(missing.path(), options, &database, &result).code() ==
            minikv::StatusCode::kCorruption && database == nullptr,
        "a missing MANIFEST table must block Open"
    );

    TemporaryDirectory legacy("/tmp/minikv-v5-legacy-XXXXXX");
    if (!legacy.valid()) {
        return;
    }
    const fs::path legacy_wal = fs::path(legacy.path()) /
        minikv::GenerationFileName(1, "wal");
    minikv::test::Expect(WriteFile(legacy_wal, "legacy"), "legacy WAL must exist");
    minikv::test::Expect(
        minikv::Database::Open(legacy.path(), {}, &database, &result).code() ==
            minikv::StatusCode::kVersionMismatch && database == nullptr,
        "legacy data without a MANIFEST must not be guessed or rewritten"
    );
}

void TestDirectoryFormatMismatchAndExclusiveLock() {
    TemporaryDirectory directory("/tmp/minikv-v5-lock-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    std::unique_ptr<minikv::Database> first;
    std::unique_ptr<minikv::Database> second;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(directory.path(), {}, &first, &result).ok(),
        "first database instance must acquire LOCK"
    );
    minikv::test::Expect(
        minikv::Database::Open(directory.path(), {}, &second, &result).code() ==
            minikv::StatusCode::kIOError && second == nullptr,
        "a second in-process instance must not acquire the directory LOCK"
    );

    const pid_t child = ::fork();
    minikv::test::Expect(child >= 0, "lock test must fork");
    if (child == 0) {
        std::unique_ptr<minikv::Database> child_database;
        minikv::DatabaseOpenResult child_result;
        const auto status = minikv::Database::Open(
            directory.path(), {}, &child_database, &child_result
        );
        _exit(!status.ok() && child_database == nullptr ? 0 : 1);
    }
    if (child > 0) {
        int wait_status = 0;
        minikv::test::Expect(
            ::waitpid(child, &wait_status, 0) == child &&
                WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0,
            "a second process must not acquire the directory LOCK"
        );
    }
    first.reset();
    minikv::test::Expect(
        minikv::Database::Open(directory.path(), {}, &second, &result).ok(),
        "LOCK must be released when the owning Database is destroyed"
    );
    second.reset();

    std::string manifest;
    const fs::path manifest_path = fs::path(directory.path()) / "MANIFEST";
    minikv::test::Expect(ReadFile(manifest_path, &manifest), "Manifest must be readable");
    if (manifest.size() >= minikv::kManifestHeaderSize) {
        manifest[5] = static_cast<char>(minikv::kStorageFormatVersion + 1);
        minikv::test::Expect(
            WriteFile(manifest_path, manifest),
            "unsupported Manifest fixture must be written"
        );
        minikv::test::Expect(
            minikv::Database::Open(
                directory.path(), {}, &second, &result
            ).code() == minikv::StatusCode::kVersionMismatch && second == nullptr,
            "Open must explicitly reject an unsupported directory format"
        );
    }
}

void TestEveryManifestPublicationFailureRecovers() {
    const minikv::test::FlushFailurePoint failures[] = {
        minikv::test::FlushFailurePoint::kManifestCreate,
        minikv::test::FlushFailurePoint::kManifestWrite,
        minikv::test::FlushFailurePoint::kManifestFileSync,
        minikv::test::FlushFailurePoint::kManifestRename,
        minikv::test::FlushFailurePoint::kManifestDirectorySync,
    };
    for (const auto failure : failures) {
        TemporaryDirectory directory("/tmp/minikv-v5-manifest-fail-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1;
        auto environment =
            std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
        std::unique_ptr<minikv::Database> database;
        minikv::DatabaseOpenResult result;
        minikv::test::Expect(
            minikv::Database::OpenWithEnvironment(
                directory.path(), options, environment, &database, &result
            ).ok(),
            "Manifest fault fixture must open"
        );
        environment->Arm(failure);
        minikv::test::Expect(
            database->Put("durable", "value").code() ==
                minikv::StatusCode::kIOError && database->has_immutable() &&
                database->Get("durable").value == "value" &&
                CountExtension(directory.path(), ".wal") == 1,
            "every Manifest publication failure must retain the old WAL"
        );
        database.reset();
        environment->Disarm();
        minikv::test::Expect(
            minikv::Database::Open(
                directory.path(), options, &database, &result
            ).ok() && database->Get("durable").value == "value",
            "restart must expose either the old WAL Version or new table Version"
        );
    }
}

enum class ManifestCrashPoint {
    kAfterFileSync,
    kAfterRename,
    kAfterDirectorySync,
};

class CrashingManifestEnvironment final : public minikv::FlushEnvironment {
private:
    class CrashingFile final : public minikv::WritableFile {
    public:
        CrashingFile(
            std::unique_ptr<minikv::WritableFile> inner,
            ManifestCrashPoint* point,
            bool manifest
        )
            : inner_(std::move(inner)), point_(point), manifest_(manifest) {}

        minikv::WriteResult WriteSome(std::string_view data) override {
            return inner_->WriteSome(data);
        }

        minikv::Status Sync() override {
            const auto status = inner_->Sync();
            if (status.ok() && manifest_ &&
                *point_ == ManifestCrashPoint::kAfterFileSync) {
                Crash();
            }
            return status;
        }

        [[nodiscard]] std::string_view name() const noexcept override {
            return inner_->name();
        }

    private:
        [[noreturn]] static void Crash() {
            ::kill(::getpid(), SIGKILL);
            _exit(99);
        }

        std::unique_ptr<minikv::WritableFile> inner_;
        ManifestCrashPoint* point_;
        bool manifest_ = false;
    };

public:
    void Arm(ManifestCrashPoint point) {
        point_ = point;
        armed_ = true;
    }

    minikv::Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<minikv::WritableFile>* output
    ) override {
        std::unique_ptr<minikv::WritableFile> inner;
        const auto status = delegate_.CreateTruncated(path, &inner);
        if (!status.ok()) {
            return status;
        }
        const bool manifest = path.size() >= 12 &&
            path.substr(path.size() - 12) == "MANIFEST.tmp";
        output->reset(new CrashingFile(std::move(inner), &point_, armed_ && manifest));
        return minikv::Status::Ok();
    }

    minikv::Status Rename(
        const std::string& source,
        const std::string& destination
    ) override {
        const auto status = delegate_.Rename(source, destination);
        const bool manifest = destination.size() >= 8 &&
            destination.substr(destination.size() - 8) == "MANIFEST";
        if (status.ok() && armed_ && manifest) {
            manifest_renamed_ = true;
            if (point_ == ManifestCrashPoint::kAfterRename) {
                Crash();
            }
        }
        return status;
    }

    minikv::Status RemoveFile(const std::string& path) override {
        return delegate_.RemoveFile(path);
    }

    minikv::Status SyncDirectory(const std::string& directory) override {
        const auto status = delegate_.SyncDirectory(directory);
        if (status.ok() && armed_ && manifest_renamed_ &&
            point_ == ManifestCrashPoint::kAfterDirectorySync) {
            Crash();
        }
        return status;
    }

private:
    [[noreturn]] static void Crash() {
        ::kill(::getpid(), SIGKILL);
        _exit(98);
    }

    minikv::PosixFlushEnvironment delegate_;
    ManifestCrashPoint point_ = ManifestCrashPoint::kAfterFileSync;
    bool armed_ = false;
    bool manifest_renamed_ = false;
};

void TestCrashAtEveryManifestCommitBoundary() {
    const ManifestCrashPoint points[] = {
        ManifestCrashPoint::kAfterFileSync,
        ManifestCrashPoint::kAfterRename,
        ManifestCrashPoint::kAfterDirectorySync,
    };
    for (const auto point : points) {
        TemporaryDirectory directory("/tmp/minikv-v5-manifest-crash-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        const pid_t child = ::fork();
        minikv::test::Expect(child >= 0, "Manifest crash test must fork");
        if (child == 0) {
            minikv::Options options;
            options.memtable_size_limit = 1;
            auto environment = std::make_shared<CrashingManifestEnvironment>();
            std::unique_ptr<minikv::Database> database;
            minikv::DatabaseOpenResult result;
            if (!minikv::Database::OpenWithEnvironment(
                    directory.path(), options, environment, &database, &result
                ).ok()) {
                _exit(2);
            }
            environment->Arm(point);
            const auto status = database->Put("crash", "durable");
            (void)status;
            _exit(3);
        }
        if (child < 0) {
            return;
        }
        int wait_status = 0;
        minikv::test::Expect(
            ::waitpid(child, &wait_status, 0) == child &&
                WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGKILL,
            "child must die exactly at the armed MANIFEST boundary"
        );

        minikv::Options options;
        options.memtable_size_limit = 1;
        std::unique_ptr<minikv::Database> recovered;
        minikv::DatabaseOpenResult result;
        minikv::test::Expect(
            minikv::Database::Open(
                directory.path(), options, &recovered, &result
            ).ok() && recovered->Get("crash").value == "durable",
            "restart must expose a complete old or new Version after SIGKILL"
        );
    }
}

}  // namespace

int main() {
    TestVersionEditAndManifestRoundTrip();
    TestManifestCorruptionAndVersionRejection();
    TestManifestIsLiveSetAndOrphansAreRemoved();
    TestMissingReferenceAndLegacyDirectoryAreRejected();
    TestDirectoryFormatMismatchAndExclusiveLock();
    TestEveryManifestPublicationFailureRecovers();
    TestCrashAtEveryManifestCommitBoundary();
    return minikv::test::Finish("V5 Manifest and Version");
}
