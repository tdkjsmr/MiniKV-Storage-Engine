#include "minikv/bloom_filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "minikv/coding.hpp"

namespace minikv {
namespace {

constexpr std::array<char, 4> kBloomMagic = {'M', 'K', 'B', 'F'};
constexpr long double kNaturalLogOfTwo = 0.693147180559945309417L;

Status BloomCorruption(std::string message) {
    return Status::Corruption("bloom filter: " + std::move(message));
}

std::uint64_t FinalizeHash64(std::uint64_t hash) noexcept {
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33U;
    return hash;
}

std::array<std::uint64_t, 2> BloomHashes(std::string_view key) noexcept {
    std::uint64_t first =
        14695981039346656037ULL ^ 0x9e3779b97f4a7c15ULL;
    std::uint64_t second =
        14695981039346656037ULL ^ 0xd6e8feb86659fd93ULL;
    for (const unsigned char byte : key) {
        const std::uint64_t value = static_cast<std::uint64_t>(byte);
        first = (first ^ value) * 1099511628211ULL;
        second = (second ^ value) * 1099511628211ULL;
    }
    return {FinalizeHash64(first), FinalizeHash64(second)};
}

std::uint32_t ChecksumParts(
    std::string_view prefix,
    std::string_view payload
) {
    std::string bytes;
    bytes.reserve(prefix.size() + payload.size());
    bytes.append(prefix);
    bytes.append(payload);
    return Crc32c(bytes);
}

bool FilterShapeIsValid(const BloomFilter& filter) {
    if (filter.bit_count() == 0) {
        return filter.hash_count() == 0 && filter.key_count() == 0 &&
               filter.memory_size() == 0;
    }
    return filter.bit_count() % 8U == 0 &&
           filter.bit_count() / 8U == filter.memory_size() &&
           filter.hash_count() > 0 &&
           filter.hash_count() <= kMaximumBloomHashCount &&
           filter.memory_size() <= kMaximumBloomFilterSize;
}

}  // namespace

Status BloomFilter::Create(
    std::size_t expected_key_count,
    double false_positive_rate,
    BloomFilter* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("Bloom filter output must not be null");
    }
    *output = {};
    if (!std::isfinite(false_positive_rate) || false_positive_rate <= 0.0 ||
        false_positive_rate >= 1.0) {
        return Status::InvalidArgument(
            "Bloom false-positive rate must be finite and between zero and one"
        );
    }
    if (expected_key_count == 0) {
        return Status::Ok();
    }

    const long double key_count = static_cast<long double>(expected_key_count);
    const long double requested_bits = std::ceil(
        -key_count * std::log(static_cast<long double>(false_positive_rate)) /
        (kNaturalLogOfTwo * kNaturalLogOfTwo)
    );
    const long double maximum_bits =
        static_cast<long double>(kMaximumBloomFilterSize) * 8.0L;
    if (!std::isfinite(requested_bits) || requested_bits > maximum_bits) {
        return Status::InvalidArgument(
            "Bloom filter exceeds the 64 MiB format limit"
        );
    }

    std::uint64_t bit_count = static_cast<std::uint64_t>(requested_bits);
    bit_count = std::max<std::uint64_t>(bit_count, 64U);
    bit_count = (bit_count + 7U) & ~std::uint64_t{7U};
    const long double ideal_hashes =
        static_cast<long double>(bit_count) / key_count * kNaturalLogOfTwo;
    const auto rounded_hashes = static_cast<std::uint64_t>(
        std::max<long double>(1.0L, std::round(ideal_hashes))
    );

    output->bit_count_ = bit_count;
    output->hash_count_ = static_cast<std::uint8_t>(std::min<std::uint64_t>(
        rounded_hashes,
        kMaximumBloomHashCount
    ));
    output->bits_.assign(static_cast<std::size_t>(bit_count / 8U), 0);
    return Status::Ok();
}

Status BloomFilter::Add(std::string_view key) {
    if (!FilterShapeIsValid(*this) || bit_count_ == 0) {
        return Status::InvalidArgument("Bloom filter has no allocated bit array");
    }
    if (key_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("Bloom filter key count is exhausted");
    }
    const auto hashes = BloomHashes(key);
    const std::uint64_t first = hashes[0];
    std::uint64_t second = hashes[1];
    second |= 1U;
    for (std::uint8_t index = 0; index < hash_count_; ++index) {
        const std::uint64_t bit =
            (first + static_cast<std::uint64_t>(index) * second) % bit_count_;
        bits_[static_cast<std::size_t>(bit / 8U)] |=
            static_cast<std::uint8_t>(1U << (bit % 8U));
    }
    ++key_count_;
    return Status::Ok();
}

bool BloomFilter::MayContain(std::string_view key) const noexcept {
    if (!FilterShapeIsValid(*this) || bit_count_ == 0 || key_count_ == 0) {
        return false;
    }
    const auto hashes = BloomHashes(key);
    const std::uint64_t first = hashes[0];
    std::uint64_t second = hashes[1];
    second |= 1U;
    for (std::uint8_t index = 0; index < hash_count_; ++index) {
        const std::uint64_t bit =
            (first + static_cast<std::uint64_t>(index) * second) % bit_count_;
        const auto mask = static_cast<std::uint8_t>(1U << (bit % 8U));
        if ((bits_[static_cast<std::size_t>(bit / 8U)] & mask) == 0) {
            return false;
        }
    }
    return true;
}

Status EncodeBloomFilter(
    const BloomFilter& filter,
    std::string* destination
) {
    if (destination == nullptr) {
        return Status::InvalidArgument("Bloom encoding output must not be null");
    }
    destination->clear();
    if (!FilterShapeIsValid(filter)) {
        return Status::InvalidArgument("Bloom filter shape is invalid");
    }
    const std::uint64_t total_size =
        static_cast<std::uint64_t>(kBloomFilterHeaderSize) + filter.bits_.size();
    if (total_size > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument("Bloom encoding exceeds format limits");
    }

    std::string header;
    header.append(kBloomMagic.data(), kBloomMagic.size());
    header.push_back(static_cast<char>(kBloomFilterFormatVersion));
    header.push_back(static_cast<char>(filter.hash_count_));
    PutFixed16(header, static_cast<std::uint16_t>(kBloomFilterHeaderSize));
    PutFixed32(header, static_cast<std::uint32_t>(total_size));
    PutFixed64(header, filter.bit_count_);
    PutFixed64(header, filter.key_count_);
    const std::string_view payload = filter.bits_.empty()
        ? std::string_view{}
        : std::string_view(
              reinterpret_cast<const char*>(filter.bits_.data()),
              filter.bits_.size()
          );
    PutFixed32(header, ChecksumParts(header, payload));

    destination->reserve(static_cast<std::size_t>(total_size));
    destination->append(header);
    destination->append(payload);
    return Status::Ok();
}

Status DecodeBloomFilter(
    std::string_view encoded,
    BloomFilter* output
) {
    if (output == nullptr) {
        return Status::InvalidArgument("Bloom decoding output must not be null");
    }
    *output = {};
    if (encoded.size() < kBloomFilterHeaderSize ||
        encoded.size() > kBloomFilterHeaderSize + kMaximumBloomFilterSize) {
        return BloomCorruption("encoded size is outside format limits");
    }
    if (!std::equal(kBloomMagic.begin(), kBloomMagic.end(), encoded.begin()) ||
        static_cast<std::uint8_t>(encoded[4]) != kBloomFilterFormatVersion) {
        return BloomCorruption("magic or format version is unsupported");
    }

    const auto hash_count = static_cast<std::uint8_t>(encoded[5]);
    std::uint16_t header_size = 0;
    std::uint32_t total_size = 0;
    std::uint64_t bit_count = 0;
    std::uint64_t key_count = 0;
    std::uint32_t checksum = 0;
    if (!DecodeFixed16(encoded.substr(6), &header_size) ||
        !DecodeFixed32(encoded.substr(8), &total_size) ||
        !DecodeFixed64(encoded.substr(12), &bit_count) ||
        !DecodeFixed64(encoded.substr(20), &key_count) ||
        !DecodeFixed32(encoded.substr(28), &checksum) ||
        header_size != kBloomFilterHeaderSize || total_size != encoded.size() ||
        ChecksumParts(encoded.substr(0, 28), encoded.substr(kBloomFilterHeaderSize)) !=
            checksum) {
        return BloomCorruption("header fields or checksum are invalid");
    }

    const std::size_t payload_size = encoded.size() - kBloomFilterHeaderSize;
    if (bit_count == 0) {
        if (hash_count != 0 || key_count != 0 || payload_size != 0) {
            return BloomCorruption("empty filter fields are inconsistent");
        }
        return Status::Ok();
    }
    if (bit_count % 8U != 0 || bit_count / 8U != payload_size ||
        hash_count == 0 || hash_count > kMaximumBloomHashCount) {
        return BloomCorruption("bit array or hash count is invalid");
    }

    BloomFilter decoded;
    decoded.bit_count_ = bit_count;
    decoded.key_count_ = key_count;
    decoded.hash_count_ = hash_count;
    decoded.bits_.assign(
        encoded.begin() + static_cast<std::ptrdiff_t>(kBloomFilterHeaderSize),
        encoded.end()
    );
    *output = std::move(decoded);
    return Status::Ok();
}

}  // namespace minikv
