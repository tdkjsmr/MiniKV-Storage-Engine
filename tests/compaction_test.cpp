#include "minikv/database.hpp"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "fault_injecting_flush_environment.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) : path_(pattern) {
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(
            result != nullptr,
            "temporary compaction directory must exist"
        );
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

bool OpenDatabase(
    const std::string& path,
    const minikv::Options& options,
    std::unique_ptr<minikv::Database>* database
) {
    minikv::DatabaseOpenResult result;
    return minikv::Database::Open(path, options, database, &result).ok();
}

bool OpenDatabase(
    const std::string& path,
    const minikv::Options& options,
    std::shared_ptr<minikv::FlushEnvironment> environment,
    std::unique_ptr<minikv::Database>* database
) {
    minikv::DatabaseOpenResult result;
    return minikv::Database::OpenWithEnvironment(
        path,
        options,
        std::move(environment),
        database,
        &result
    ).ok();
}

bool HasValue(
    const minikv::Database& database,
    std::string_view key,
    std::string_view value
) {
    const auto result = database.Get(key);
    return result.status.ok() && result.value == value;
}

void MakeTwoLevelZeroTables(minikv::Database* database) {
    minikv::test::Expect(
        database->Put("alpha", "old").ok() &&
            database->Put("deleted", "present").ok() &&
            database->Flush().ok(),
        "first L0 fixture generation must flush"
    );
    minikv::test::Expect(
        database->Put("alpha", "new").ok() &&
            database->Delete("deleted").ok() &&
            database->Put("omega", "value").ok() &&
            database->Flush().ok(),
        "second L0 fixture generation must flush"
    );
}

void ExpectFixtureSemantics(const minikv::Database& database) {
    minikv::test::Expect(
        HasValue(database, "alpha", "new") &&
            database.Get("deleted").status.IsNotFound() &&
            HasValue(database, "omega", "value"),
        "compaction must preserve latest-value and deletion semantics"
    );
}

void TestTombstoneRequiresBottommostProof() {
    TemporaryDirectory directory("/tmp/minikv-v7-tombstone-proof-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    minikv::MemTable older(options);
    minikv::MemTable newer(options);
    minikv::test::Expect(
        older.Put(1, "gone", "old").ok() && newer.Delete(2, "gone").ok(),
        "tombstone-proof input MemTables must build"
    );
    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::SSTableReader> older_table;
    std::unique_ptr<minikv::SSTableReader> newer_table;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(), 1, older, options, environment, &older_table
        ).ok() &&
            minikv::PublishTable(
                directory.path(), 2, newer, options, environment, &newer_table
            ).ok(),
        "tombstone-proof SSTables must publish"
    );
    const std::vector<minikv::CompactionInput> inputs = {
        {0, older_table.get()},
        {0, newer_table.get()},
    };
    std::vector<minikv::MemTable> outputs;
    minikv::CompactionBuildStats stats;
    minikv::test::Expect(
        minikv::BuildCompactionOutputs(
            inputs, options, false, &outputs, &stats
        ).ok() &&
            outputs.size() == 1 && outputs.front().Lookup("gone").deleted() &&
            stats.tombstones_dropped == 0,
        "a winning tombstone must remain without bottommost proof"
    );
    minikv::test::Expect(
        minikv::BuildCompactionOutputs(
            inputs, options, true, &outputs, &stats
        ).ok() && outputs.empty() && stats.tombstones_dropped == 1,
        "the same tombstone may be dropped after bottommost proof"
    );
}

void TestMergeTombstonesStatisticsAndRestart() {
    TemporaryDirectory directory("/tmp/minikv-v7-merge-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "V7 merge fixture must open"
    );

    minikv::test::Expect(
        database->Put("cycle", "first").ok() &&
            database->Put("deleted", "present").ok() &&
            database->Put("stable", "unchanged").ok() &&
            database->Flush().ok(),
        "first semantic generation must flush"
    );
    minikv::test::Expect(
        database->Delete("cycle").ok() &&
            database->Delete("deleted").ok() &&
            database->Put("duplicate", "old").ok() &&
            database->Flush().ok(),
        "second semantic generation must flush"
    );
    minikv::test::Expect(
        database->Put("cycle", "final").ok() &&
            database->Put("duplicate", "new").ok() &&
            database->Flush().ok(),
        "third semantic generation must flush"
    );
    minikv::test::Expect(
        database->level_table_count(0) == 3 &&
            database->level_table_count(1) == 0,
        "fixture must begin with three L0 tables"
    );

    minikv::CompactionStats stats;
    minikv::test::Expect(database->Compact(&stats).ok(), "V7 Compact must succeed");
    minikv::test::Expect(
        database->level_table_count(0) == 0 &&
            database->level_table_count(1) == 1 &&
            HasValue(*database, "cycle", "final") &&
            HasValue(*database, "duplicate", "new") &&
            HasValue(*database, "stable", "unchanged") &&
            database->Get("deleted").status.IsNotFound(),
        "heap merge must retain only the greatest Sequence per user key"
    );
    minikv::test::Expect(
        stats.input_files == 3 && stats.output_files == 1 &&
            stats.input_records == 8 && stats.output_records == 3 &&
            stats.duplicate_records_dropped == 4 &&
            stats.tombstones_dropped == 1 && stats.input_bytes > 0 &&
            stats.output_bytes > 0 &&
            stats.bytes_reclaimed == stats.input_bytes - stats.output_bytes &&
            database->last_compaction_statistics().input_files == 3,
        "compaction statistics must report records, bytes, and reclaimed space"
    );

    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "compacted database must reopen"
    );
    minikv::test::Expect(
        database->level_table_count(0) == 0 &&
            database->level_table_count(1) == 1 &&
            HasValue(*database, "cycle", "final") &&
            database->Get("deleted").status.IsNotFound(),
        "restart must preserve the compacted Version exactly"
    );
}

void TestRangeSelectionAndOutputSplitting() {
    {
        TemporaryDirectory directory("/tmp/minikv-v7-disjoint-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1024U * 1024U;
        std::unique_ptr<minikv::Database> database;
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, &database),
            "disjoint-range fixture must open"
        );
        minikv::test::Expect(
            database->Put("a", "one").ok() &&
                database->Put("c", "three").ok() && database->Flush().ok() &&
                database->Compact().ok(),
            "first disjoint L1 range must compact"
        );
        minikv::CompactionStats stats;
        minikv::test::Expect(
            database->Put("x", "twenty-four").ok() &&
                database->Put("z", "twenty-six").ok() && database->Flush().ok() &&
                database->Compact(&stats).ok(),
            "second disjoint L0 range must compact"
        );
        minikv::test::Expect(
            stats.input_files == 1 && database->level_table_count(1) == 2 &&
                HasValue(*database, "a", "one") &&
                HasValue(*database, "z", "twenty-six"),
            "non-overlapping L1 file must not be selected"
        );
    }

    {
        TemporaryDirectory directory("/tmp/minikv-v7-boundary-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1024U * 1024U;
        std::unique_ptr<minikv::Database> database;
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, &database),
            "equal-boundary fixture must open"
        );
        minikv::test::Expect(
            database->Put("m", "old").ok() && database->Flush().ok() &&
                database->Compact().ok(),
            "boundary L1 fixture must compact"
        );
        minikv::CompactionStats stats;
        minikv::test::Expect(
            database->Put("m", "new").ok() &&
                database->Put("z", "last").ok() && database->Flush().ok() &&
                database->Compact(&stats).ok(),
            "equal-boundary L0 range must compact"
        );
        minikv::test::Expect(
            stats.input_files == 2 && database->level_table_count(1) == 1 &&
                HasValue(*database, "m", "new"),
            "a range sharing one endpoint must select the overlapping L1 file"
        );
    }

    {
        TemporaryDirectory directory("/tmp/minikv-v7-split-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1024U * 1024U;
        options.compaction_output_size_limit = 96;
        std::unique_ptr<minikv::Database> database;
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, &database),
            "output-split fixture must open"
        );
        for (std::size_t index = 0; index < 20; ++index) {
            minikv::test::Expect(
                database->Put(
                    "key-" + std::to_string(index),
                    std::string(32, static_cast<char>('a' + index % 26U))
                ).ok(),
                "split fixture Put must succeed"
            );
        }
        minikv::test::Expect(database->Flush().ok(), "split fixture must flush");
        minikv::CompactionStats stats;
        minikv::test::Expect(
            database->Compact(&stats).ok() && stats.output_files > 1 &&
                database->level_table_count(1) == stats.output_files,
            "compaction must split sorted output at the configured target size"
        );
        for (std::size_t index = 0; index < 20; ++index) {
            minikv::test::Expect(
                database->Get("key-" + std::to_string(index)).status.ok(),
                "every split-output key must remain readable"
            );
        }
    }
}

void TestAllTombstonesProduceNoOutput() {
    TemporaryDirectory directory("/tmp/minikv-v7-empty-output-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "empty-output fixture must open"
    );
    minikv::test::Expect(
        database->Put("gone", "old").ok() && database->Flush().ok() &&
            database->Delete("gone").ok() && database->Flush().ok(),
        "empty-output fixture generations must flush"
    );
    minikv::CompactionStats stats;
    minikv::test::Expect(
        database->Compact(&stats).ok() && stats.input_files == 2 &&
            stats.output_files == 0 && stats.output_records == 0 &&
            stats.tombstones_dropped == 1 &&
            database->flushed_table_count() == 0 &&
            database->Get("gone").status.IsNotFound(),
        "bottommost tombstones may remove all selected tables without an output"
    );
    minikv::test::Expect(
        database->Put("after", "works").ok() &&
            HasValue(*database, "after", "works"),
        "deletion-only VersionEdit must keep the active WAL generation writable"
    );
    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database) &&
            database->Get("gone").status.IsNotFound() &&
            HasValue(*database, "after", "works"),
        "empty-output compaction must survive restart"
    );
}

void TestReferenceModelBeforeAfterAndRestart() {
    TemporaryDirectory directory("/tmp/minikv-v7-model-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    options.compaction_output_size_limit = 300;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "compaction reference-model fixture must open"
    );
    std::map<std::string, std::optional<std::string>> reference;
    for (std::size_t operation = 0; operation < 240; ++operation) {
        const std::string key = "model-" + std::to_string(operation % 29U);
        if (operation % 6U == 0) {
            minikv::test::Expect(database->Delete(key).ok(), "model Delete must work");
            reference[key] = std::nullopt;
        } else {
            const std::string value = "value-" + std::to_string(operation);
            minikv::test::Expect(database->Put(key, value).ok(), "model Put must work");
            reference[key] = value;
        }
        if (operation % 19U == 18U) {
            minikv::test::Expect(database->Flush().ok(), "model Flush must work");
        }
    }
    minikv::test::Expect(database->Flush().ok(), "final model Flush must work");

    const auto verify = [&reference](const minikv::Database& current) {
        for (const auto& [key, expected] : reference) {
            const auto result = current.Get(key);
            minikv::test::Expect(
                expected.has_value()
                    ? result.status.ok() && result.value == *expected
                    : result.status.IsNotFound(),
                "database must equal the reference model"
            );
        }
    };
    verify(*database);
    minikv::test::Expect(database->Compact().ok(), "model Compact must succeed");
    verify(*database);
    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "model database must reopen after compaction"
    );
    verify(*database);
}

void TestEveryPublicationFailureAndRetry() {
    const minikv::test::FlushFailurePoint failures[] = {
        minikv::test::FlushFailurePoint::kCreate,
        minikv::test::FlushFailurePoint::kWrite,
        minikv::test::FlushFailurePoint::kFileSync,
        minikv::test::FlushFailurePoint::kRename,
        minikv::test::FlushFailurePoint::kDirectorySync,
        minikv::test::FlushFailurePoint::kManifestCreate,
        minikv::test::FlushFailurePoint::kManifestWrite,
        minikv::test::FlushFailurePoint::kManifestFileSync,
        minikv::test::FlushFailurePoint::kManifestRename,
        minikv::test::FlushFailurePoint::kManifestDirectorySync,
        minikv::test::FlushFailurePoint::kRemoveCompactionInput,
        minikv::test::FlushFailurePoint::kRemoveWal,
    };

    for (const auto failure : failures) {
        TemporaryDirectory directory("/tmp/minikv-v7-failure-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1024U * 1024U;
        auto environment =
            std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
        std::unique_ptr<minikv::Database> database;
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, environment, &database),
            "compaction failure fixture must open"
        );
        MakeTwoLevelZeroTables(database.get());
        environment->Arm(failure);
        minikv::test::Expect(
            database->Compact().code() == minikv::StatusCode::kIOError,
            "every injected compaction publication failure must propagate"
        );
        ExpectFixtureSemantics(*database);
        environment->Disarm();
        minikv::test::Expect(
            database->Compact().ok(),
            "every failed compaction stage must be retryable in process"
        );
        ExpectFixtureSemantics(*database);
        minikv::test::Expect(
            database->level_table_count(0) == 0 &&
                database->level_table_count(1) == 1,
            "retry must leave one complete L1 Version"
        );
        database.reset();
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, &database),
            "database must reopen after a retried compaction failure"
        );
        ExpectFixtureSemantics(*database);
    }
}

enum class CompactionCrashPoint {
    kOutputFileSync,
    kOutputRename,
    kOutputDirectorySync,
    kManifestFileSync,
    kManifestRename,
    kManifestDirectorySync,
};

[[noreturn]] void CrashNow() {
    ::kill(::getpid(), SIGKILL);
    _exit(99);
}

class CrashingCompactionEnvironment final : public minikv::FlushEnvironment {
private:
    class CrashingFile final : public minikv::WritableFile {
    public:
        CrashingFile(
            std::unique_ptr<minikv::WritableFile> inner,
            CompactionCrashPoint* point,
            bool armed,
            bool manifest
        )
            : inner_(std::move(inner)),
              point_(point),
              armed_(armed),
              manifest_(manifest) {}

        minikv::WriteResult WriteSome(std::string_view data) override {
            return inner_->WriteSome(data);
        }

        minikv::Status Sync() override {
            const auto status = inner_->Sync();
            if (status.ok() && armed_ &&
                ((manifest_ && *point_ == CompactionCrashPoint::kManifestFileSync) ||
                 (!manifest_ && *point_ == CompactionCrashPoint::kOutputFileSync))) {
                CrashNow();
            }
            return status;
        }

        [[nodiscard]] std::string_view name() const noexcept override {
            return inner_->name();
        }

    private:
        std::unique_ptr<minikv::WritableFile> inner_;
        CompactionCrashPoint* point_;
        bool armed_ = false;
        bool manifest_ = false;
    };

public:
    void Arm(CompactionCrashPoint point) {
        point_ = point;
        armed_ = true;
        output_renamed_ = false;
        manifest_renamed_ = false;
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
        output->reset(new CrashingFile(
            std::move(inner),
            &point_,
            armed_,
            manifest
        ));
        return minikv::Status::Ok();
    }

    minikv::Status Rename(
        const std::string& source,
        const std::string& destination
    ) override {
        const auto status = delegate_.Rename(source, destination);
        if (!status.ok() || !armed_) {
            return status;
        }
        const bool manifest = destination.size() >= 8 &&
            destination.substr(destination.size() - 8) == "MANIFEST";
        if (manifest) {
            manifest_renamed_ = true;
            if (point_ == CompactionCrashPoint::kManifestRename) {
                CrashNow();
            }
        } else if (destination.size() >= 4 &&
                   destination.substr(destination.size() - 4) == ".sst") {
            output_renamed_ = true;
            if (point_ == CompactionCrashPoint::kOutputRename) {
                CrashNow();
            }
        }
        return status;
    }

    minikv::Status RemoveFile(const std::string& path) override {
        return delegate_.RemoveFile(path);
    }

    minikv::Status SyncDirectory(const std::string& directory) override {
        const auto status = delegate_.SyncDirectory(directory);
        if (!status.ok() || !armed_) {
            return status;
        }
        if (manifest_renamed_ &&
            point_ == CompactionCrashPoint::kManifestDirectorySync) {
            CrashNow();
        }
        if (output_renamed_ && !manifest_renamed_ &&
            point_ == CompactionCrashPoint::kOutputDirectorySync) {
            CrashNow();
        }
        return status;
    }

private:
    minikv::PosixFlushEnvironment delegate_;
    CompactionCrashPoint point_ = CompactionCrashPoint::kOutputFileSync;
    bool armed_ = false;
    bool output_renamed_ = false;
    bool manifest_renamed_ = false;
};

void TestCrashAtEveryCommitBoundary() {
    const CompactionCrashPoint points[] = {
        CompactionCrashPoint::kOutputFileSync,
        CompactionCrashPoint::kOutputRename,
        CompactionCrashPoint::kOutputDirectorySync,
        CompactionCrashPoint::kManifestFileSync,
        CompactionCrashPoint::kManifestRename,
        CompactionCrashPoint::kManifestDirectorySync,
    };
    for (const auto point : points) {
        TemporaryDirectory directory("/tmp/minikv-v7-crash-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        const pid_t child = ::fork();
        minikv::test::Expect(child >= 0, "compaction crash test must fork");
        if (child == 0) {
            minikv::Options options;
            options.memtable_size_limit = 1024U * 1024U;
            auto environment = std::make_shared<CrashingCompactionEnvironment>();
            std::unique_ptr<minikv::Database> database;
            if (!OpenDatabase(directory.path(), options, environment, &database)) {
                _exit(2);
            }
            MakeTwoLevelZeroTables(database.get());
            environment->Arm(point);
            const auto status = database->Compact();
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
            "child must die at the armed compaction commit boundary"
        );

        minikv::Options options;
        options.memtable_size_limit = 1024U * 1024U;
        std::unique_ptr<minikv::Database> recovered;
        minikv::test::Expect(
            OpenDatabase(directory.path(), options, &recovered),
            "database must reopen after a compaction SIGKILL"
        );
        ExpectFixtureSemantics(*recovered);
        minikv::test::Expect(
            recovered->Compact().ok(),
            "recovered old or new Version must remain compactable"
        );
        ExpectFixtureSemantics(*recovered);
    }
}

}  // namespace

int main() {
    TestTombstoneRequiresBottommostProof();
    TestMergeTombstonesStatisticsAndRestart();
    TestRangeSelectionAndOutputSplitting();
    TestAllTombstonesProduceNoOutput();
    TestReferenceModelBeforeAfterAndRestart();
    TestEveryPublicationFailureAndRetry();
    TestCrashAtEveryCommitBoundary();
    return minikv::test::Finish("V7 Compaction");
}
