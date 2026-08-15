#pragma once

#include <cstdint>
#include <string_view>

namespace minikv {

inline constexpr std::uint32_t kEngineVersionMajor = 1;
inline constexpr std::uint32_t kEngineVersionMinor = 0;
inline constexpr std::uint32_t kEngineVersionPatch = 0;
inline constexpr std::string_view kEngineVersion = "1.0.0";

[[nodiscard]] constexpr std::string_view EngineVersion() noexcept {
    return kEngineVersion;
}

}  // namespace minikv
