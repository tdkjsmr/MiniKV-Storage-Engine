#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace minikv {

void PutFixed16(std::string& destination, std::uint16_t value);
void PutFixed32(std::string& destination, std::uint32_t value);
void PutFixed64(std::string& destination, std::uint64_t value);

[[nodiscard]] bool DecodeFixed16(std::string_view input, std::uint16_t* value);
[[nodiscard]] bool DecodeFixed32(std::string_view input, std::uint32_t* value);
[[nodiscard]] bool DecodeFixed64(std::string_view input, std::uint64_t* value);

// CRC32C (Castagnoli) uses the reflected polynomial 0x82F63B78.
[[nodiscard]] std::uint32_t Crc32c(std::string_view data) noexcept;

}  // namespace minikv
