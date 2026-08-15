#include "minikv/database.hpp"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
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
        minikv::test::Expect(result != nullptr, "temporary database directory must exist");
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

std::size_t CountExtension(const std::string& directory, std::string extension) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

void TestThresholdGenerationsAndLayeredReads() {
    TemporaryDirectory directory("/tmp/minikv-v3-generations-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 12;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult open_result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &open_result
        ).ok(),
        "new V3 database must open"
    );
    minikv::WriteOptions invalid_write_options;
    invalid_write_options.sync_mode = static_cast<minikv::SyncMode>(99);
    minikv::test::Expect(
        database->Put("invalid", "value", invalid_write_options).code() ==
            minikv::StatusCode::kInvalidArgument &&
            database->last_sequence() == 0,
        "invalid sync mode must not allocate a sequence or touch the WAL"
    );
    minikv::test::Expect(
        database->Put("a", "1").ok(),
        "write below threshold must remain mutable"
    );
    minikv::test::Expect(
        database->flushed_table_count() == 0 &&
            database->mutable_generation() == 1,
        "threshold must not trigger early"
    );
    minikv::test::Expect(
        database->Put("b", "2").ok(),
        "write crossing threshold must flush in the foreground"
    );
    minikv::test::Expect(
        database->flushed_table_count() == 1 &&
            database->mutable_generation() == 2 &&
            !database->has_immutable(),
        "successful Flush must publish one table and activate the next generation"
    );
    minikv::test::Expect(
        CountExtension(directory.path(), ".sst") == 1 &&
            CountExtension(directory.path(), ".wal") == 1,
        "published generation must replace its WAL with one SST and a new WAL"
    );

    minikv::test::Expect(
        database->Put("a", "new").ok(),
        "new generation overwrite must succeed"
    );
    minikv::test::Expect(
        database->Get("a").value == "new",
        "Mutable record must override an older flushed record"
    );
    minikv::test::Expect(database->Flush().ok(), "explicit Flush must succeed");
    minikv::test::Expect(
        database->Delete("a").ok(),
        "tombstone in a newer generation must succeed"
    );
    minikv::test::Expect(
        database->Get("a").status.IsNotFound(),
        "new tombstone must stop lookup before older flushed values"
    );
    const std::uint64_t sequence_before_restart = database->last_sequence();
    database.reset();

    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &open_result
        ).ok(),
        "multi-generation database must reopen"
    );
    minikv::test::Expect(
        database->Get("a").status.IsNotFound() &&
            database->Get("b").value == "2",
        "reopen must preserve cross-table tombstone and value semantics"
    );
    minikv::test::Expect(
        database->last_sequence() == sequence_before_restart,
        "reopen must restore the sequence frontier across tables and WAL"
    );
}

void TestEveryFlushFailureRetainsRecoverableWal() {
    const minikv::test::FlushFailurePoint failures[] = {
        minikv::test::FlushFailurePoint::kCreate,
        minikv::test::FlushFailurePoint::kWrite,
        minikv::test::FlushFailurePoint::kFileSync,
        minikv::test::FlushFailurePoint::kRename,
        minikv::test::FlushFailurePoint::kDirectorySync,
        minikv::test::FlushFailurePoint::kRemoveWal,
    };

    for (const auto failure : failures) {
        TemporaryDirectory directory("/tmp/minikv-v3-failure-XXXXXX");
        if (!directory.valid()) {
            return;
        }
        minikv::Options options;
        options.memtable_size_limit = 1;
        auto environment =
            std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
        std::unique_ptr<minikv::Database> database;
        minikv::DatabaseOpenResult open_result;
        minikv::test::Expect(
            minikv::Database::OpenWithEnvironment(
                directory.path(),
                options,
                environment,
                &database,
                &open_result
            ).ok(),
            "fault-test database must open before injection"
        );
        environment->Arm(failure);
        minikv::test::Expect(
            database->Put("durable", "value").code() ==
                minikv::StatusCode::kIOError,
            "injected Flush failure must propagate"
        );
        minikv::test::Expect(
            database->has_immutable() &&
                database->Get("durable").value == "value",
            "failed Flush must retain a readable immutable generation"
        );
        minikv::test::Expect(
            CountExtension(directory.path(), ".wal") == 1,
            "old generation WAL must remain after every Flush failure"
        );
        database.reset();
        environment->Disarm();

        minikv::test::Expect(
            minikv::Database::Open(
                directory.path(),
                options,
                &database,
                &open_result
            ).ok(),
            "restart after injected Flush failure must succeed"
        );
        minikv::test::Expect(
            database->Get("durable").value == "value",
            "restart must recover the write from either WAL or published table"
        );
        minikv::test::Expect(
            database->Flush().ok(),
            "recovered generation must remain flushable"
        );
    }
}

void TestFailedFlushCanBeRetriedInProcess() {
    TemporaryDirectory directory("/tmp/minikv-v3-retry-XXXXXX");
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
            directory.path(),
            options,
            environment,
            &database,
            &result
        ).ok(),
        "retry database must open"
    );
    environment->Arm(minikv::test::FlushFailurePoint::kRename);
    minikv::test::Expect(
        database->Put("first", "value").code() ==
            minikv::StatusCode::kIOError,
        "first Flush must fail at rename"
    );
    environment->Disarm();
    minikv::test::Expect(
        database->Flush().ok(),
        "same-process Flush retry must publish the retained immutable generation"
    );
    minikv::test::Expect(
        database->Put("second", "value").ok(),
        "writes must continue in a new WAL generation after retry"
    );
    minikv::test::Expect(
        database->Get("first").value == "value" &&
            database->Get("second").value == "value",
        "retry must preserve both generations"
    );
}

class CrashAfterRenameEnvironment final : public minikv::FlushEnvironment {
public:
    void Arm() { armed_ = true; }

    minikv::Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<minikv::WritableFile>* output
    ) override {
        return delegate_.CreateTruncated(path, output);
    }

    minikv::Status Rename(
        const std::string& source,
        const std::string& destination
    ) override {
        const auto status = delegate_.Rename(source, destination);
        if (status.ok() && armed_) {
            ::kill(::getpid(), SIGKILL);
            _exit(4);
        }
        return status;
    }

    minikv::Status RemoveFile(const std::string& path) override {
        return delegate_.RemoveFile(path);
    }

    minikv::Status SyncDirectory(const std::string& directory) override {
        return delegate_.SyncDirectory(directory);
    }

private:
    minikv::PosixFlushEnvironment delegate_;
    bool armed_ = false;
};

void TestCrashAfterRenameBeforeDirectorySync() {
    TemporaryDirectory directory("/tmp/minikv-v3-crash-flush-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const pid_t child = ::fork();
    minikv::test::Expect(child >= 0, "Flush crash test must fork");
    if (child == 0) {
        minikv::Options options;
        options.memtable_size_limit = 1;
        auto environment = std::make_shared<CrashAfterRenameEnvironment>();
        std::unique_ptr<minikv::Database> database;
        minikv::DatabaseOpenResult result;
        if (!minikv::Database::OpenWithEnvironment(
                 directory.path(),
                 options,
                 environment,
                 &database,
                 &result
             ).ok()) {
            _exit(2);
        }
        environment->Arm();
        const auto status = database->Put("crash", "survives");
        (void)status;
        _exit(3);
    }
    if (child < 0) {
        return;
    }

    int wait_status = 0;
    minikv::test::Expect(
        ::waitpid(child, &wait_status, 0) == child,
        "Flush crash-test child must be collected"
    );
    minikv::test::Expect(
        WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGKILL,
        "child must die after table rename and before directory sync"
    );

    minikv::Options options;
    options.memtable_size_limit = 1;
    std::unique_ptr<minikv::Database> recovered;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &recovered,
            &result
        ).ok(),
        "database must reopen after process death during Flush"
    );
    minikv::test::Expect(
        recovered->Get("crash").value == "survives",
        "strict WAL must cover a crash at the table publication boundary"
    );
}

void TestRepeatedFlushRestartAgainstReferenceModel() {
    TemporaryDirectory directory("/tmp/minikv-v3-model-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 96;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &result
        ).ok(),
        "reference-model database must open"
    );

    std::map<std::string, std::optional<std::string>> reference;
    for (std::size_t operation = 0; operation < 1000; ++operation) {
        const std::string key = "key-" + std::to_string(operation % 23);
        if (operation % 7 == 0) {
            minikv::test::Expect(database->Delete(key).ok(), "model Delete must work");
            reference[key] = std::nullopt;
        } else {
            const std::string value = "value-" + std::to_string(operation);
            minikv::test::Expect(
                database->Put(key, value).ok(),
                "model Put must work"
            );
            reference[key] = value;
        }

        if (operation % 41 == 40) {
            database.reset();
            minikv::test::Expect(
                minikv::Database::Open(
                    directory.path(),
                    options,
                    &database,
                    &result
                ).ok(),
                "database must reopen repeatedly between Flushes"
            );
        }

        for (const auto& [expected_key, expected_value] : reference) {
            const auto actual = database->Get(expected_key);
            minikv::test::Expect(
                expected_value.has_value()
                    ? actual.found() && actual.value == *expected_value
                    : actual.status.IsNotFound(),
                "layered Get must match the reference model"
            );
        }
    }
    minikv::test::Expect(database->Flush().ok(), "final model Flush must succeed");
    database.reset();
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &result
        ).ok(),
        "fully flushed model database must reopen"
    );
    for (const auto& [key, value] : reference) {
        const auto actual = database->Get(key);
        minikv::test::Expect(
            value.has_value()
                ? actual.found() && actual.value == *value
                : actual.status.IsNotFound(),
            "final restart must match the reference model"
        );
    }
}

void TestOpenValidation() {
    minikv::Options options;
    options.memtable_size_limit = 0;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open("/tmp/unused", options, &database, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "zero MemTable threshold must be rejected"
    );
    minikv::test::Expect(
        minikv::Database::Open("/tmp/unused", {}, nullptr, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "null database output must be rejected"
    );
    options = {};
    options.bloom_false_positive_rate = 1.0;
    minikv::test::Expect(
        minikv::Database::Open("/tmp/unused", options, &database, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "invalid Bloom false-positive rate must be rejected"
    );
}

void TestCorruptPublishedTableBlocksOpen() {
    TemporaryDirectory directory("/tmp/minikv-v3-corrupt-table-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &result
        ).ok() && database->Put("key", "value").ok(),
        "corruption fixture must flush one table"
    );
    database.reset();

    fs::path table_path;
    for (const auto& entry : fs::directory_iterator(directory.path())) {
        if (entry.path().extension() == ".sst") {
            table_path = entry.path();
            break;
        }
    }
    minikv::test::Expect(!table_path.empty(), "corruption fixture table must exist");
    if (table_path.empty()) {
        return;
    }
    std::fstream table(
        table_path,
        std::ios::in | std::ios::out | std::ios::binary
    );
    table.seekg(-1, std::ios::end);
    char byte = 0;
    table.read(&byte, 1);
    byte ^= static_cast<char>(0x01);
    table.seekp(-1, std::ios::end);
    table.write(&byte, 1);
    table.close();
    minikv::test::Expect(table.good(), "corrupt table byte must be written");

    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(),
            options,
            &database,
            &result
        ).code() == minikv::StatusCode::kCorruption,
        "published table corruption must block database Open"
    );
    minikv::test::Expect(
        database == nullptr,
        "corrupt storage must never be exposed as an empty database"
    );
}

}  // namespace

int main() {
    TestThresholdGenerationsAndLayeredReads();
    TestEveryFlushFailureRetainsRecoverableWal();
    TestFailedFlushCanBeRetriedInProcess();
    TestCrashAfterRenameBeforeDirectorySync();
    TestRepeatedFlushRestartAgainstReferenceModel();
    TestOpenValidation();
    TestCorruptPublishedTableBlocksOpen();
    return minikv::test::Finish("database generations");
}
