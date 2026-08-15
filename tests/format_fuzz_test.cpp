#include "minikv/bloom_filter.hpp"
#include "minikv/database.hpp"
#include "minikv/manifest.hpp"
#include "minikv/memtable.hpp"
#include "minikv/status.hpp"
#include "minikv/table.hpp"
#include "minikv/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <unistd.h>

#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kFuzzSeed = 0x56313046555A5A31ULL;
constexpr std::size_t kRandomInputs = 30000;
constexpr std::size_t kCorpusMutations = 12000;
constexpr std::size_t kTableMutations = 750;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) {
        path_ = pattern;
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(
            result != nullptr,
            "temporary format-fuzz directory must exist"
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

bool WriteFile(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

bool IsWalResultAllowed(const minikv::Status& status) {
    return status.ok() || status.IsIncomplete() || status.IsCorruption();
}

bool IsManifestResultAllowed(const minikv::Status& status) {
    return status.ok() || status.IsCorruption() ||
           status.IsVersionMismatch();
}

void TestStableStatusCodeContract() {
    static_assert(
        std::is_same_v<
            std::underlying_type_t<minikv::StatusCode>,
            std::uint8_t
        >
    );
    const std::vector<std::pair<minikv::StatusCode, std::string_view>> codes = {
        {minikv::StatusCode::kOk, "OK"},
        {minikv::StatusCode::kNotFound, "NotFound"},
        {minikv::StatusCode::kIncomplete, "Incomplete"},
        {minikv::StatusCode::kInvalidArgument, "InvalidArgument"},
        {minikv::StatusCode::kIOError, "IOError"},
        {minikv::StatusCode::kCorruption, "Corruption"},
        {minikv::StatusCode::kVersionMismatch, "VersionMismatch"},
        {minikv::StatusCode::kClosed, "Closed"},
    };
    bool stable = true;
    for (std::size_t index = 0; index < codes.size(); ++index) {
        stable = stable &&
            static_cast<std::uint8_t>(codes[index].first) == index &&
            minikv::StatusCodeName(codes[index].first) == codes[index].second;
    }
    stable = stable &&
        minikv::StatusCodeName(
            static_cast<minikv::StatusCode>(255)
        ) == "Unknown" &&
        minikv::Status::InvalidArgument("x").IsInvalidArgument() &&
        minikv::Status::IOError("x").IsIOError() &&
        minikv::Status::Corruption("x").IsCorruption() &&
        minikv::Status::Closed("x").IsClosed();
    minikv::test::Expect(
        stable,
        "status numbers, names, and category predicates must remain stable"
    );
}

struct GoldenFormats {
    std::string wal;
    std::string bloom;
    std::string manifest;
    std::string table;
};

GoldenFormats BuildGoldenFormats() {
    GoldenFormats formats;

    minikv::WalRecord record;
    record.type = minikv::ValueType::kValue;
    record.sequence = 7;
    record.key = std::string("golden\0key", 10);
    record.value = std::string("golden\0value", 12);
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &formats.wal).ok(),
        "golden WAL must encode"
    );

    minikv::BloomFilter filter;
    minikv::test::Expect(
        minikv::BloomFilter::Create(32, 0.01, &filter).ok() &&
            filter.Add("alpha").ok() &&
            filter.Add("omega").ok() &&
            minikv::EncodeBloomFilter(filter, &formats.bloom).ok(),
        "golden Bloom filter must encode"
    );

    minikv::test::Expect(
        minikv::EncodeManifest(
            minikv::Version::NewDatabase(),
            {},
            &formats.manifest
        ).ok(),
        "golden MANIFEST must encode"
    );

    minikv::MemTable table;
    minikv::test::Expect(
        table.Put(1, "alpha", "one").ok() &&
            table.Delete(2, "middle").ok() &&
            table.Put(3, "omega", "three").ok() &&
            minikv::EncodeTable(1, table, {}, &formats.table).ok(),
        "golden SSTable must encode"
    );
    return formats;
}

void TestGoldenFormatsAndVersionRejection(const GoldenFormats& formats) {
    const auto wal = minikv::DecodeWalRecord(formats.wal, {});
    minikv::BloomFilter bloom;
    minikv::Version version;
    minikv::test::Expect(
        wal.status.ok() && wal.bytes_consumed == formats.wal.size() &&
            minikv::DecodeBloomFilter(formats.bloom, &bloom).ok() &&
            minikv::DecodeManifest(formats.manifest, {}, &version).ok(),
        "current golden WAL, Bloom, and MANIFEST bytes must decode"
    );

    std::string old_wal = formats.wal;
    old_wal[4] = 0;
    std::string old_bloom = formats.bloom;
    old_bloom[4] = 0;
    std::string old_manifest = formats.manifest;
    old_manifest[5] = 1;
    minikv::test::Expect(
        minikv::DecodeWalRecord(old_wal, {}).status.IsCorruption() &&
            minikv::DecodeBloomFilter(old_bloom, &bloom).IsCorruption() &&
            minikv::DecodeManifest(old_manifest, {}, &version)
                .IsVersionMismatch(),
        "unsupported component bytes must be rejected and directory format "
        "must report VersionMismatch"
    );

    TemporaryDirectory directory("/tmp/minikv-v10-golden-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const fs::path path = fs::path(directory.path()) / "golden.sst";
    std::unique_ptr<minikv::SSTableReader> reader;
    minikv::test::Expect(
        WriteFile(path, formats.table) &&
            minikv::SSTableReader::Open(path.string(), {}, &reader).ok(),
        "current golden SSTable bytes must open"
    );

    std::string old_table = formats.table;
    old_table[4] = 0;
    minikv::test::Expect(
        WriteFile(path, old_table) &&
            minikv::SSTableReader::Open(path.string(), {}, &reader)
                .IsCorruption(),
        "unsupported SSTable format bytes must be rejected"
    );
}

std::string RandomBytes(std::mt19937_64& random, std::size_t maximum_size) {
    const std::size_t size = static_cast<std::size_t>(
        random() % (maximum_size + 1U)
    );
    std::string bytes(size, '\0');
    for (char& byte : bytes) {
        byte = static_cast<char>(random() & 0xFFU);
    }
    return bytes;
}

void Mutate(std::string* bytes, std::mt19937_64& random) {
    if (bytes == nullptr) {
        return;
    }
    const std::uint64_t action = random() % 5U;
    if (action == 0U && !bytes->empty()) {
        bytes->resize(
            static_cast<std::size_t>(random() % bytes->size())
        );
        return;
    }
    if (action == 1U) {
        const std::size_t appended =
            1U + static_cast<std::size_t>(random() % 16U);
        for (std::size_t index = 0; index < appended; ++index) {
            bytes->push_back(static_cast<char>(random() & 0xFFU));
        }
        return;
    }
    if (bytes->empty()) {
        bytes->push_back(static_cast<char>(random() & 0xFFU));
        return;
    }
    const std::size_t mutations =
        1U + static_cast<std::size_t>(random() % 8U);
    for (std::size_t mutation = 0; mutation < mutations; ++mutation) {
        const std::size_t index = static_cast<std::size_t>(
            random() % bytes->size()
        );
        (*bytes)[index] ^= static_cast<char>(
            1U << static_cast<unsigned int>(random() % 8U)
        );
    }
}

bool DecodeInMemoryFormats(std::string_view bytes) {
    const auto wal = minikv::DecodeWalRecord(bytes, {});
    if (!IsWalResultAllowed(wal.status) ||
        (wal.status.ok() &&
         (wal.bytes_consumed == 0U || wal.bytes_consumed > bytes.size()))) {
        return false;
    }

    minikv::BloomFilter bloom;
    const auto bloom_status = minikv::DecodeBloomFilter(bytes, &bloom);
    if (!bloom_status.ok() && !bloom_status.IsCorruption()) {
        return false;
    }

    minikv::Version version;
    return IsManifestResultAllowed(
        minikv::DecodeManifest(bytes, {}, &version)
    );
}

void TestDeterministicInMemoryFormatFuzz(const GoldenFormats& formats) {
    std::cout << "V10 format-fuzz seed: " << kFuzzSeed
              << ", random inputs: " << kRandomInputs
              << ", corpus mutations: " << kCorpusMutations << '\n';
    std::mt19937_64 random(kFuzzSeed);
    bool valid_results = true;
    for (std::size_t iteration = 0;
         iteration < kRandomInputs;
         ++iteration) {
        valid_results =
            DecodeInMemoryFormats(RandomBytes(random, 512U)) &&
            valid_results;
    }

    const std::vector<std::string> corpus = {
        formats.wal,
        formats.bloom,
        formats.manifest,
        formats.table,
    };
    for (std::size_t iteration = 0;
         iteration < kCorpusMutations;
         ++iteration) {
        std::string bytes = corpus[static_cast<std::size_t>(
            random() % corpus.size()
        )];
        Mutate(&bytes, random);
        valid_results = DecodeInMemoryFormats(bytes) && valid_results;
    }
    minikv::test::Expect(
        valid_results,
        "all deterministic decoder inputs must return a documented category"
    );
}

void TestDeterministicSSTableFuzz(const GoldenFormats& formats) {
    TemporaryDirectory directory("/tmp/minikv-v10-table-fuzz-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const fs::path path = fs::path(directory.path()) / "fuzz.sst";
    std::mt19937_64 random(kFuzzSeed ^ 0x53535441424C45ULL);
    bool valid_results = true;
    for (std::size_t iteration = 0;
         iteration < kTableMutations;
         ++iteration) {
        std::string bytes = formats.table;
        Mutate(&bytes, random);
        if (!WriteFile(path, bytes)) {
            valid_results = false;
            break;
        }
        std::unique_ptr<minikv::SSTableReader> reader;
        const auto status = minikv::SSTableReader::Open(
            path.string(),
            {},
            &reader
        );
        if (!status.ok() && !status.IsCorruption()) {
            valid_results = false;
            break;
        }
        if (status.ok()) {
            std::vector<minikv::MemTableRecord> records;
            if (!reader->ReadRecords(100, &records).ok()) {
                valid_results = false;
                break;
            }
        }
    }
    minikv::test::Expect(
        valid_results,
        "mutated SSTables must open safely or report Corruption"
    );
}

}  // namespace

int main() {
    TestStableStatusCodeContract();
    const auto formats = BuildGoldenFormats();
    TestGoldenFormatsAndVersionRejection(formats);
    TestDeterministicInMemoryFormatFuzz(formats);
    TestDeterministicSSTableFuzz(formats);
    return minikv::test::Finish("V10 Format Fuzz");
}
