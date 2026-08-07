#include "minikv/memtable.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void Expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestValidationAndBoundaries() {
    minikv::Options options;
    options.max_key_size = 4;
    options.max_value_size = 5;
    minikv::MemTable table(options);

    Expect(
        table.Put(1, "", "value").code() == minikv::StatusCode::kInvalidArgument,
        "empty keys must be rejected"
    );
    Expect(
        table.Put(1, "12345", "value").code() == minikv::StatusCode::kInvalidArgument,
        "oversized keys must be rejected"
    );
    Expect(
        table.Put(1, "key", "123456").code() == minikv::StatusCode::kInvalidArgument,
        "oversized values must be rejected"
    );
    Expect(
        table.Put(0, "key", "value").code() == minikv::StatusCode::kInvalidArgument,
        "sequence zero must be rejected"
    );
    Expect(
        table.Get("").status.code() == minikv::StatusCode::kInvalidArgument,
        "Get must reject an empty key"
    );
    Expect(
        table.Get("12345").status.code() == minikv::StatusCode::kInvalidArgument,
        "Get must reject an oversized key"
    );
    Expect(table.Put(1, "1234", "12345").ok(), "boundary-sized data must be accepted");
    Expect(table.Put(2, "zero", "").ok(), "empty values must be accepted");
}

void TestBinaryData() {
    minikv::MemTable table;
    const std::string key("a\0b", 3);
    const std::string value("x\0y\0z", 5);

    Expect(table.Put(1, key, value).ok(), "binary Put must succeed");
    const auto result = table.Get(key);
    Expect(result.found(), "binary key must be found");
    Expect(result.value == value, "binary value must round-trip without truncation");
    Expect(result.sequence == 1, "lookup must return the stored sequence number");
}

void TestOverwriteDeleteAndRewrite() {
    minikv::MemTable table;

    Expect(table.Put(1, "key", "old").ok(), "initial Put must succeed");
    Expect(table.Put(2, "key", "new").ok(), "newer Put must replace the old value");
    Expect(table.Get("key").value == "new", "largest sequence number must be visible");
    Expect(
        table.Put(2, "key", "conflict").code() == minikv::StatusCode::kInvalidArgument,
        "duplicate sequence numbers must be rejected"
    );
    Expect(
        table.Put(1, "key", "stale").code() == minikv::StatusCode::kInvalidArgument,
        "stale sequence numbers must be rejected"
    );

    Expect(table.Delete(3, "key").ok(), "Delete must append a newer tombstone");
    const auto tombstone = table.Lookup("key");
    Expect(tombstone.found(), "internal lookup must expose a tombstone");
    Expect(tombstone.deleted(), "internal lookup must identify a tombstone");
    Expect(tombstone.sequence == 3, "tombstone must preserve its sequence number");
    Expect(table.Get("key").status.IsNotFound(), "user-visible Get must hide tombstones");

    Expect(table.Put(4, "key", "reborn").ok(), "a newer Put must supersede a tombstone");
    Expect(table.Get("key").value == "reborn", "rewritten value must become visible");
    Expect(table.size() == 1, "versions of one key occupy one V0 MemTable entry");
}

void TestSizeAccounting() {
    minikv::MemTable table;
    constexpr std::size_t metadata_size = sizeof(std::uint64_t) + sizeof(minikv::ValueType);

    Expect(table.Put(1, "abc", "1234").ok(), "size test Put must succeed");
    Expect(
        table.ApproximateDataSize() == metadata_size + 3U + 4U,
        "size must include metadata, key, and value"
    );

    Expect(table.Put(2, "abc", "x").ok(), "size test overwrite must succeed");
    Expect(
        table.ApproximateDataSize() == metadata_size + 3U + 1U,
        "overwrite must remove the old value size"
    );

    Expect(table.Delete(3, "abc").ok(), "size test Delete must succeed");
    Expect(
        table.ApproximateDataSize() == metadata_size + 3U,
        "tombstone size must not include an old value"
    );
}

std::string RandomValue(std::mt19937_64& random) {
    const auto length = static_cast<std::size_t>(random() % 32U);
    std::string value(length, '\0');
    for (char& byte : value) {
        byte = static_cast<char>(random() & 0xFFU);
    }
    return value;
}

void TestRandomOperationsAgainstReferenceModel() {
    constexpr std::size_t operation_count = 10'000;
    constexpr std::uint64_t seed = 0x4D494E494B56ULL;

    minikv::MemTable table;
    std::map<std::string, std::optional<std::string>, std::less<>> reference;
    std::mt19937_64 random(seed);

    for (std::size_t index = 0; index < operation_count; ++index) {
        std::string key = "key-" + std::to_string(random() % 128U);
        if ((random() % 7U) == 0U) {
            key.push_back('\0');
            key.push_back(static_cast<char>(random() & 0x7FU));
        }

        const auto sequence = static_cast<std::uint64_t>(index + 1U);
        if ((random() % 4U) == 0U) {
            Expect(table.Delete(sequence, key).ok(), "random Delete must succeed");
            reference[key] = std::nullopt;
        } else {
            const std::string value = RandomValue(random);
            Expect(table.Put(sequence, key, value).ok(), "random Put must succeed");
            reference[key] = value;
        }

        const auto expected = reference.find(key);
        const auto actual = table.Get(key);
        if (expected->second.has_value()) {
            Expect(actual.found(), "reference value must be visible");
            Expect(actual.value == *expected->second, "value must match the reference model");
        } else {
            Expect(actual.status.IsNotFound(), "reference deletion must read as NotFound");
        }
    }
}

}  // namespace

int main() {
    TestValidationAndBoundaries();
    TestBinaryData();
    TestOverwriteDeleteAndRewrite();
    TestSizeAccounting();
    TestRandomOperationsAgainstReferenceModel();

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }

    std::cout << "All MemTable tests passed\n";
    return 0;
}
