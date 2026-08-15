#include "minikv/flush.hpp"
#include "minikv/table.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
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
        minikv::test::Expect(result != nullptr, "temporary directory must be created");
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

minikv::Options SmallBlocks() {
    minikv::Options options;
    options.sstable_block_size = 100;
    return options;
}

minikv::MemTable SampleMemTable(minikv::Options options = {}) {
    minikv::MemTable table(options);
    minikv::test::Expect(table.Put(1, "alpha", "one").ok(), "sample Put must work");
    minikv::test::Expect(table.Delete(2, "beta").ok(), "sample Delete must work");
    minikv::test::Expect(table.Put(3, "gamma", "three").ok(), "sample Put must work");
    return table;
}

bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

void TestEmptyDuplicateTombstoneAndDiskLookup() {
    TemporaryDirectory directory("/tmp/minikv-v4-table-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const auto options = SmallBlocks();
    std::string encoded;
    minikv::MemTable empty(options);
    minikv::test::Expect(
        minikv::EncodeTable(1, empty, options, &encoded).code() ==
            minikv::StatusCode::kInvalidArgument,
        "empty SSTable must be rejected"
    );

    minikv::MemTable single(options);
    minikv::test::Expect(single.Put(1, "only", "value").ok(),
                         "single-key fixture must accept Put");
    minikv::test::Expect(
        minikv::EncodeTable(2, single, options, &encoded).ok(),
        "single-key SSTable must encode"
    );
    const fs::path single_path = fs::path(directory.path()) / "single.sst";
    minikv::test::Expect(WriteFile(single_path, encoded),
                         "single-key SSTable must be written");
    std::unique_ptr<minikv::SSTableReader> single_reader;
    minikv::test::Expect(
        minikv::SSTableReader::Open(
            single_path.string(), options, &single_reader
        ).ok() && single_reader->metadata().block_count == 1 &&
            single_reader->Get("only").value == "value",
        "single-key SSTable must reopen through one block"
    );

    minikv::MemTable table(options);
    minikv::test::Expect(table.Put(1, "a", "old").ok(), "first duplicate Put works");
    minikv::test::Expect(table.Put(2, "a", "new").ok(), "newer duplicate replaces old");
    minikv::test::Expect(table.Put(3, "b", "two").ok(), "Put b works");
    minikv::test::Expect(table.Put(4, "c", "three").ok(), "Put c works");
    minikv::test::Expect(table.Delete(5, "d").ok(), "Delete d works");
    minikv::test::Expect(table.Put(6, "e", "five").ok(), "Put e works");
    minikv::test::Expect(table.Put(7, "f", "six").ok(), "Put f works");
    minikv::test::Expect(table.Put(8, "g", "seven").ok(), "Put g works");

    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::SSTableReader> reader;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(), 9, table, options, environment, &reader
        ).ok(),
        "block-oriented SSTable publication must succeed"
    );
    if (reader == nullptr) {
        return;
    }
    const auto& metadata = reader->metadata();
    minikv::test::Expect(
        metadata.generation == 9 && metadata.record_count == 7 &&
            metadata.minimum_sequence == 2 && metadata.maximum_sequence == 8,
        "Footer summaries must match the visible generation"
    );
    minikv::test::Expect(
        metadata.block_count >= 3 && metadata.minimum_key == "a" &&
            metadata.maximum_key == "g",
        "small block target must create a multi-block indexed table"
    );

    minikv::SSTableReadStats stats;
    auto result = reader->Get("a", &stats);
    minikv::test::Expect(
        result.found() && result.value == "new" && result.sequence == 2,
        "duplicate key must retain only its newest record"
    );
    minikv::test::Expect(
        stats.data_blocks_read == 1,
        "an indexed hit must read exactly one candidate data block"
    );
    result = reader->Get("g", &stats);
    minikv::test::Expect(
        result.found() && result.value == "seven" &&
            stats.data_blocks_read == 1,
        "the final key of the final block must be found with one read"
    );
    result = reader->Get("d", &stats);
    minikv::test::Expect(result.deleted(), "tombstone must survive SSTable lookup");
    minikv::test::Expect(
        stats.data_blocks_read == 1,
        "a tombstone lookup must read exactly one candidate block"
    );
    result = reader->Get("bb", &stats);
    minikv::test::Expect(
        result.status.IsNotFound() && stats.bloom_filter_checks == 1 &&
            stats.data_blocks_read + stats.bloom_filter_rejections == 1 &&
            stats.bloom_false_positives == stats.data_blocks_read,
        "an in-range miss must be Bloom-rejected or read one candidate block"
    );
    result = reader->Get("z", &stats);
    minikv::test::Expect(
        result.status.IsNotFound() && stats.range_rejections == 1 &&
            stats.bloom_filter_checks == 0 && stats.data_blocks_read == 0,
        "range rejection must avoid every data-block read"
    );

    std::vector<minikv::MemTableRecord> records;
    minikv::test::Expect(
        reader->ReadRecords(3, &records).ok() && records.size() == 3 &&
            records.front().key == "a" && records.back().key == "c",
        "diagnostic scan must honor key order and record limit"
    );
}

void TestEveryByteAndRegionCorruption() {
    TemporaryDirectory directory("/tmp/minikv-v4-corruption-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const auto options = SmallBlocks();
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeTable(7, SampleMemTable(options), options, &encoded).ok(),
        "corruption fixture must encode"
    );
    minikv::test::Expect(
        encoded.substr(0, 4) == "MKST" &&
            static_cast<std::uint8_t>(encoded[4]) == minikv::kTableFormatVersion,
        "V4 SSTable magic and version must be stable"
    );

    const fs::path path = fs::path(directory.path()) / "damaged.sst";
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        std::string damaged = encoded;
        damaged[index] ^= static_cast<char>(0x01);
        minikv::test::Expect(WriteFile(path, damaged), "damaged table must be written");
        std::unique_ptr<minikv::SSTableReader> reader;
        minikv::test::Expect(
            minikv::SSTableReader::Open(path.string(), options, &reader).code() ==
                minikv::StatusCode::kCorruption,
            "every one-byte mutation in Header, block, index, or Footer must fail"
        );
        minikv::test::Expect(
            reader == nullptr,
            "corrupt SSTable must never expose a partial reader"
        );
    }

    minikv::test::Expect(WriteFile(path, encoded.substr(0, encoded.size() - 1)),
                         "truncated table must be written");
    std::unique_ptr<minikv::SSTableReader> reader;
    minikv::test::Expect(
        minikv::SSTableReader::Open(path.string(), options, &reader).code() ==
            minikv::StatusCode::kCorruption,
        "truncated Footer must be rejected"
    );
}

void TestRandomSortedTableAgainstReferenceModel() {
    TemporaryDirectory directory("/tmp/minikv-v4-random-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    auto options = SmallBlocks();
    minikv::MemTable table(options);
    std::map<std::string, std::string> reference;
    std::mt19937_64 random(0x4456455253494F4EULL);
    std::uint64_t sequence = 0;
    for (int step = 0; step < 400; ++step) {
        const std::string key = "key-" + std::to_string(random() % 137U);
        ++sequence;
        if (random() % 5U == 0) {
            minikv::test::Expect(table.Delete(sequence, key).ok(), "random Delete works");
            reference.erase(key);
        } else {
            const std::string value = "value-" + std::to_string(random());
            minikv::test::Expect(table.Put(sequence, key, value).ok(), "random Put works");
            reference[key] = value;
        }
    }

    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::SSTableReader> reader;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(), 17, table, options, environment, &reader
        ).ok(),
        "random table publication must succeed"
    );
    if (reader == nullptr) {
        return;
    }
    for (std::size_t id = 0; id < 137; ++id) {
        const std::string key = "key-" + std::to_string(id);
        const auto expected = reference.find(key);
        const auto actual = reader->Get(key);
        minikv::test::Expect(
            expected == reference.end()
                ? actual.status.IsNotFound() || actual.deleted()
                : actual.found() && actual.value == expected->second,
            "disk point lookup must match the random reference model"
        );
    }
}

void TestPublishCreatesOnlyFinalSyncedTable() {
    TemporaryDirectory directory("/tmp/minikv-v4-publish-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::SSTableReader> published;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(), 11, SampleMemTable(), {}, environment, &published
        ).ok(),
        "POSIX table publication must succeed"
    );
    const fs::path final_path =
        fs::path(directory.path()) / minikv::GenerationFileName(11, "sst");
    minikv::test::Expect(fs::exists(final_path), "final table must exist");
    minikv::test::Expect(
        !fs::exists(final_path.string() + ".tmp"),
        "temporary table must not remain after publication"
    );
    minikv::test::Expect(
        published != nullptr && published->Get("gamma").value == "three",
        "publication must return the disk-backed immutable reader"
    );
}

void TestPublishFailureOutputs() {
    TemporaryDirectory directory("/tmp/minikv-v4-table-fail-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const minikv::test::FlushFailurePoint failures[] = {
        minikv::test::FlushFailurePoint::kCreate,
        minikv::test::FlushFailurePoint::kWrite,
        minikv::test::FlushFailurePoint::kFileSync,
        minikv::test::FlushFailurePoint::kRename,
        minikv::test::FlushFailurePoint::kDirectorySync,
    };
    for (const auto failure : failures) {
        const std::uint64_t generation =
            static_cast<std::uint64_t>(20) + static_cast<std::uint64_t>(failure);
        const fs::path final_path = fs::path(directory.path()) /
            minikv::GenerationFileName(generation, "sst");
        auto environment =
            std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
        environment->Arm(failure);
        std::unique_ptr<minikv::SSTableReader> published;
        minikv::test::Expect(
            minikv::PublishTable(
                directory.path(), generation, SampleMemTable(), {},
                *environment, &published
            ).code() == minikv::StatusCode::kIOError,
            "each publication failure point must propagate"
        );
        minikv::test::Expect(
            published == nullptr,
            "failed publication must not expose a table"
        );
        minikv::test::Expect(
            fs::exists(final_path) ==
                (failure == minikv::test::FlushFailurePoint::kDirectorySync),
            "final table name may appear only after rename succeeds"
        );
        minikv::test::Expect(
            !fs::exists(final_path.string() + ".tmp"),
            "handled publication failures must clean the temporary table"
        );
    }
}

}  // namespace

int main() {
    TestEmptyDuplicateTombstoneAndDiskLookup();
    TestEveryByteAndRegionCorruption();
    TestRandomSortedTableAgainstReferenceModel();
    TestPublishCreatesOnlyFinalSyncedTable();
    TestPublishFailureOutputs();
    return minikv::test::Finish("V4 SSTable and flush");
}
