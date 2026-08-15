#include "minikv/coding.hpp"

#include <cstddef>

namespace minikv {
namespace {

std::uint32_t ExtendCrc32c(std::uint32_t state, std::string_view data) noexcept {
    constexpr std::uint32_t polynomial = 0x82F63B78U;

    for (const char character : data) {
        state ^= static_cast<std::uint8_t>(character);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (state & 1U);
            state = (state >> 1U) ^ (polynomial & mask);
        }
    }
    return state;
}

}  // namespace

void PutFixed16(std::string& destination, std::uint16_t value) {
    destination.push_back(static_cast<char>(value & 0xFFU));
    destination.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void PutFixed32(std::string& destination, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        destination.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void PutFixed64(std::string& destination, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        destination.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

bool DecodeFixed16(std::string_view input, std::uint16_t* value) {
    if (value == nullptr || input.size() < sizeof(std::uint16_t)) {
        return false;
    }

    *value = static_cast<std::uint16_t>(static_cast<std::uint8_t>(input[0])) |
             static_cast<std::uint16_t>(
                 static_cast<std::uint16_t>(static_cast<std::uint8_t>(input[1])) << 8U
             );
    return true;
}

bool DecodeFixed32(std::string_view input, std::uint32_t* value) {
    if (value == nullptr || input.size() < sizeof(std::uint32_t)) {
        return false;
    }

    std::uint32_t decoded = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const auto index = static_cast<std::size_t>(shift / 8);
        decoded |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index]))
                   << shift;
    }
    *value = decoded;
    return true;
}

bool DecodeFixed64(std::string_view input, std::uint64_t* value) {
    if (value == nullptr || input.size() < sizeof(std::uint64_t)) {
        return false;
    }

    std::uint64_t decoded = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        const auto index = static_cast<std::size_t>(shift / 8);
        decoded |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(input[index]))
                   << shift;
    }
    *value = decoded;
    return true;
}

std::uint32_t Crc32c(std::string_view data) noexcept {
    constexpr std::uint32_t initial_state = 0xFFFFFFFFU;
    return ~ExtendCrc32c(initial_state, data);
}

}  // namespace minikv
