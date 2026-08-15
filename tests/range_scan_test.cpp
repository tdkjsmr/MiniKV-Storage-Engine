#include "minikv/database.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

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
            "temporary range-scan directory must exist"
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

std::string NumberedKey(std::string_view prefix, std::size_t number) {
    std::ostringstream output;
    output << prefix << std::setfill('0') << std::setw(3) << number;
    return output.str();
}

std::vector<std::string> Keys(const minikv::ScanResult& result) {
    std::vector<std::string> keys;
    keys.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        keys.push_back(entry.key);
    }
    return keys;
}

void TestSSTableIteratorSeeksAndRetainsOneBlock() {
    TemporaryDirectory directory("/tmp/minikv-v8-table-iterator-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.sstable_block_size = 96;
    minikv::MemTable memtable(options);
    for (std::size_t index = 0; index < 20; ++index) {
        minikv::test::Expect(
            memtable.Put(
                static_cast<std::uint64_t>(index + 1U),
                NumberedKey("key-", index),
                std::string(24, 'v')
            ).ok(),
            "iterator fixture Put must succeed"
        );
    }
    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::SSTableReader> table;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(), 1, memtable, options, environment, &table
        ).ok() && table->metadata().block_count >= 10,
        "iterator fixture must publish a multi-block SSTable"
    );

    std::unique_ptr<minikv::SSTableIterator> iterator;
    const std::string begin = NumberedKey("key-", 5);
    const std::string end = NumberedKey("key-", 10);
    minikv::test::Expect(
        table->NewIterator(begin, std::string_view(end), &iterator).ok(),
        "SSTable iterator must seek to a half-open range"
    );
    std::vector<std::string> keys;
    while (iterator->valid()) {
        const auto* record = iterator->record();
        minikv::test::Expect(record != nullptr, "valid iterator must expose a record");
        if (record != nullptr) {
            keys.push_back(record->key);
        }
        minikv::test::Expect(iterator->Next().ok(), "iterator Next must succeed");
    }
    minikv::test::Expect(
        keys == std::vector<std::string>({
            NumberedKey("key-", 5), NumberedKey("key-", 6),
            NumberedKey("key-", 7), NumberedKey("key-", 8),
            NumberedKey("key-", 9),
        }) &&
            iterator->statistics().data_blocks_read <
                table->metadata().block_count,
        "iterator must honor [begin,end) without scanning the complete table"
    );

    minikv::test::Expect(
        table->NewIterator(begin, std::string_view(begin), &iterator).ok() &&
            !iterator->valid() && iterator->record() == nullptr &&
            iterator->statistics().data_blocks_read == 0,
        "an empty range must read no SSTable data block"
    );
}

void TestHalfOpenBinaryRangesAndPrefixes() {
    TemporaryDirectory directory("/tmp/minikv-v8-bounds-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    minikv::Options invalid_options;
    invalid_options.maximum_scan_entries = 0;
    std::unique_ptr<minikv::Database> invalid_database;
    minikv::DatabaseOpenResult invalid_open_result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path() + "/invalid",
            invalid_options,
            &invalid_database,
            &invalid_open_result
        ).code() == minikv::StatusCode::kInvalidArgument &&
            invalid_database == nullptr,
        "Open must reject a zero maximum_scan_entries option"
    );
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "binary-range database must open"
    );
    const std::vector<std::string> keys = {
        "a", "aa", "ab", "b", std::string("\x7f", 1),
        std::string("\x80", 1), std::string("\xff", 1),
        std::string("\xff\x00", 2),
    };
    for (std::size_t index = 0; index < keys.size(); ++index) {
        minikv::test::Expect(
            database->Put(keys[index], "value-" + std::to_string(index)).ok(),
            "binary-range Put must succeed"
        );
    }
    minikv::test::Expect(
        database->Put("empty-value", {}).ok(),
        "an empty value must remain a successful stored value"
    );

    minikv::ScanOptions range;
    range.begin = "aa";
    range.end = "b";
    range.limit = 20;
    const auto bounded = database->Scan(range);
    minikv::test::Expect(
        bounded.status.ok() && Keys(bounded) ==
            std::vector<std::string>({"aa", "ab"}) && !bounded.truncated,
        "Scan must use inclusive begin and exclusive end"
    );
    const auto prefix = database->ScanPrefix("a", 20);
    minikv::test::Expect(
        prefix.status.ok() && Keys(prefix) ==
            std::vector<std::string>({"a", "aa", "ab"}),
        "ScanPrefix must include only bytewise prefix matches"
    );
    const auto empty_value = database->ScanPrefix("empty-value", 2);
    minikv::test::Expect(
        empty_value.status.ok() && empty_value.entries.size() == 1 &&
            empty_value.entries.front().value.empty(),
        "Scan must distinguish an empty stored value from NotFound"
    );
    const auto high_prefix = database->ScanPrefix(std::string("\xff", 1), 20);
    minikv::test::Expect(
        high_prefix.status.ok() && Keys(high_prefix) ==
            std::vector<std::string>({
                std::string("\xff", 1), std::string("\xff\x00", 2)
            }),
        "all-0xff prefixes must support an unbounded upper endpoint"
    );
    const auto all = database->LoadAll(20);
    auto sorted = keys;
    sorted.push_back("empty-value");
    std::sort(sorted.begin(), sorted.end());
    minikv::test::Expect(
        all.status.ok() && Keys(all) == sorted,
        "LoadAll must provide the same deterministic empty-prefix order"
    );

    range.begin = "same";
    range.end = "same";
    minikv::test::Expect(
        database->Scan(range).status.ok() && database->Scan(range).entries.empty(),
        "equal half-open bounds must produce an empty successful scan"
    );
    range.begin = "z";
    range.end = "a";
    minikv::test::Expect(
        database->Scan(range).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "reversed scan bounds must be rejected"
    );
    range.begin.clear();
    range.end.reset();
    range.limit = 0;
    minikv::test::Expect(
        database->Scan(range).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "zero scan limit must be rejected"
    );
    minikv::test::Expect(
        database->ScanPrefix(
            std::string(options.max_key_size + 1U, 'x'),
            1
        ).status.code() == minikv::StatusCode::kInvalidArgument,
        "a prefix exceeding max_key_size must be rejected"
    );
}

void TestGreatestSequenceAcrossEverySource() {
    TemporaryDirectory directory("/tmp/minikv-v8-sources-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "cross-source scan database must open"
    );
    minikv::test::Expect(
        database->Put("a", "l1-old").ok() &&
            database->Put("b", "l1-old").ok() &&
            database->Put("deleted", "l1-old").ok() &&
            database->Put("stable", "l1-only").ok() &&
            database->Flush().ok() && database->Compact().ok(),
        "L1 scan source must build"
    );
    minikv::test::Expect(
        database->Put("a", "l0-new").ok() && database->Delete("b").ok() &&
            database->Delete("deleted").ok() &&
            database->Put("c", "l0-value").ok() && database->Flush().ok(),
        "L0 scan source must build"
    );
    minikv::test::Expect(
        database->Put("a", "mutable-newest").ok() &&
            database->Delete("c").ok() && database->Put("d", "mutable").ok(),
        "Mutable scan source must build"
    );

    const auto result = database->ScanPrefix({}, 100);
    minikv::test::Expect(
        result.status.ok() && Keys(result) ==
            std::vector<std::string>({"a", "d", "stable"}) &&
            result.entries.size() == 3 &&
            result.entries.front().value == "mutable-newest" &&
            result.statistics.tables_considered == 2 &&
            result.statistics.data_blocks_read > 0 &&
            result.statistics.obsolete_versions_skipped >= 4 &&
            result.statistics.tombstones_skipped == 3,
        "scan merge must apply greatest-Sequence and hide winning tombstones"
    );
}

void TestImmutableRemainsScannableAfterFlushFailure() {
    TemporaryDirectory directory("/tmp/minikv-v8-immutable-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    auto environment =
        std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, environment, &database),
        "immutable scan database must open"
    );
    minikv::test::Expect(
        database->Put("pending:a", "one").ok() &&
            database->Put("pending:b", "two").ok(),
        "immutable scan fixture writes must succeed"
    );
    environment->Arm(minikv::test::FlushFailurePoint::kRename);
    minikv::test::Expect(
        database->Flush().code() == minikv::StatusCode::kIOError &&
            database->has_immutable(),
        "injected Flush failure must retain an Immutable MemTable"
    );
    const auto scan = database->ScanPrefix("pending:", 10);
    minikv::test::Expect(
        scan.status.ok() && Keys(scan) ==
            std::vector<std::string>({"pending:a", "pending:b"}),
        "failed-Flush Immutable records must remain visible to Scan"
    );
    environment->Disarm();
    minikv::test::Expect(database->Flush().ok(), "failed Flush must remain retryable");
}

void TestPaginationTokenIntegrityRestartAndStaleness() {
    TemporaryDirectory directory("/tmp/minikv-v8-token-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.maximum_scan_entries = 10;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "pagination database must open"
    );
    for (std::size_t index = 0; index < 26; ++index) {
        minikv::test::Expect(
            database->Put(NumberedKey("p:", index), std::to_string(index)).ok(),
            "pagination Put must succeed"
        );
    }
    const auto first = database->ScanPrefix("p:", 7);
    minikv::test::Expect(
        first.status.ok() && first.entries.size() == 7 && first.truncated &&
            !first.continuation_token.empty(),
        "first page must be strictly limited and return a token"
    );

    for (std::size_t cut = 1; cut < first.continuation_token.size(); ++cut) {
        const auto truncated = database->ScanPrefix(
            "p:", 7, first.continuation_token.substr(0, cut)
        );
        minikv::test::Expect(
            truncated.status.code() == minikv::StatusCode::kInvalidArgument &&
                truncated.entries.empty(),
            "every truncated continuation token must be rejected"
        );
    }
    for (std::size_t index = 0; index < first.continuation_token.size(); ++index) {
        std::string damaged = first.continuation_token;
        damaged[index] ^= static_cast<char>(0x01);
        const auto rejected = database->ScanPrefix("p:", 7, damaged);
        minikv::test::Expect(
            rejected.status.code() == minikv::StatusCode::kInvalidArgument &&
                rejected.entries.empty(),
            "every one-byte token mutation must be rejected"
        );
    }
    minikv::test::Expect(
        database->ScanPrefix(
            "p:", 7, first.continuation_token + std::string("x")
        ).status.code() == minikv::StatusCode::kInvalidArgument,
        "a continuation token with appended bytes must be rejected"
    );
    minikv::test::Expect(
        database->ScanPrefix("q:", 7, first.continuation_token).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "a token must be bound to its original range"
    );
    minikv::ScanOptions too_many;
    too_many.limit = 11;
    minikv::test::Expect(
        database->Scan(too_many).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "configured maximum_scan_entries must be enforced"
    );

    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "unchanged database must reopen for token continuation"
    );
    const auto second_after_restart = database->ScanPrefix(
        "p:", 5, first.continuation_token
    );
    minikv::test::Expect(
        second_after_restart.status.ok() &&
            !second_after_restart.entries.empty() &&
            second_after_restart.entries.front().key == NumberedKey("p:", 7),
        "an unchanged database must accept a token after restart"
    );

    std::vector<std::string> paged = Keys(first);
    std::string token = first.continuation_token;
    std::size_t page_number = 0;
    while (!token.empty()) {
        const std::size_t limit = page_number % 2U == 0 ? 5U : 9U;
        const auto page = database->ScanPrefix("p:", limit, token);
        minikv::test::Expect(page.status.ok(), "every continuation page must succeed");
        const auto page_keys = Keys(page);
        paged.insert(paged.end(), page_keys.begin(), page_keys.end());
        token = page.continuation_token;
        ++page_number;
    }
    std::vector<std::string> expected;
    for (std::size_t index = 0; index < 26; ++index) {
        expected.push_back(NumberedKey("p:", index));
    }
    minikv::test::Expect(
        paged == expected,
        "pagination must return every key exactly once in deterministic order"
    );

    minikv::test::Expect(database->Put("p:new", "mutation").ok(), "mutation must work");
    minikv::test::Expect(
        database->ScanPrefix("p:", 7, first.continuation_token).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "a write must make an older continuation token explicitly stale"
    );
}

void TestCompactionMakesTokenStale() {
    TemporaryDirectory directory("/tmp/minikv-v8-token-version-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "Version-token database must open"
    );
    for (std::size_t index = 0; index < 8; ++index) {
        minikv::test::Expect(
            database->Put(NumberedKey("v:", index), "value").ok(),
            "Version-token Put must succeed"
        );
    }
    minikv::test::Expect(database->Flush().ok(), "Version-token Flush must succeed");
    const auto first = database->ScanPrefix("v:", 3);
    minikv::test::Expect(first.status.ok() && first.truncated, "token fixture must page");
    minikv::test::Expect(database->Compact().ok(), "token fixture Compact must succeed");
    minikv::test::Expect(
        database->ScanPrefix("v:", 3, first.continuation_token).status.code() ==
            minikv::StatusCode::kInvalidArgument,
        "a Version change must make an older token explicitly stale"
    );
}

void VerifyAgainstReference(
    const minikv::Database& database,
    const std::map<std::string, std::optional<std::string>>& reference
) {
    std::vector<std::pair<std::string, std::string>> actual;
    std::string token;
    std::size_t page = 0;
    do {
        const auto result = database.ScanPrefix({}, 1U + page % 13U, token);
        minikv::test::Expect(result.status.ok(), "reference-model page must succeed");
        for (const auto& entry : result.entries) {
            actual.emplace_back(entry.key, entry.value);
        }
        token = result.continuation_token;
        ++page;
    } while (!token.empty());

    std::vector<std::pair<std::string, std::string>> expected;
    for (const auto& [key, value] : reference) {
        if (value.has_value()) {
            expected.emplace_back(key, *value);
        }
    }
    minikv::test::Expect(
        actual == expected,
        "paged scan must equal the ordered reference model"
    );
}

void TestRandomModelAcrossFlushCompactionAndRestart() {
    TemporaryDirectory directory("/tmp/minikv-v8-model-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    options.maximum_scan_entries = 32;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "range-scan model database must open"
    );
    std::map<std::string, std::optional<std::string>> reference;
    for (std::size_t operation = 0; operation < 500; ++operation) {
        const std::string key = NumberedKey("item-", operation % 41U);
        if (operation % 7U == 0) {
            minikv::test::Expect(database->Delete(key).ok(), "model Delete must work");
            reference[key] = std::nullopt;
        } else {
            const std::string value = "value-" + std::to_string(operation);
            minikv::test::Expect(database->Put(key, value).ok(), "model Put must work");
            reference[key] = value;
        }
        if (operation % 37U == 36U) {
            minikv::test::Expect(database->Flush().ok(), "model Flush must work");
        }
        if (operation == 249U) {
            minikv::test::Expect(database->Compact().ok(), "mid-model Compact must work");
        }
    }
    minikv::test::Expect(
        database->Flush().ok() && database->Compact().ok(),
        "final model maintenance must succeed"
    );
    VerifyAgainstReference(*database, reference);

    minikv::ScanOptions middle;
    middle.begin = "item-010";
    middle.end = "item-030";
    middle.limit = 32;
    const auto range = database->Scan(middle);
    minikv::test::Expect(
        range.status.ok() && std::all_of(
            range.entries.begin(),
            range.entries.end(),
            [](const minikv::ScanEntry& entry) {
                return entry.key >= "item-010" && entry.key < "item-030";
            }
        ),
        "model range must honor both half-open bounds"
    );

    database.reset();
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "range-scan model database must reopen"
    );
    VerifyAgainstReference(*database, reference);
}

void TestPostOpenTableCorruptionReturnsNoPartialScan() {
    TemporaryDirectory directory("/tmp/minikv-v8-corruption-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    std::unique_ptr<minikv::Database> database;
    minikv::test::Expect(
        OpenDatabase(directory.path(), options, &database),
        "scan-corruption database must open"
    );
    minikv::test::Expect(
        database->Put("corrupt:a", "one").ok() &&
            database->Put("corrupt:b", "two").ok() && database->Flush().ok(),
        "scan-corruption fixture must flush"
    );
    fs::path table_path;
    for (const auto& entry : fs::directory_iterator(directory.path())) {
        if (entry.path().extension() == ".sst") {
            table_path = entry.path();
            break;
        }
    }
    std::fstream file(table_path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(static_cast<std::streamoff>(minikv::kTableHeaderSize));
    char byte = 0;
    file.read(&byte, 1);
    byte ^= static_cast<char>(0x01);
    file.seekp(static_cast<std::streamoff>(minikv::kTableHeaderSize));
    file.write(&byte, 1);
    file.flush();
    minikv::test::Expect(file.good(), "live SSTable corruption must be injected");

    const auto result = database->ScanPrefix("corrupt:", 10);
    minikv::test::Expect(
        result.status.code() == minikv::StatusCode::kCorruption &&
            result.entries.empty() && !result.truncated &&
            result.continuation_token.empty(),
        "scan corruption must never be converted to empty or partial success"
    );
}

}  // namespace

int main() {
    TestSSTableIteratorSeeksAndRetainsOneBlock();
    TestHalfOpenBinaryRangesAndPrefixes();
    TestGreatestSequenceAcrossEverySource();
    TestImmutableRemainsScannableAfterFlushFailure();
    TestPaginationTokenIntegrityRestartAndStaleness();
    TestCompactionMakesTokenStale();
    TestRandomModelAcrossFlushCompactionAndRestart();
    TestPostOpenTableCorruptionReturnsNoPartialScan();
    return minikv::test::Finish("V8 Range Scan");
}
