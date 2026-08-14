#include "minikv/bloom_filter.hpp"
#include "minikv/coding.hpp"
#include "minikv/database.hpp"
#include "minikv/table.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

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

std::string NumberedKey(std::string_view prefix, std::uint64_t number) {
    return std::string(prefix) + std::to_string(1000000000ULL + number);
}

bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

void PutFixed32At(std::string* bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        (*bytes)[offset + index] = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
}

void AddStats(
    const minikv::SSTableReadStats& source,
    minikv::SSTableReadStats* destination
) {
    destination->tables_considered += source.tables_considered;
    destination->range_rejections += source.range_rejections;
    destination->bloom_filter_checks += source.bloom_filter_checks;
    destination->bloom_filter_rejections += source.bloom_filter_rejections;
    destination->bloom_false_positives += source.bloom_false_positives;
    destination->data_blocks_read += source.data_blocks_read;
    destination->bytes_read += source.bytes_read;
}

void TestParameterDerivationRoundTripAndCorruption() {
    minikv::BloomFilter filter;
    minikv::test::Expect(
        minikv::BloomFilter::Create(10000, 0.01, &filter).ok() &&
            filter.bit_count() == 95856 && filter.hash_count() == 7,
        "one-percent target must derive the expected bit and hash counts"
    );
    for (std::uint64_t index = 0; index < 10000; ++index) {
        minikv::test::Expect(
            filter.Add(NumberedKey("present-", index)).ok(),
            "Bloom insertion must succeed"
        );
    }
    const std::string binary_key("binary\0key", 10);
    minikv::test::Expect(
        filter.Add(binary_key).ok() && filter.MayContain(binary_key),
        "Bloom hashing must preserve embedded NUL bytes"
    );
    for (std::uint64_t index = 0; index < 10000; ++index) {
        minikv::test::Expect(
            filter.MayContain(NumberedKey("present-", index)),
            "an inserted key must never be rejected"
        );
    }

    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeBloomFilter(filter, &encoded).ok(),
        "Bloom serialization must succeed"
    );
    minikv::BloomFilter decoded;
    minikv::test::Expect(
        minikv::DecodeBloomFilter(encoded, &decoded).ok() &&
            decoded.bit_count() == filter.bit_count() &&
            decoded.hash_count() == filter.hash_count() &&
            decoded.key_count() == filter.key_count(),
        "Bloom serialization must preserve its parameters"
    );
    for (std::uint64_t index = 0; index < 10000; ++index) {
        minikv::test::Expect(
            decoded.MayContain(NumberedKey("present-", index)),
            "serialized Bloom data must preserve every possible hit"
        );
    }

    for (std::size_t index = 0; index < encoded.size(); ++index) {
        std::string damaged = encoded;
        damaged[index] ^= static_cast<char>(0x01);
        minikv::BloomFilter rejected;
        minikv::test::Expect(
            minikv::DecodeBloomFilter(damaged, &rejected).code() ==
                minikv::StatusCode::kCorruption,
            "every one-byte Bloom mutation must be rejected"
        );
    }

    minikv::BloomFilter empty;
    minikv::test::Expect(
        minikv::BloomFilter::Create(0, 0.01, &empty).ok() &&
            !empty.MayContain("anything") &&
            minikv::EncodeBloomFilter(empty, &encoded).ok() &&
            encoded.size() == minikv::kBloomFilterHeaderSize &&
            minikv::DecodeBloomFilter(encoded, &decoded).ok() &&
            !decoded.MayContain("anything"),
        "an empty Bloom filter must round-trip and reject every key"
    );
    minikv::test::Expect(
        minikv::BloomFilter::Create(1, 0.0, &decoded).code() ==
                minikv::StatusCode::kInvalidArgument &&
            minikv::BloomFilter::Create(1, 1.0, &decoded).code() ==
                minikv::StatusCode::kInvalidArgument &&
            minikv::BloomFilter::Create(
                1,
                std::numeric_limits<double>::quiet_NaN(),
                &decoded
            ).code() == minikv::StatusCode::kInvalidArgument,
        "invalid false-positive targets must be rejected"
    );
}

void TestOneHundredThousandAbsentKeys() {
    minikv::BloomFilter filter;
    minikv::test::Expect(
        minikv::BloomFilter::Create(10000, 0.01, &filter).ok(),
        "false-positive experiment filter must be created"
    );
    for (std::uint64_t index = 0; index < 10000; ++index) {
        minikv::test::Expect(
            filter.Add(NumberedKey("inserted-", index)).ok(),
            "false-positive experiment insertion must succeed"
        );
    }
    std::uint64_t false_positives = 0;
    for (std::uint64_t index = 0; index < 100000; ++index) {
        if (filter.MayContain(NumberedKey("absent-", index))) {
            ++false_positives;
        }
    }
    const double measured_rate =
        static_cast<double>(false_positives) / 100000.0;
    std::cout << "Bloom experiment: " << false_positives
              << "/100000 false positives (" << measured_rate << ")\n";
    minikv::test::Expect(
        false_positives < 2000,
        "measured false-positive rate must stay below two percent"
    );
}

std::uint64_t RunAbsentTableQueries(
    minikv::SSTableReader& reader,
    minikv::SSTableReadStats* totals
) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < 5000; ++index) {
        minikv::SSTableReadStats operation;
        const auto result = reader.Get(NumberedKey("key-", index * 2U + 1U), &operation);
        minikv::test::Expect(
            result.status.IsNotFound(),
            "every odd key in the table experiment must be absent"
        );
        AddStats(operation, totals);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
    );
}

void TestTableReadReductionAndSemanticFalseNegativeCheck() {
    TemporaryDirectory directory("/tmp/minikv-v6-bloom-table-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options enabled_options;
    enabled_options.sstable_block_size = 256;
    minikv::MemTable table(enabled_options);
    for (std::uint64_t index = 0; index < 5000; ++index) {
        minikv::test::Expect(
            table.Put(
                index + 1U,
                NumberedKey("key-", index * 2U),
                NumberedKey("value-", index)
            ).ok(),
            "table experiment insertion must succeed"
        );
    }

    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeTable(1, table, enabled_options, &encoded).ok(),
        "Bloom-enabled SSTable must encode"
    );
    const fs::path path = fs::path(directory.path()) / "enabled.sst";
    minikv::test::Expect(WriteFile(path, encoded), "experiment SSTable must be written");

    std::unique_ptr<minikv::SSTableReader> enabled;
    std::unique_ptr<minikv::SSTableReader> disabled;
    minikv::Options disabled_options = enabled_options;
    disabled_options.bloom_filter_enabled = false;
    minikv::test::Expect(
        minikv::SSTableReader::Open(path.string(), enabled_options, &enabled).ok() &&
            minikv::SSTableReader::Open(path.string(), disabled_options, &disabled).ok() &&
            enabled->metadata().bloom_size > minikv::kBloomFilterHeaderSize &&
            enabled->metadata().bloom_hash_count > 0,
        "SSTable Open must load verified Bloom metadata"
    );
    if (enabled == nullptr || disabled == nullptr) {
        return;
    }

    std::string encoded_without_bloom;
    minikv::test::Expect(
        minikv::EncodeTable(
            2,
            table,
            disabled_options,
            &encoded_without_bloom
        ).ok(),
        "Bloom-disabled SSTable construction must remain supported"
    );
    const std::size_t disabled_footer_offset =
        encoded_without_bloom.size() - minikv::kTableFooterSize;
    std::uint64_t disabled_bloom_size = 1;
    minikv::test::Expect(
        minikv::DecodeFixed64(
            std::string_view(encoded_without_bloom).substr(
                disabled_footer_offset + 88
            ),
            &disabled_bloom_size
        ) && disabled_bloom_size == 0,
        "Bloom-disabled SSTable must publish an empty Bloom region"
    );

    minikv::SSTableReadStats enabled_stats;
    minikv::SSTableReadStats disabled_stats;
    const std::uint64_t enabled_us = RunAbsentTableQueries(*enabled, &enabled_stats);
    const std::uint64_t disabled_us = RunAbsentTableQueries(*disabled, &disabled_stats);
    std::cout << "SSTable absent-read experiment: Bloom on " << enabled_us
              << " us / " << enabled_stats.data_blocks_read
              << " blocks, Bloom off " << disabled_us << " us / "
              << disabled_stats.data_blocks_read << " blocks\n";
    minikv::test::Expect(
        enabled_stats.bloom_filter_checks == 4999 &&
            enabled_stats.data_blocks_read < disabled_stats.data_blocks_read / 10U &&
            enabled_stats.bloom_filter_rejections +
                    enabled_stats.bloom_false_positives ==
                enabled_stats.bloom_filter_checks &&
            disabled_stats.bloom_filter_checks == 0 &&
            disabled_stats.data_blocks_read == 4999,
        "Bloom must eliminate most in-range absent-key data-block reads"
    );

    const std::size_t footer_offset = encoded.size() - minikv::kTableFooterSize;
    std::uint64_t bloom_offset = 0;
    std::uint64_t bloom_size = 0;
    minikv::test::Expect(
        minikv::DecodeFixed64(
            std::string_view(encoded).substr(footer_offset + 80),
            &bloom_offset
        ) && minikv::DecodeFixed64(
            std::string_view(encoded).substr(footer_offset + 88),
            &bloom_size
        ) && bloom_size > minikv::kBloomFilterHeaderSize,
        "Footer must locate the Bloom region"
    );
    if (bloom_size <= minikv::kBloomFilterHeaderSize) {
        return;
    }
    std::fill(
        encoded.begin() + static_cast<std::ptrdiff_t>(bloom_offset) +
            static_cast<std::ptrdiff_t>(minikv::kBloomFilterHeaderSize),
        encoded.begin() + static_cast<std::ptrdiff_t>(bloom_offset + bloom_size),
        '\0'
    );
    std::string checksum_input = encoded.substr(
        static_cast<std::size_t>(bloom_offset),
        28
    );
    checksum_input.append(encoded.substr(
        static_cast<std::size_t>(bloom_offset) + minikv::kBloomFilterHeaderSize,
        static_cast<std::size_t>(bloom_size) - minikv::kBloomFilterHeaderSize
    ));
    PutFixed32At(
        &encoded,
        static_cast<std::size_t>(bloom_offset) + 28,
        minikv::Crc32c(checksum_input)
    );
    const fs::path false_negative_path =
        fs::path(directory.path()) / "false-negative.sst";
    minikv::test::Expect(
        WriteFile(false_negative_path, encoded),
        "semantic false-negative fixture must be written"
    );
    std::unique_ptr<minikv::SSTableReader> rejected;
    minikv::test::Expect(
        minikv::SSTableReader::Open(
            false_negative_path.string(), enabled_options, &rejected
        ).code() == minikv::StatusCode::kCorruption && rejected == nullptr,
        "Open must reject a checksummed Bloom filter with false negatives"
    );
}

void TestDatabaseOperationAndCumulativeStatistics() {
    TemporaryDirectory directory("/tmp/minikv-v6-database-stats-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::Options options;
    options.memtable_size_limit = 1024U * 1024U;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult open_result;
    minikv::test::Expect(
        minikv::Database::Open(
            directory.path(), options, &database, &open_result
        ).ok() && database->Put("a", "one").ok() &&
            database->Put("z", "last").ok(),
        "statistics database fixture must open and accept records"
    );
    if (database == nullptr) {
        return;
    }

    minikv::DatabaseReadStats operation;
    minikv::test::Expect(
        database->Get("a", &operation).value == "one" &&
            operation.point_lookups == 1 && operation.mutable_hits == 1 &&
            operation.tables_considered == 0,
        "a Mutable hit must report one lookup without touching SSTables"
    );
    minikv::test::Expect(database->Flush().ok(), "statistics fixture must flush");
    minikv::test::Expect(
        database->Get("a", &operation).value == "one" &&
            operation.point_lookups == 1 && operation.tables_considered == 1 &&
            operation.bloom_filter_checks == 1 &&
            operation.data_blocks_read == 1,
        "a disk hit must expose Bloom and physical block statistics"
    );
    minikv::test::Expect(
        database->Get("zz", &operation).status.IsNotFound() &&
            operation.range_rejections == 1 &&
            operation.bloom_filter_checks == 0 &&
            operation.data_blocks_read == 0,
        "an out-of-range database miss must be counted before Bloom"
    );

    bool observed_bloom_rejection = false;
    std::uint64_t in_range_queries = 0;
    for (; in_range_queries < 100; ++in_range_queries) {
        const auto result = database->Get(
            NumberedKey("m-missing-", in_range_queries),
            &operation
        );
        minikv::test::Expect(
            result.status.IsNotFound() && operation.tables_considered == 1 &&
                operation.bloom_filter_checks == 1,
            "an in-range database miss must consult the Bloom filter"
        );
        if (operation.bloom_filter_rejections == 1) {
            observed_bloom_rejection = true;
            ++in_range_queries;
            break;
        }
    }
    const auto cumulative = database->read_statistics();
    minikv::test::Expect(
        observed_bloom_rejection &&
            cumulative.point_lookups == 3U + in_range_queries &&
            cumulative.mutable_hits == 1 &&
            cumulative.bloom_filter_rejections >= 1 &&
            cumulative.data_blocks_read >= 1,
        "database lifetime statistics must accumulate every completed Get"
    );
}

}  // namespace

int main() {
    TestParameterDerivationRoundTripAndCorruption();
    TestOneHundredThousandAbsentKeys();
    TestTableReadReductionAndSemanticFalseNegativeCheck();
    TestDatabaseOperationAndCumulativeStatistics();
    return minikv::test::Finish("V6 Bloom filters and read statistics");
}
