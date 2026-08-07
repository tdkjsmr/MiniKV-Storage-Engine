#include "minikv/memtable.hpp"

#include <limits>
#include <utility>

namespace minikv {
namespace {

constexpr std::size_t kRecordMetadataSize = sizeof(std::uint64_t) + sizeof(ValueType);

}  // namespace

MemTable::MemTable(Options options) : options_(options) {}

Status MemTable::Put(
    std::uint64_t sequence,
    std::string_view key,
    std::string_view value
) {
    return Add(sequence, ValueType::kValue, key, value);
}

Status MemTable::Delete(std::uint64_t sequence, std::string_view key) {
    return Add(sequence, ValueType::kDeletion, key, {});
}

LookupResult MemTable::Lookup(std::string_view key) const {
    const auto validation = Validate(key, {});
    if (!validation.ok()) {
        return {validation, 0, ValueType::kValue, {}};
    }

    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return {Status::NotFound("key does not exist"), 0, ValueType::kValue, {}};
    }

    return {
        Status::Ok(),
        entry->second.sequence,
        entry->second.type,
        entry->second.value,
    };
}

LookupResult MemTable::Get(std::string_view key) const {
    auto result = Lookup(key);
    if (result.deleted()) {
        result.status = Status::NotFound("key was deleted");
        result.value.clear();
    }
    return result;
}

Status MemTable::Add(
    std::uint64_t sequence,
    ValueType type,
    std::string_view key,
    std::string_view value
) {
    if (sequence == 0) {
        return Status::InvalidArgument("sequence number must be greater than zero");
    }

    const auto validation = Validate(key, value);
    if (!validation.ok()) {
        return validation;
    }

    const auto current = entries_.find(key);
    if (current != entries_.end() && sequence <= current->second.sequence) {
        return Status::InvalidArgument(
            "sequence number must be greater than the existing record"
        );
    }

    const std::size_t new_size = EncodedPayloadSize(key, value);
    if (new_size > std::numeric_limits<std::size_t>::max() - approximate_data_size_) {
        return Status::InvalidArgument("MemTable size accounting overflow");
    }

    std::string owned_key(key);
    Entry new_entry{sequence, type, std::string(value)};

    if (current == entries_.end()) {
        entries_.emplace(std::move(owned_key), std::move(new_entry));
        approximate_data_size_ += new_size;
        return Status::Ok();
    }

    const std::size_t old_size = EncodedPayloadSize(current->first, current->second.value);
    current->second = std::move(new_entry);
    approximate_data_size_ = approximate_data_size_ - old_size + new_size;
    return Status::Ok();
}

Status MemTable::Validate(std::string_view key, std::string_view value) const {
    if (key.empty()) {
        return Status::InvalidArgument("key must not be empty");
    }
    if (key.size() > options_.max_key_size) {
        return Status::InvalidArgument("key exceeds configured maximum size");
    }
    if (value.size() > options_.max_value_size) {
        return Status::InvalidArgument("value exceeds configured maximum size");
    }
    return Status::Ok();
}

std::size_t MemTable::EncodedPayloadSize(
    std::string_view key,
    std::string_view value
) noexcept {
    return kRecordMetadataSize + key.size() + value.size();
}

}  // namespace minikv
