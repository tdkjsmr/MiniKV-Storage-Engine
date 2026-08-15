#include "minikv/database.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kModelSeed = 0x5631304D4F44454CULL;
constexpr std::uint64_t kCrashSeed = 0x5631304352415348ULL;
constexpr std::size_t kModelOperations = 20000;
constexpr std::size_t kCrashRounds = 8;
constexpr std::size_t kConcurrentWriters = 4;
constexpr std::size_t kWritesPerThread = 400;

using ReferenceModel = std::map<std::string, std::optional<std::string>>;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) {
        path_ = pattern;
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(
            result != nullptr,
            "temporary reliability directory must exist"
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

std::string NumberedKey(std::string_view prefix, std::size_t number) {
    char digits[24] = {};
    const int written = std::snprintf(
        digits,
        sizeof(digits),
        "%06zu",
        number
    );
    if (written <= 0) {
        return std::string(prefix);
    }
    return std::string(prefix) + digits;
}

minikv::WriteOptions AsyncWrite() {
    minikv::WriteOptions options;
    options.sync_mode = minikv::SyncMode::kAsync;
    return options;
}

bool OpenDatabase(
    const std::string& directory,
    const minikv::Options& options,
    std::unique_ptr<minikv::Database>* database
) {
    minikv::DatabaseOpenResult result;
    return minikv::Database::Open(
        directory,
        options,
        database,
        &result
    ).ok();
}

std::vector<std::pair<std::string, std::string>> VisibleEntries(
    const ReferenceModel& reference
) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& [key, value] : reference) {
        if (value.has_value()) {
            entries.emplace_back(key, *value);
        }
    }
    return entries;
}

bool VerifyDatabase(
    const minikv::Database& database,
    const ReferenceModel& reference
) {
    for (const auto& [key, expected] : reference) {
        const auto actual = database.Get(key);
        if (expected.has_value()) {
            if (!actual.status.ok() || actual.value != *expected) {
                return false;
            }
        } else if (!actual.status.IsNotFound()) {
            return false;
        }
    }

    std::vector<std::pair<std::string, std::string>> actual_entries;
    std::string token;
    do {
        const auto page = database.LoadAll(73, token);
        if (!page.status.ok()) {
            return false;
        }
        for (const auto& entry : page.entries) {
            actual_entries.emplace_back(entry.key, entry.value);
        }
        token = page.continuation_token;
    } while (!token.empty());
    return actual_entries == VisibleEntries(reference);
}

bool VerifyRandomRange(
    const minikv::Database& database,
    const ReferenceModel& reference,
    std::mt19937_64& random
) {
    const std::size_t left = static_cast<std::size_t>(random() % 257U);
    const std::size_t width = 1U + static_cast<std::size_t>(random() % 80U);
    minikv::ScanOptions options;
    options.begin = NumberedKey("model-", left);
    options.end = NumberedKey("model-", std::min<std::size_t>(257U, left + width));
    options.limit = 31;

    std::vector<std::pair<std::string, std::string>> expected;
    for (auto entry = reference.lower_bound(options.begin);
         entry != reference.end() && entry->first < *options.end;
         ++entry) {
        if (entry->second.has_value()) {
            expected.emplace_back(entry->first, *entry->second);
        }
    }

    const auto result = database.Scan(options);
    if (!result.status.ok()) {
        return false;
    }
    const bool expected_truncated = expected.size() > options.limit;
    if (expected_truncated) {
        expected.resize(options.limit);
    }
    std::vector<std::pair<std::string, std::string>> actual;
    for (const auto& entry : result.entries) {
        actual.emplace_back(entry.key, entry.value);
    }
    return actual == expected && result.truncated == expected_truncated &&
           result.continuation_token.empty() == !expected_truncated;
}

void TestLongRandomModelAcrossMaintenanceAndRestart() {
    TemporaryDirectory directory("/tmp/minikv-v10-model-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    std::cout << "V10 model seed: " << kModelSeed
              << ", operations: " << kModelOperations << '\n';

    minikv::Options options;
    options.memtable_size_limit = 64U * 1024U;
    options.compaction_output_size_limit = 16U * 1024U;
    options.maximum_scan_entries = 128;

    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "model database must open"
    );
    if (database == nullptr) {
        return;
    }

    ReferenceModel reference;
    std::mt19937_64 random(kModelSeed);
    const auto async = AsyncWrite();
    bool operations_ok = true;
    for (std::size_t operation = 1; operation <= kModelOperations; ++operation) {
        const std::string key = NumberedKey(
            "model-",
            static_cast<std::size_t>(random() % 257U)
        );
        const std::uint64_t choice = random() % 100U;
        if (choice < 55U) {
            std::string value = "value-" + std::to_string(operation) + "-";
            const std::size_t suffix = static_cast<std::size_t>(random() % 48U);
            value.append(suffix, static_cast<char>(random() & 0xFFU));
            operations_ok =
                database->Put(key, value, async).ok() && operations_ok;
            reference[key] = std::move(value);
        } else if (choice < 70U) {
            operations_ok = database->Delete(key, async).ok() && operations_ok;
            reference[key] = std::nullopt;
        } else if (choice < 88U) {
            const auto actual = database->Get(key);
            const auto expected = reference.find(key);
            const bool missing = expected == reference.end() ||
                !expected->second.has_value();
            operations_ok = (
                missing
                    ? actual.status.IsNotFound()
                    : actual.status.ok() &&
                        actual.value == *expected->second
            ) && operations_ok;
        } else {
            operations_ok =
                VerifyRandomRange(*database, reference, random) && operations_ok;
        }

        if (operation % 257U == 0U) {
            operations_ok = database->Flush().ok() && operations_ok;
        }
        if (operation % 1021U == 0U) {
            operations_ok = database->Compact().ok() && operations_ok;
        }
        if (operation % 1499U == 0U) {
            operations_ok = database->Close().ok() && operations_ok;
            database.reset();
            operations_ok =
                OpenDatabase(directory.path(), options, &database) && operations_ok;
            if (database == nullptr) {
                break;
            }
            operations_ok =
                VerifyDatabase(*database, reference) && operations_ok;
        }
    }

    minikv::test::Expect(
        operations_ok && database != nullptr,
        "20,000-operation model must preserve point and range semantics"
    );
    if (database == nullptr) {
        return;
    }
    minikv::test::Expect(
        database->Flush().ok() && database->Compact().ok() &&
            VerifyDatabase(*database, reference),
        "model state must survive final maintenance"
    );
    minikv::test::Expect(
        database->Close().ok(),
        "model database must close durably"
    );
    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database) &&
            VerifyDatabase(*database, reference),
        "model state must survive the final reopen"
    );
}

bool WriteExact(int descriptor, const void* bytes, std::size_t size) {
    const auto* current = static_cast<const char*>(bytes);
    std::size_t remaining = size;
    while (remaining != 0U) {
        const ssize_t result = ::write(descriptor, current, remaining);
        if (result > 0) {
            const auto consumed = static_cast<std::size_t>(result);
            current += consumed;
            remaining -= consumed;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool ReadExact(int descriptor, void* bytes, std::size_t size) {
    auto* current = static_cast<char*>(bytes);
    std::size_t remaining = size;
    while (remaining != 0U) {
        const ssize_t result = ::read(descriptor, current, remaining);
        if (result > 0) {
            const auto consumed = static_cast<std::size_t>(result);
            current += consumed;
            remaining -= consumed;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

struct CrashOperation {
    bool deletion = false;
    std::string key;
    std::string value;
};

void ApplyOperation(
    minikv::Database* database,
    const CrashOperation& operation
) {
    const auto status = operation.deletion
        ? database->Delete(operation.key)
        : database->Put(operation.key, operation.value);
    if (!status.ok()) {
        ::_exit(20);
    }
}

void TestAcknowledgedWritesRecoverAfterRandomCrashes() {
    TemporaryDirectory parent("/tmp/minikv-v10-crash-XXXXXX");
    if (!parent.valid()) {
        return;
    }
    std::cout << "V10 crash seed: " << kCrashSeed
              << ", rounds: " << kCrashRounds << '\n';
    std::mt19937_64 random(kCrashSeed);

    for (std::size_t round = 0; round < kCrashRounds; ++round) {
        const std::string directory =
            parent.path() + "/round-" + std::to_string(round);
        std::error_code error;
        fs::create_directory(directory, error);
        minikv::test::Expect(!error, "crash-round directory must be created");
        if (error) {
            continue;
        }

        std::vector<CrashOperation> operations;
        for (std::size_t index = 0; index < 32U; ++index) {
            CrashOperation operation;
            operation.deletion = random() % 5U == 0U;
            operation.key = NumberedKey(
                "crash-",
                static_cast<std::size_t>(random() % 13U)
            );
            operation.value = "round-" + std::to_string(round) +
                "-value-" + std::to_string(index);
            operations.push_back(std::move(operation));
        }
        const std::size_t kill_after =
            5U + static_cast<std::size_t>(random() % 24U);

        int acknowledged[2] = {-1, -1};
        int resume[2] = {-1, -1};
        minikv::test::Expect(
            ::pipe(acknowledged) == 0 && ::pipe(resume) == 0,
            "crash synchronization pipes must open"
        );
        const pid_t child = ::fork();
        minikv::test::Expect(child >= 0, "crash test must fork");
        if (child == 0) {
            ::close(acknowledged[0]);
            ::close(resume[1]);
            minikv::Options options;
            std::unique_ptr<minikv::Database> database;
            if (!OpenDatabase(directory, options, &database)) {
                ::_exit(10);
            }
            for (std::size_t index = 0; index < operations.size(); ++index) {
                ApplyOperation(database.get(), operations[index]);
                const std::uint32_t acknowledged_index =
                    static_cast<std::uint32_t>(index);
                if (!WriteExact(
                        acknowledged[1],
                        &acknowledged_index,
                        sizeof(acknowledged_index)
                    )) {
                    ::_exit(11);
                }
                char command = 0;
                if (!ReadExact(resume[0], &command, sizeof(command))) {
                    ::_exit(12);
                }
            }
            ::_exit(0);
        }
        if (child < 0) {
            ::close(acknowledged[0]);
            ::close(acknowledged[1]);
            ::close(resume[0]);
            ::close(resume[1]);
            continue;
        }

        ::close(acknowledged[1]);
        ::close(resume[0]);
        bool protocol_ok = true;
        for (std::size_t count = 0; count < kill_after; ++count) {
            std::uint32_t index = 0;
            protocol_ok = ReadExact(
                acknowledged[0],
                &index,
                sizeof(index)
            ) && index == count && protocol_ok;
            if (count + 1U != kill_after) {
                const char resume_child = 'R';
                protocol_ok =
                    WriteExact(resume[1], &resume_child, 1) && protocol_ok;
            }
        }
        minikv::test::Expect(
            protocol_ok,
            "child must report each strict write before the crash"
        );
        static_cast<void>(::kill(child, SIGKILL));
        ::close(acknowledged[0]);
        ::close(resume[1]);
        int wait_status = 0;
        minikv::test::Expect(
            ::waitpid(child, &wait_status, 0) == child &&
                WIFSIGNALED(wait_status) &&
                WTERMSIG(wait_status) == SIGKILL,
            "crash child must terminate through SIGKILL"
        );

        ReferenceModel expected;
        for (std::size_t index = 0; index < kill_after; ++index) {
            const auto& operation = operations[index];
            expected[operation.key] = operation.deletion
                ? std::optional<std::string>()
                : std::optional<std::string>(operation.value);
        }
        minikv::Options options;
        std::unique_ptr<minikv::Database> recovered;
        minikv::test::Expect(
            OpenDatabase(directory, options, &recovered) &&
                VerifyDatabase(*recovered, expected),
            "every acknowledged strict write must recover after SIGKILL"
        );
        if (recovered != nullptr) {
            minikv::test::Expect(
                recovered->Close().ok(),
                "recovered crash database must close"
            );
        }
    }
}

void TestSerializedWritersReadersAndBackgroundMaintenance() {
    TemporaryDirectory directory("/tmp/minikv-v10-concurrent-XXXXXX");
    if (!directory.valid()) {
        return;
    }

    minikv::Options options;
    options.background_maintenance = true;
    options.memtable_size_limit = 4U * 1024U;
    options.compaction_output_size_limit = 8U * 1024U;
    options.level0_compaction_trigger = 3;
    options.maximum_scan_entries = 128;

    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "concurrency-stress database must open"
    );
    if (database == nullptr) {
        return;
    }

    std::atomic<std::size_t> unexpected{0};
    std::vector<std::thread> readers;
    for (std::size_t reader = 0; reader < 4U; ++reader) {
        readers.emplace_back([&, reader] {
            std::uint64_t state = 0x9E3779B97F4A7C15ULL + reader;
            for (std::size_t iteration = 0;
                 iteration < 600U;
                 ++iteration) {
                state = state * 6364136223846793005ULL + 1U;
                const std::size_t writer =
                    static_cast<std::size_t>(state % kConcurrentWriters);
                const std::size_t slot = static_cast<std::size_t>(
                    (state >> 16U) % 67U
                );
                const auto get = database->Get(
                    NumberedKey(
                        "thread-" + std::to_string(writer) + "-",
                        slot
                    )
                );
                if (!get.status.ok() && !get.status.IsNotFound()) {
                    ++unexpected;
                }

                const auto scan = database->ScanPrefix("thread-", 128);
                if (!scan.status.ok() ||
                    !std::is_sorted(
                        scan.entries.begin(),
                        scan.entries.end(),
                        [](const minikv::ScanEntry& left,
                           const minikv::ScanEntry& right) {
                            return left.key < right.key;
                        }
                    ) ||
                    std::any_of(
                        scan.entries.begin(),
                        scan.entries.end(),
                        [](const minikv::ScanEntry& entry) {
                            return entry.key.rfind("thread-", 0) != 0;
                        }
                    )) {
                    ++unexpected;
                }
            }
        });
    }

    std::vector<ReferenceModel> thread_models(kConcurrentWriters);
    std::vector<std::thread> writers;
    const auto async = AsyncWrite();
    for (std::size_t writer = 0; writer < kConcurrentWriters; ++writer) {
        writers.emplace_back([&, writer] {
            for (std::size_t operation = 0;
                 operation < kWritesPerThread;
                 ++operation) {
                const std::string key = NumberedKey(
                    "thread-" + std::to_string(writer) + "-",
                    operation % 67U
                );
                if (operation % 6U == 0U) {
                    if (!database->Delete(key, async).ok()) {
                        ++unexpected;
                        return;
                    }
                    thread_models[writer][key] = std::nullopt;
                } else {
                    const std::string value =
                        "writer-" + std::to_string(writer) +
                        "-operation-" + std::to_string(operation);
                    if (!database->Put(key, value, async).ok()) {
                        ++unexpected;
                        return;
                    }
                    thread_models[writer][key] = value;
                }
            }
        });
    }

    for (auto& writer : writers) {
        writer.join();
    }
    const auto maintenance_status = database->WaitForBackgroundWork();
    for (auto& reader : readers) {
        reader.join();
    }

    ReferenceModel expected;
    for (const auto& model : thread_models) {
        expected.insert(model.begin(), model.end());
    }
    minikv::test::Expect(
        unexpected == 0U && maintenance_status.ok() &&
            VerifyDatabase(*database, expected),
        "serialized writers and concurrent readers must preserve a stable model"
    );
    minikv::test::Expect(
        database->Close().ok(),
        "concurrency-stress Close must finish"
    );
    database.reset();
    options.background_maintenance = false;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database) &&
            VerifyDatabase(*database, expected),
        "concurrency-stress final state must survive restart"
    );
}

void TestSameKeyWritersFollowSequenceOrder() {
    TemporaryDirectory directory("/tmp/minikv-v10-same-key-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 16U * 1024U * 1024U;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "same-key database must open"
    );
    if (database == nullptr) {
        return;
    }

    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> writers;
    const auto async = AsyncWrite();
    for (std::size_t writer = 0; writer < 4U; ++writer) {
        writers.emplace_back([&, writer] {
            for (std::size_t operation = 0; operation < 300U; ++operation) {
                const std::string value =
                    std::to_string(writer) + ":" + std::to_string(operation);
                if (!database->Put("shared-key", value, async).ok()) {
                    ++failures;
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    const auto result = database->Get("shared-key");
    minikv::test::Expect(
        failures == 0U && result.status.ok() &&
            database->last_sequence() == 1200U &&
            result.sequence == database->last_sequence(),
        "same-key concurrent writes must expose the greatest serialized Sequence"
    );
}

}  // namespace

int main() {
    TestLongRandomModelAcrossMaintenanceAndRestart();
    TestAcknowledgedWritesRecoverAfterRandomCrashes();
    TestSerializedWritersReadersAndBackgroundMaintenance();
    TestSameKeyWritersFollowSequenceOrder();
    return minikv::test::Finish("V10 Reliability");
}
