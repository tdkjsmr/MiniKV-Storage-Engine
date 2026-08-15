#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/options.hpp"
#include "minikv/status.hpp"

namespace minikv {

enum class ValueType : std::uint8_t {
    kValue = 1,
    kDeletion = 2,
};

struct LookupResult {
    Status status;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kValue;
    std::string value;

    [[nodiscard]] bool found() const noexcept { return status.ok(); }
    [[nodiscard]] bool deleted() const noexcept {
        return found() && type == ValueType::kDeletion;
    }
};

struct MemTableRecord {
    std::string key;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kValue;
    std::string value;
};

// V0 uses std::map deliberately: it keeps keys ordered and leaves the project
// focused on storage semantics instead of reimplementing a container.
// Thread safety is introduced with the database coordination layer in a later
// phase; a MemTable instance must currently be externally synchronized.
class MemTable {
public:
    explicit MemTable(Options options = {});

    Status Put(std::uint64_t sequence, std::string_view key, std::string_view value);
    Status Delete(std::uint64_t sequence, std::string_view key);

    // Lookup exposes tombstones so a future database read path can prevent an
    // older value in another table from being resurrected.
    [[nodiscard]] LookupResult Lookup(std::string_view key) const;

    // Get provides user-visible semantics: a tombstone is reported as missing.
    [[nodiscard]] LookupResult Get(std::string_view key) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t ApproximateDataSize() const noexcept {
        return approximate_data_size_;
    }
    [[nodiscard]] std::vector<MemTableRecord> Records() const;

private:
    struct Entry {
        std::uint64_t sequence = 0;
        ValueType type = ValueType::kValue;
        std::string value;
    };

    Status Add(
        std::uint64_t sequence,
        ValueType type,
        std::string_view key,
        std::string_view value
    );
    [[nodiscard]] Status Validate(std::string_view key, std::string_view value) const;
    [[nodiscard]] static std::size_t EncodedPayloadSize(
        std::string_view key,
        std::string_view value
    ) noexcept;

    Options options_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::size_t approximate_data_size_ = 0;
};

}  // namespace minikv
