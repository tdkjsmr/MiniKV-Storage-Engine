#include "minikv/database.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) {
        path_ = pattern;
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(
            result != nullptr,
            "temporary concurrency directory must exist"
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

bool HasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

class BlockingTableEnvironment final : public minikv::FlushEnvironment {
public:
    explicit BlockingTableEnvironment(std::size_t block_on_table = 1)
        : block_on_table_(block_on_table) {}

    class Writer final : public minikv::WritableFile {
    public:
        Writer(
            std::unique_ptr<minikv::WritableFile> delegate,
            BlockingTableEnvironment* owner,
            bool block
        )
            : delegate_(std::move(delegate)), owner_(owner), block_(block) {}

        minikv::WriteResult WriteSome(std::string_view bytes) override {
            if (block_) {
                owner_->BlockOnce();
                block_ = false;
            }
            return delegate_->WriteSome(bytes);
        }

        minikv::Status Sync() override { return delegate_->Sync(); }
        [[nodiscard]] std::string_view name() const noexcept override {
            return delegate_->name();
        }

    private:
        std::unique_ptr<minikv::WritableFile> delegate_;
        BlockingTableEnvironment* owner_ = nullptr;
        bool block_ = false;
    };

    minikv::Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<minikv::WritableFile>* output
    ) override {
        std::unique_ptr<minikv::WritableFile> delegate;
        const auto status = delegate_.CreateTruncated(path, &delegate);
        if (!status.ok()) {
            return status;
        }
        const bool is_table = HasSuffix(path, ".sst.tmp");
        if (is_table) {
            ++table_files_created_;
        }
        output->reset(new Writer(
            std::move(delegate),
            this,
            is_table && table_files_created_ == block_on_table_
        ));
        return minikv::Status::Ok();
    }

    minikv::Status Rename(
        const std::string& from,
        const std::string& to
    ) override {
        return delegate_.Rename(from, to);
    }

    minikv::Status RemoveFile(const std::string& path) override {
        return delegate_.RemoveFile(path);
    }

    minikv::Status SyncDirectory(const std::string& directory) override {
        return delegate_.SyncDirectory(directory);
    }

    void WaitUntilBlocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return blocked_; });
    }

    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    void BlockOnce() {
        std::unique_lock<std::mutex> lock(mutex_);
        blocked_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    minikv::PosixFlushEnvironment delegate_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool blocked_ = false;
    bool released_ = false;
    std::size_t block_on_table_ = 1;
    std::size_t table_files_created_ = 0;
};

class FailFirstTableRenameEnvironment final : public minikv::FlushEnvironment {
public:
    minikv::Status CreateTruncated(
        const std::string& path,
        std::unique_ptr<minikv::WritableFile>* output
    ) override {
        return delegate_.CreateTruncated(path, output);
    }

    minikv::Status Rename(
        const std::string& from,
        const std::string& to
    ) override {
        if (HasSuffix(to, ".sst") && fail_.exchange(false)) {
            return minikv::Status::IOError(
                "injected first background table rename failure"
            );
        }
        return delegate_.Rename(from, to);
    }

    minikv::Status RemoveFile(const std::string& path) override {
        return delegate_.RemoveFile(path);
    }

    minikv::Status SyncDirectory(const std::string& directory) override {
        return delegate_.SyncDirectory(directory);
    }

private:
    minikv::PosixFlushEnvironment delegate_;
    std::atomic<bool> fail_{true};
};

bool OpenDatabase(
    const std::string& directory,
    const minikv::Options& options,
    std::unique_ptr<minikv::Database>* database,
    minikv::DatabaseOpenResult* result
) {
    return minikv::Database::Open(directory, options, database, result).ok();
}

void TestBackgroundFlushAllowsNextGenerationReadsAndWrites() {
    TemporaryDirectory directory("/tmp/minikv-v9-overlap-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.background_maintenance = true;
    options.memtable_size_limit = 1;
    options.level0_compaction_trigger = 100;
    auto environment = std::make_shared<BlockingTableEnvironment>();
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::OpenWithEnvironment(
            directory.path(), options, environment, &database, &result
        ).ok(),
        "background-overlap database must open"
    );
    minikv::test::Expect(
        database->Put("first", "one").ok(),
        "threshold write must schedule background Flush"
    );
    environment->WaitUntilBlocked();
    minikv::test::Expect(
        database->Put("second", "two").ok() &&
            database->Get("first").value == "one" &&
            database->Get("second").value == "two",
        "next WAL generation writes and reads must progress during table build"
    );
    environment->Release();
    minikv::test::Expect(
        database->WaitForBackgroundWork().ok(),
        "released background Flush must complete"
    );
    const auto stats = database->background_statistics();
    minikv::test::Expect(
        stats.flushes_completed == 1 && stats.failures == 0 &&
            database->flushed_table_count() == 1,
        "background statistics must report one completed Flush"
    );
    minikv::test::Expect(
        database->Close().ok() && database->Close().ok() &&
            database->state() == minikv::DatabaseState::kClosed,
        "Close must flush the remaining Mutable and be idempotent"
    );
    minikv::test::Expect(
        database->Put("closed", "no").code() == minikv::StatusCode::kClosed &&
            database->Get("first").status.code() ==
                minikv::StatusCode::kClosed &&
            database->ScanPrefix({}, 10).status.code() ==
                minikv::StatusCode::kClosed,
        "new operations after Close must fail explicitly"
    );

    database.reset();
    options.background_maintenance = false;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database, &result) &&
            database->Get("first").value == "one" &&
            database->Get("second").value == "two",
        "normal Close must preserve both WAL generations after reopen"
    );
}

void TestTwoWalCrashRecovery() {
    TemporaryDirectory directory("/tmp/minikv-v9-two-wal-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    int ready_pipe[2] = {-1, -1};
    minikv::test::Expect(::pipe(ready_pipe) == 0, "two-WAL ready pipe must open");
    const pid_t child = ::fork();
    minikv::test::Expect(child >= 0, "two-WAL recovery test must fork");
    if (child == 0) {
        ::close(ready_pipe[0]);
        minikv::Options options;
        options.background_maintenance = true;
        options.memtable_size_limit = 1;
        auto environment = std::make_shared<BlockingTableEnvironment>();
        std::unique_ptr<minikv::Database> database;
        minikv::DatabaseOpenResult result;
        if (!minikv::Database::OpenWithEnvironment(
                directory.path(), options, environment, &database, &result
            ).ok() ||
            !database->Put("old-generation", "old").ok()) {
            ::_exit(2);
        }
        environment->WaitUntilBlocked();
        if (!database->Put("new-generation", "new").ok()) {
            ::_exit(3);
        }
        const char ready = 'R';
        if (::write(ready_pipe[1], &ready, 1) != 1) {
            ::_exit(4);
        }
        for (;;) {
            ::pause();
        }
    }
    if (child < 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        return;
    }
    ::close(ready_pipe[1]);
    char ready = 0;
    const ssize_t received = ::read(ready_pipe[0], &ready, 1);
    ::close(ready_pipe[0]);
    minikv::test::Expect(
        received == 1 && ready == 'R',
        "child must persist two consecutive WAL generations"
    );
    static_cast<void>(::kill(child, SIGKILL));
    int child_status = 0;
    minikv::test::Expect(
        ::waitpid(child, &child_status, 0) == child &&
            WIFSIGNALED(child_status),
        "two-WAL child must be collected after SIGKILL"
    );

    minikv::Options options;
    std::unique_ptr<minikv::Database> recovered;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &recovered, &result) &&
            result.wals_recovered == 2 && recovered->has_immutable() &&
            recovered->Get("old-generation").value == "old" &&
            recovered->Get("new-generation").value == "new",
        "Open must recover one Immutable and one Mutable WAL in sequence order"
    );
}

void TestBackgroundErrorIsObservableAndRetryable() {
    TemporaryDirectory directory("/tmp/minikv-v9-error-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.background_maintenance = true;
    options.memtable_size_limit = 1;
    auto environment = std::make_shared<FailFirstTableRenameEnvironment>();
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::OpenWithEnvironment(
            directory.path(), options, environment, &database, &result
        ).ok() &&
            database->Put("retry", "visible").ok(),
        "background-error fixture must schedule a committed write"
    );
    const auto failed = database->WaitForBackgroundWork();
    minikv::test::Expect(
        failed.code() == minikv::StatusCode::kIOError &&
            database->background_status().code() ==
                minikv::StatusCode::kIOError &&
            database->Get("retry").value == "visible" &&
            database->background_statistics().failures == 1 &&
            database->Put("blocked", "until-retry").code() ==
                minikv::StatusCode::kIOError,
        "background failure must be observable without hiding committed data"
    );
    minikv::test::Expect(
        database->Flush().ok() && database->background_status().ok() &&
            !database->has_immutable(),
        "manual Flush must retry and clear a transient background error"
    );
}

void TestPublishedFlushKeepsNextWalSequenceRecoverable() {
    TemporaryDirectory directory("/tmp/minikv-v9-frontier-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    int ready_pipe[2] = {-1, -1};
    minikv::test::Expect(
        ::pipe(ready_pipe) == 0,
        "published-frontier ready pipe must open"
    );
    const pid_t child = ::fork();
    minikv::test::Expect(child >= 0, "published-frontier test must fork");
    if (child == 0) {
        ::close(ready_pipe[0]);
        minikv::Options options;
        options.background_maintenance = true;
        options.memtable_size_limit = 1;
        options.level0_compaction_trigger = 100;
        auto environment = std::make_shared<BlockingTableEnvironment>(2);
        std::unique_ptr<minikv::Database> database;
        minikv::DatabaseOpenResult result;
        if (!minikv::Database::OpenWithEnvironment(
                directory.path(), options, environment, &database, &result
            ).ok() ||
            !database->Put("published", "first").ok() ||
            !database->WaitForBackgroundWork().ok() ||
            !database->Put("next-wal", "second").ok()) {
            ::_exit(2);
        }
        environment->WaitUntilBlocked();
        const char ready = 'R';
        if (::write(ready_pipe[1], &ready, 1) != 1) {
            ::_exit(3);
        }
        for (;;) {
            ::pause();
        }
    }
    if (child < 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        return;
    }
    ::close(ready_pipe[1]);
    char ready = 0;
    const ssize_t received = ::read(ready_pipe[0], &ready, 1);
    ::close(ready_pipe[0]);
    minikv::test::Expect(
        received == 1 && ready == 'R',
        "child must publish the old table and retain the next WAL"
    );
    static_cast<void>(::kill(child, SIGKILL));
    int child_status = 0;
    minikv::test::Expect(
        ::waitpid(child, &child_status, 0) == child &&
            WIFSIGNALED(child_status),
        "published-frontier child must be collected"
    );

    minikv::Options options;
    std::unique_ptr<minikv::Database> recovered;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &recovered, &result) &&
            result.tables_loaded == 1 && result.wals_recovered == 2 &&
            recovered->has_immutable() &&
            recovered->Get("published").value == "first" &&
            recovered->Get("next-wal").value == "second",
        "Manifest frontier must not claim Sequence values still owned by next WAL"
    );
}

void TestCloseFailureIsStableAndRecoverable() {
    TemporaryDirectory directory("/tmp/minikv-v9-close-error-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    auto environment = std::make_shared<FailFirstTableRenameEnvironment>();
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        minikv::Database::OpenWithEnvironment(
            directory.path(), options, environment, &database, &result
        ).ok() &&
            database->Put("close-error", "recoverable").ok(),
        "Close-error fixture must commit a Mutable record"
    );
    const auto first_close = database->Close();
    const auto second_close = database->Close();
    minikv::test::Expect(
        first_close.code() == minikv::StatusCode::kIOError &&
            second_close.code() == minikv::StatusCode::kIOError &&
            second_close.ToString() == first_close.ToString() &&
            database->state() == minikv::DatabaseState::kClosed,
        "failed and repeated Close must return the same stable result"
    );
    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database, &result) &&
            database->Get("close-error").value == "recoverable",
        "failed Close must leave committed data recoverable on Open"
    );
}

void TestAutomaticCompactionAndConcurrentClose() {
    TemporaryDirectory directory("/tmp/minikv-v9-close-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.background_maintenance = true;
    options.memtable_size_limit = 1;
    options.level0_compaction_trigger = 2;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult result;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database, &result),
        "automatic-compaction database must open"
    );
    minikv::test::Expect(
        database->Put("compact-a", "one").ok() &&
            database->WaitForBackgroundWork().ok() &&
            database->Put("compact-b", "two").ok() &&
            database->WaitForBackgroundWork().ok(),
        "two background Flushes must complete"
    );
    const auto maintenance = database->background_statistics();
    minikv::test::Expect(
        maintenance.flushes_completed == 2 &&
            maintenance.compactions_completed == 1 &&
            database->level_table_count(0) == 0 &&
            database->level_table_count(1) == 1,
        "L0 trigger must run one automatic background Compaction"
    );

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> unexpected{0};
    std::vector<std::thread> readers;
    for (std::size_t reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const auto get = database->Get("compact-a");
                if (!get.status.ok() &&
                    get.status.code() != minikv::StatusCode::kClosed) {
                    ++unexpected;
                }
                const auto scan = database->ScanPrefix("compact-", 10);
                if (!scan.status.ok() &&
                    scan.status.code() != minikv::StatusCode::kClosed) {
                    ++unexpected;
                }
            }
        });
    }
    std::thread writer([&] {
        for (std::size_t index = 0; index < 200; ++index) {
            const auto status = database->Put(
                "live-" + std::to_string(index % 7U),
                std::to_string(index)
            );
            if (status.code() == minikv::StatusCode::kClosed) {
                break;
            }
            if (!status.ok()) {
                ++unexpected;
                break;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    minikv::Status first_close;
    minikv::Status second_close;
    std::thread closer_one([&] { first_close = database->Close(); });
    std::thread closer_two([&] { second_close = database->Close(); });
    closer_one.join();
    closer_two.join();
    stop = true;
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }
    minikv::test::Expect(
        first_close.ok() && second_close.ok() && unexpected == 0 &&
            database->state() == minikv::DatabaseState::kClosed,
        "concurrent readers, one serialized writer, and repeated Close must finish"
    );
}

}  // namespace

int main() {
    TestBackgroundFlushAllowsNextGenerationReadsAndWrites();
    TestTwoWalCrashRecovery();
    TestBackgroundErrorIsObservableAndRetryable();
    TestPublishedFlushKeepsNextWalSequenceRecoverable();
    TestCloseFailureIsStableAndRecoverable();
    TestAutomaticCompactionAndConcurrentClose();
    return minikv::test::Finish("V9 Background Concurrency");
}
