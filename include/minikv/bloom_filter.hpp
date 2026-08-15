#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "minikv/status.hpp"

namespace minikv {

inline constexpr std::uint8_t kBloomFilterFormatVersion = 1;
inline constexpr std::size_t kBloomFilterHeaderSize = 32;
inline constexpr std::size_t kMaximumBloomFilterSize = 64U * 1024U * 1024U;
inline constexpr std::uint8_t kMaximumBloomHashCount = 30;

class BloomFilter {
public:
    static Status Create(
        std::size_t expected_key_count,
        double false_positive_rate,
        BloomFilter* output
    );

    Status Add(std::string_view key);
    [[nodiscard]] bool MayContain(std::string_view key) const noexcept;

    [[nodiscard]] std::uint64_t bit_count() const noexcept { return bit_count_; }
    [[nodiscard]] std::uint64_t key_count() const noexcept { return key_count_; }
    [[nodiscard]] std::uint8_t hash_count() const noexcept { return hash_count_; }
    [[nodiscard]] std::size_t memory_size() const noexcept { return bits_.size(); }

private:
    friend Status EncodeBloomFilter(
        const BloomFilter& filter,
        std::string* destination
    );
    friend Status DecodeBloomFilter(
        std::string_view encoded,
        BloomFilter* output
    );

    std::uint64_t bit_count_ = 0;
    std::uint64_t key_count_ = 0;
    std::uint8_t hash_count_ = 0;
    std::vector<std::uint8_t> bits_;
};

[[nodiscard]] Status EncodeBloomFilter(
    const BloomFilter& filter,
    std::string* destination
);

[[nodiscard]] Status DecodeBloomFilter(
    std::string_view encoded,
    BloomFilter* output
);

}  // namespace minikv
