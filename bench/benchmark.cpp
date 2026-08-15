#include "minikv/database.hpp"
#include "minikv/version.hpp"
#include "minikv/wal.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kDefaultSeed = 0x4D4B5642454E4348ULL;

struct Config {
    std::string workload;
    fs::path root = "benchmark-results/runtime";
    std::size_t operations = 10000;
    std::size_t keys = 5000;
    std::size_t value_size = 100;
    std::size_t threads = 4;
    std::size_t memtable_bytes = 64U * 1024U;
    std::uint64_t seed = kDefaultSeed;
    minikv::SyncMode sync_mode = minikv::SyncMode::kAsync;
    bool bloom_enabled = true;
    bool compact = false;
    bool verify = true;
    bool keep_data = false;
};

struct Metrics {
    std::uint64_t operations = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t logical_write_bytes = 0;
    std::uint64_t logical_live_bytes = 0;
    std::uint64_t storage_bytes = 0;
    std::uint64_t sstable_bytes = 0;
    std::uint64_t estimated_wal_bytes = 0;
    std::uint64_t maintenance_microseconds = 0;
    double elapsed_seconds = 0.0;
    double end_to_end_seconds = 0.0;
    minikv::DatabaseReadStats read_stats;
    std::vector<std::uint64_t> latencies_nanoseconds;
    bool verified = true;
};

class BenchmarkDirectory {
public:
    BenchmarkDirectory(fs::path root, std::string_view workload, bool keep)
        : keep_(keep) {
        std::error_code error;
        fs::create_directories(root, error);
        if (error) {
            status_ = "cannot create benchmark root: " + error.message();
            return;
        }
        path_ = root / (
            "minikv-bench-" + std::to_string(static_cast<long long>(::getpid())) +
            "-" + std::string(workload)
        );
        if (fs::exists(path_, error)) {
            status_ = "benchmark directory already exists: " + path_.string();
            return;
        }
        if (!fs::create_directory(path_, error) || error) {
            status_ = "cannot create benchmark directory: " + error.message();
            return;
        }
        valid_ = true;
    }

    ~BenchmarkDirectory() {
        if (valid_ && !keep_) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

private:
    fs::path path_;
    std::string status_;
    bool valid_ = false;
    bool keep_ = false;
};

void PrintUsage(std::ostream& output) {
    output
        << "usage: minikv_benchmark --workload NAME [options]\n"
        << "workloads: sequential-write, random-write, read-hit, read-miss,\n"
        << "           mixed-50, mixed-95, parallel-read\n"
        << "options:\n"
        << "  --operations N       timed operations (default 10000)\n"
        << "  --keys N             fixture keys (default 5000)\n"
        << "  --value-size N       value bytes (default 100)\n"
        << "  --threads N          parallel-read threads (default 4)\n"
        << "  --memtable-bytes N   MemTable threshold (default 65536)\n"
        << "  --seed N             deterministic decimal seed\n"
        << "  --root PATH          benchmark scratch root\n"
        << "  --sync strict|async  write acknowledgment policy\n"
        << "  --bloom on|off       construct and query Bloom filters\n"
        << "  --compact on|off     compact fixture before timed reads\n"
        << "  --verify on|off      verify final values (default on)\n"
        << "  --keep-data           retain the generated database\n";
}

bool ParseUnsigned(
    std::string_view text,
    std::uint64_t* output
) {
    if (output == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    *output = value;
    return true;
}

bool ParseSize(
    std::string_view text,
    std::size_t* output
) {
    std::uint64_t value = 0;
    if (!ParseUnsigned(text, &value) ||
        value > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    *output = static_cast<std::size_t>(value);
    return true;
}

bool ParseOnOff(std::string_view text, bool* output) {
    if (output == nullptr) {
        return false;
    }
    if (text == "on") {
        *output = true;
        return true;
    }
    if (text == "off") {
        *output = false;
        return true;
    }
    return false;
}

bool IsKnownWorkload(std::string_view workload) {
    return workload == "sequential-write" ||
           workload == "random-write" ||
           workload == "read-hit" ||
           workload == "read-miss" ||
           workload == "mixed-50" ||
           workload == "mixed-95" ||
           workload == "parallel-read";
}

bool ParseArguments(
    int argc,
    char** argv,
    Config* config,
    std::string* error
) {
    if (config == nullptr || error == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            PrintUsage(std::cout);
            return false;
        }
        if (argument == "--keep-data") {
            config->keep_data = true;
            continue;
        }
        if (index + 1 >= argc) {
            *error = "missing value for " + std::string(argument);
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--workload") {
            config->workload = value;
        } else if (argument == "--operations") {
            if (!ParseSize(value, &config->operations)) {
                *error = "invalid operation count";
                return false;
            }
        } else if (argument == "--keys") {
            if (!ParseSize(value, &config->keys)) {
                *error = "invalid key count";
                return false;
            }
        } else if (argument == "--value-size") {
            if (!ParseSize(value, &config->value_size)) {
                *error = "invalid value size";
                return false;
            }
        } else if (argument == "--threads") {
            if (!ParseSize(value, &config->threads)) {
                *error = "invalid thread count";
                return false;
            }
        } else if (argument == "--memtable-bytes") {
            if (!ParseSize(value, &config->memtable_bytes)) {
                *error = "invalid MemTable size";
                return false;
            }
        } else if (argument == "--seed") {
            if (!ParseUnsigned(value, &config->seed)) {
                *error = "invalid seed";
                return false;
            }
        } else if (argument == "--root") {
            config->root = value;
        } else if (argument == "--sync") {
            if (value == "strict") {
                config->sync_mode = minikv::SyncMode::kSync;
            } else if (value == "async") {
                config->sync_mode = minikv::SyncMode::kAsync;
            } else {
                *error = "sync must be strict or async";
                return false;
            }
        } else if (argument == "--bloom") {
            if (!ParseOnOff(value, &config->bloom_enabled)) {
                *error = "Bloom switch must be on or off";
                return false;
            }
        } else if (argument == "--compact") {
            if (!ParseOnOff(value, &config->compact)) {
                *error = "Compaction switch must be on or off";
                return false;
            }
        } else if (argument == "--verify") {
            if (!ParseOnOff(value, &config->verify)) {
                *error = "verification switch must be on or off";
                return false;
            }
        } else {
            *error = "unknown argument: " + std::string(argument);
            return false;
        }
    }

    if (!IsKnownWorkload(config->workload)) {
        *error = "a known --workload is required";
        return false;
    }
    if (config->operations == 0 || config->keys == 0 ||
        config->value_size == 0 || config->threads == 0 ||
        config->memtable_bytes == 0) {
        *error = "numeric options must be positive";
        return false;
    }
    if (config->value_size > minikv::Options::kDefaultMaxValueSize) {
        *error = "value size exceeds MiniKV's default limit";
        return false;
    }
    return true;
}

std::string KeyFor(std::size_t index) {
    char digits[32] = {};
    const int written = std::snprintf(
        digits,
        sizeof(digits),
        "key-%012zu",
        index
    );
    return written > 0 ? std::string(digits) : std::string("key");
}

std::string MissingKeyFor(std::size_t index) {
    return KeyFor(index) + "-missing";
}

std::string ValueFor(
    std::size_t key_index,
    std::size_t version,
    std::size_t size
) {
    std::string value =
        "value-" + std::to_string(key_index) + "-" + std::to_string(version);
    if (value.size() < size) {
        value.append(size - value.size(), static_cast<char>('a' + key_index % 26U));
    } else {
        value.resize(size);
    }
    return value;
}

minikv::WriteOptions WritePolicy(const Config& config) {
    minikv::WriteOptions options;
    options.sync_mode = config.sync_mode;
    return options;
}

bool OpenDatabase(
    const fs::path& directory,
    const Config& config,
    std::unique_ptr<minikv::Database>* database
) {
    minikv::Options options;
    options.memtable_size_limit = config.memtable_bytes;
    options.bloom_filter_enabled = config.bloom_enabled;
    options.level0_compaction_trigger =
        std::numeric_limits<std::size_t>::max();
    minikv::DatabaseOpenResult result;
    const auto status = minikv::Database::Open(
        directory.string(),
        options,
        database,
        &result
    );
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return false;
    }
    return true;
}

void RecordLatency(
    Clock::time_point begin,
    std::vector<std::uint64_t>* latencies
) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - begin
    ).count();
    latencies->push_back(
        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U
    );
}

bool FillFixture(
    minikv::Database* database,
    const Config& config,
    std::vector<std::string>* values,
    Metrics* metrics
) {
    values->assign(config.keys, {});
    std::vector<std::size_t> order(config.keys);
    std::iota(order.begin(), order.end(), 0U);
    std::mt19937_64 random(config.seed);
    std::shuffle(order.begin(), order.end(), random);
    const std::size_t batch = std::max<std::size_t>(1U, config.keys / 8U);
    const auto async = minikv::WriteOptions{minikv::SyncMode::kAsync};

    for (std::size_t position = 0; position < order.size(); ++position) {
        const std::size_t key_index = order[position];
        (*values)[key_index] = ValueFor(
            key_index,
            0,
            config.value_size
        );
        const std::string key = KeyFor(key_index);
        if (!database->Put(key, (*values)[key_index], async).ok()) {
            return false;
        }
        metrics->logical_live_bytes +=
            static_cast<std::uint64_t>(key.size() + (*values)[key_index].size());
        if ((position + 1U) % batch == 0U &&
            position + 1U != order.size() &&
            !database->Flush().ok()) {
            return false;
        }
    }
    if (!database->Flush().ok()) {
        return false;
    }
    if (config.compact && !database->Compact().ok()) {
        return false;
    }

    const std::size_t warmup = std::min<std::size_t>(
        1000U,
        config.operations / 10U
    );
    for (std::size_t operation = 0; operation < warmup; ++operation) {
        const std::size_t key_index =
            static_cast<std::size_t>(random() % config.keys);
        if (!database->Get(KeyFor(key_index)).status.ok()) {
            return false;
        }
    }
    return true;
}

minikv::DatabaseReadStats SubtractStats(
    const minikv::DatabaseReadStats& after,
    const minikv::DatabaseReadStats& before
) {
    minikv::DatabaseReadStats result;
    result.point_lookups = after.point_lookups - before.point_lookups;
    result.mutable_hits = after.mutable_hits - before.mutable_hits;
    result.immutable_hits = after.immutable_hits - before.immutable_hits;
    result.tables_considered =
        after.tables_considered - before.tables_considered;
    result.range_rejections =
        after.range_rejections - before.range_rejections;
    result.bloom_filter_checks =
        after.bloom_filter_checks - before.bloom_filter_checks;
    result.bloom_filter_rejections =
        after.bloom_filter_rejections - before.bloom_filter_rejections;
    result.bloom_false_positives =
        after.bloom_false_positives - before.bloom_false_positives;
    result.data_blocks_read =
        after.data_blocks_read - before.data_blocks_read;
    result.bytes_read = after.bytes_read - before.bytes_read;
    return result;
}

bool RunReadWorkload(
    minikv::Database* database,
    const Config& config,
    bool missing,
    Metrics* metrics
) {
    std::mt19937_64 random(config.seed ^ 0x52454144ULL);
    const auto before_stats = database->read_statistics();
    const auto begin = Clock::now();
    for (std::size_t operation = 0;
         operation < config.operations;
         ++operation) {
        const std::size_t key_index =
            static_cast<std::size_t>(random() % config.keys);
        const std::string key = missing
            ? MissingKeyFor(key_index)
            : KeyFor(key_index);
        const auto operation_begin = Clock::now();
        const auto result = database->Get(key);
        RecordLatency(operation_begin, &metrics->latencies_nanoseconds);
        if ((missing && !result.status.IsNotFound()) ||
            (!missing && !result.status.ok())) {
            return false;
        }
    }
    const auto end = Clock::now();
    metrics->elapsed_seconds =
        std::chrono::duration<double>(end - begin).count();
    metrics->end_to_end_seconds = metrics->elapsed_seconds;
    metrics->operations = config.operations;
    metrics->reads = config.operations;
    metrics->read_stats = SubtractStats(
        database->read_statistics(),
        before_stats
    );
    return true;
}

bool RunParallelReadWorkload(
    minikv::Database* database,
    const Config& config,
    Metrics* metrics
) {
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};
    std::vector<std::vector<std::uint64_t>> thread_latencies(config.threads);
    std::vector<std::thread> threads;
    threads.reserve(config.threads);
    const auto before_stats = database->read_statistics();

    for (std::size_t thread = 0; thread < config.threads; ++thread) {
        threads.emplace_back([&, thread] {
            std::mt19937_64 random(config.seed + thread);
            ++ready;
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (std::size_t operation = thread;
                 operation < config.operations;
                 operation += config.threads) {
                const std::size_t key_index =
                    static_cast<std::size_t>(random() % config.keys);
                const auto operation_begin = Clock::now();
                const auto result = database->Get(KeyFor(key_index));
                RecordLatency(operation_begin, &thread_latencies[thread]);
                if (!result.status.ok()) {
                    valid = false;
                }
            }
        });
    }
    while (ready.load() != config.threads) {
        std::this_thread::yield();
    }
    const auto begin = Clock::now();
    start = true;
    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = Clock::now();
    for (auto& latencies : thread_latencies) {
        metrics->latencies_nanoseconds.insert(
            metrics->latencies_nanoseconds.end(),
            latencies.begin(),
            latencies.end()
        );
    }
    metrics->elapsed_seconds =
        std::chrono::duration<double>(end - begin).count();
    metrics->end_to_end_seconds = metrics->elapsed_seconds;
    metrics->operations = config.operations;
    metrics->reads = config.operations;
    metrics->read_stats = SubtractStats(
        database->read_statistics(),
        before_stats
    );
    return valid.load();
}

bool RunWriteWorkload(
    minikv::Database* database,
    const Config& config,
    bool random_order,
    std::vector<std::string>* values,
    Metrics* metrics
) {
    std::vector<std::size_t> order(config.operations);
    std::iota(order.begin(), order.end(), 0U);
    if (random_order) {
        std::mt19937_64 random(config.seed ^ 0x5752495445ULL);
        std::shuffle(order.begin(), order.end(), random);
    }
    values->assign(config.operations, {});
    const auto policy = WritePolicy(config);
    const auto begin = Clock::now();
    for (const std::size_t key_index : order) {
        const std::string key = KeyFor(key_index);
        (*values)[key_index] = ValueFor(
            key_index,
            0,
            config.value_size
        );
        const auto operation_begin = Clock::now();
        const auto status = database->Put(
            key,
            (*values)[key_index],
            policy
        );
        RecordLatency(operation_begin, &metrics->latencies_nanoseconds);
        if (!status.ok()) {
            return false;
        }
        const std::uint64_t payload = static_cast<std::uint64_t>(
            key.size() + (*values)[key_index].size()
        );
        metrics->logical_write_bytes += payload;
        metrics->logical_live_bytes += payload;
        metrics->estimated_wal_bytes +=
            static_cast<std::uint64_t>(minikv::kWalHeaderSize) + payload;
    }
    const auto operation_end = Clock::now();
    const auto maintenance_begin = Clock::now();
    if (!database->Flush().ok()) {
        return false;
    }
    const auto end = Clock::now();
    metrics->elapsed_seconds =
        std::chrono::duration<double>(operation_end - begin).count();
    metrics->end_to_end_seconds =
        std::chrono::duration<double>(end - begin).count();
    const auto maintenance =
        std::chrono::duration_cast<std::chrono::microseconds>(
            end - maintenance_begin
        ).count();
    metrics->maintenance_microseconds = maintenance > 0
        ? static_cast<std::uint64_t>(maintenance)
        : 0U;
    metrics->operations = config.operations;
    metrics->writes = config.operations;
    return true;
}

bool RunMixedWorkload(
    minikv::Database* database,
    const Config& config,
    std::size_t read_percentage,
    std::vector<std::string>* values,
    Metrics* metrics
) {
    std::mt19937_64 random(config.seed ^ read_percentage);
    const auto policy = WritePolicy(config);
    const auto before_stats = database->read_statistics();
    const auto begin = Clock::now();
    for (std::size_t operation = 0;
         operation < config.operations;
         ++operation) {
        const std::size_t key_index =
            static_cast<std::size_t>(random() % config.keys);
        const auto operation_begin = Clock::now();
        if (random() % 100U < read_percentage) {
            const auto result = database->Get(KeyFor(key_index));
            if (!result.status.ok()) {
                return false;
            }
            ++metrics->reads;
        } else {
            (*values)[key_index] = ValueFor(
                key_index,
                operation + 1U,
                config.value_size
            );
            const std::string key = KeyFor(key_index);
            if (!database->Put(
                    key,
                    (*values)[key_index],
                    policy
                ).ok()) {
                return false;
            }
            const std::uint64_t payload = static_cast<std::uint64_t>(
                key.size() + (*values)[key_index].size()
            );
            metrics->logical_write_bytes += payload;
            metrics->estimated_wal_bytes +=
                static_cast<std::uint64_t>(minikv::kWalHeaderSize) + payload;
            ++metrics->writes;
        }
        RecordLatency(operation_begin, &metrics->latencies_nanoseconds);
    }
    const auto operation_end = Clock::now();
    const auto maintenance_begin = Clock::now();
    if (!database->Flush().ok()) {
        return false;
    }
    const auto end = Clock::now();
    metrics->elapsed_seconds =
        std::chrono::duration<double>(operation_end - begin).count();
    metrics->end_to_end_seconds =
        std::chrono::duration<double>(end - begin).count();
    const auto maintenance =
        std::chrono::duration_cast<std::chrono::microseconds>(
            end - maintenance_begin
        ).count();
    metrics->maintenance_microseconds = maintenance > 0
        ? static_cast<std::uint64_t>(maintenance)
        : 0U;
    metrics->operations = config.operations;
    metrics->read_stats = SubtractStats(
        database->read_statistics(),
        before_stats
    );
    return true;
}

bool VerifyValues(
    minikv::Database* database,
    const std::vector<std::string>& values
) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto result = database->Get(KeyFor(index));
        if (!result.status.ok() || result.value != values[index]) {
            return false;
        }
    }
    return true;
}

void MeasureStorage(const fs::path& directory, Metrics* metrics) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error)) {
            continue;
        }
        const auto size = entry.file_size(error);
        if (error) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".sst") {
            metrics->sstable_bytes += size;
            metrics->storage_bytes += size;
        } else if (entry.path().extension() == ".wal" ||
                   name == minikv::kManifestFileName) {
            metrics->storage_bytes += size;
        }
    }
}

std::uint64_t Percentile(
    std::vector<std::uint64_t> values,
    std::size_t percentage
) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t rank =
        (values.size() * percentage + 99U) / 100U;
    return values[std::min(values.size() - 1U, rank - 1U)];
}

double SafeRatio(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0
        ? 0.0
        : static_cast<double>(numerator) /
            static_cast<double>(denominator);
}

void PrintResult(
    const Config& config,
    const BenchmarkDirectory& directory,
    const Metrics& metrics
) {
    const double operations_per_second =
        metrics.elapsed_seconds > 0.0
            ? static_cast<double>(metrics.operations) /
                metrics.elapsed_seconds
            : 0.0;
    const double end_to_end_operations_per_second =
        metrics.end_to_end_seconds > 0.0
            ? static_cast<double>(metrics.operations) /
                metrics.end_to_end_seconds
            : 0.0;
    const bool pure_write =
        config.workload == "sequential-write" ||
        config.workload == "random-write";
    const std::uint64_t estimated_physical_write_bytes =
        metrics.estimated_wal_bytes + metrics.sstable_bytes;
    const double write_amplification = pure_write
        ? SafeRatio(
              estimated_physical_write_bytes,
              metrics.logical_write_bytes
          )
        : 0.0;
    const std::uint64_t logical_key_count =
        pure_write ? config.operations : config.keys;

    std::cout << std::fixed << std::setprecision(3)
              << "{"
              << "\"engine_version\":\"" << minikv::EngineVersion() << "\","
              << "\"workload\":\"" << config.workload << "\","
              << "\"operations\":" << metrics.operations << ","
              << "\"keys\":" << logical_key_count << ","
              << "\"value_size\":" << config.value_size << ","
              << "\"threads\":" << config.threads << ","
              << "\"seed\":" << config.seed << ","
              << "\"sync\":\""
              << (config.sync_mode == minikv::SyncMode::kSync
                      ? "strict"
                      : "async")
              << "\","
              << "\"bloom\":" << (config.bloom_enabled ? "true" : "false")
              << ",\"compacted\":" << (config.compact ? "true" : "false")
              << ",\"verified\":" << (metrics.verified ? "true" : "false")
              << ",\"elapsed_seconds\":" << metrics.elapsed_seconds
              << ",\"end_to_end_seconds\":" << metrics.end_to_end_seconds
              << ",\"ops_per_second\":" << operations_per_second
              << ",\"end_to_end_ops_per_second\":"
              << end_to_end_operations_per_second
              << ",\"p50_us\":"
              << static_cast<double>(Percentile(
                     metrics.latencies_nanoseconds,
                     50
                 )) / 1000.0
              << ",\"p95_us\":"
              << static_cast<double>(Percentile(
                     metrics.latencies_nanoseconds,
                     95
                 )) / 1000.0
              << ",\"p99_us\":"
              << static_cast<double>(Percentile(
                     metrics.latencies_nanoseconds,
                     99
                 )) / 1000.0
              << ",\"reads\":" << metrics.reads
              << ",\"writes\":" << metrics.writes
              << ",\"maintenance_us\":" << metrics.maintenance_microseconds
              << ",\"data_blocks_per_read\":"
              << SafeRatio(
                     metrics.read_stats.data_blocks_read,
                     metrics.reads
                 )
              << ",\"tables_considered_per_read\":"
              << SafeRatio(
                     metrics.read_stats.tables_considered,
                     metrics.reads
                 )
              << ",\"bytes_per_read\":"
              << SafeRatio(metrics.read_stats.bytes_read, metrics.reads)
              << ",\"estimated_write_amplification\":";
    if (pure_write) {
        std::cout << write_amplification;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"space_amplification\":"
              << SafeRatio(
                     metrics.storage_bytes,
                     metrics.logical_live_bytes
                 )
              << ",\"storage_bytes\":" << metrics.storage_bytes
              << ",\"sstable_bytes\":" << metrics.sstable_bytes
              << ",\"logical_live_bytes\":" << metrics.logical_live_bytes;
    if (config.keep_data) {
        std::cout << ",\"data_directory\":\""
                  << directory.path().string() << "\"";
    }
    std::cout << "}\n";
}

int Run(const Config& config) {
    BenchmarkDirectory directory(
        config.root,
        config.workload,
        config.keep_data
    );
    if (!directory.valid()) {
        std::cerr << directory.status() << '\n';
        return 3;
    }

    std::unique_ptr<minikv::Database> database;
    if (!OpenDatabase(directory.path(), config, &database)) {
        return 4;
    }

    Metrics metrics;
    std::vector<std::string> values;
    bool success = true;
    if (config.workload == "sequential-write" ||
        config.workload == "random-write") {
        success = RunWriteWorkload(
            database.get(),
            config,
            config.workload == "random-write",
            &values,
            &metrics
        );
    } else {
        success = FillFixture(
            database.get(),
            config,
            &values,
            &metrics
        );
        if (success && config.workload == "read-hit") {
            success = RunReadWorkload(
                database.get(),
                config,
                false,
                &metrics
            );
        } else if (success && config.workload == "read-miss") {
            success = RunReadWorkload(
                database.get(),
                config,
                true,
                &metrics
            );
        } else if (success && config.workload == "parallel-read") {
            success = RunParallelReadWorkload(
                database.get(),
                config,
                &metrics
            );
        } else if (success && config.workload == "mixed-50") {
            success = RunMixedWorkload(
                database.get(),
                config,
                50,
                &values,
                &metrics
            );
        } else if (success && config.workload == "mixed-95") {
            success = RunMixedWorkload(
                database.get(),
                config,
                95,
                &values,
                &metrics
            );
        }
    }

    if (!success) {
        std::cerr << "benchmark operation failed\n";
        return 5;
    }
    metrics.verified = !config.verify ||
        VerifyValues(database.get(), values);
    MeasureStorage(directory.path(), &metrics);
    const auto close_status = database->Close();
    if (!close_status.ok()) {
        std::cerr << close_status.ToString() << '\n';
        return 6;
    }
    if (!metrics.verified) {
        std::cerr << "benchmark verification failed\n";
        return 7;
    }
    PrintResult(config, directory, metrics);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!ParseArguments(argc, argv, &config, &error)) {
        if (!error.empty()) {
            std::cerr << error << '\n';
            PrintUsage(std::cerr);
            return 2;
        }
        return 0;
    }
    return Run(config);
}
