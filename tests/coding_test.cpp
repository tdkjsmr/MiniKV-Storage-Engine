#include "minikv/coding.hpp"

#include <cstdint>
#include <string>

#include "test_harness.hpp"

namespace {

void TestLittleEndianRoundTrip() {
    std::string encoded;
    minikv::PutFixed16(encoded, 0x1234U);
    minikv::PutFixed32(encoded, 0x89ABCDEFU);
    minikv::PutFixed64(encoded, 0x0123456789ABCDEFULL);

    const std::string expected(
        "\x34\x12"
        "\xEF\xCD\xAB\x89"
        "\xEF\xCD\xAB\x89\x67\x45\x23\x01",
        14
    );
    minikv::test::Expect(encoded == expected, "fixed integers must use little-endian bytes");

    std::uint16_t value16 = 0;
    std::uint32_t value32 = 0;
    std::uint64_t value64 = 0;
    minikv::test::Expect(
        minikv::DecodeFixed16(encoded, &value16) && value16 == 0x1234U,
        "fixed16 must decode"
    );
    minikv::test::Expect(
        minikv::DecodeFixed32(encoded.substr(2), &value32) && value32 == 0x89ABCDEFU,
        "fixed32 must decode"
    );
    minikv::test::Expect(
        minikv::DecodeFixed64(encoded.substr(6), &value64) &&
            value64 == 0x0123456789ABCDEFULL,
        "fixed64 must decode"
    );
}

void TestDecodeBoundaries() {
    std::uint64_t value = 0;
    minikv::test::Expect(
        !minikv::DecodeFixed64("short", &value),
        "fixed64 must reject short input"
    );
    minikv::test::Expect(
        !minikv::DecodeFixed64("12345678", nullptr),
        "fixed64 must reject a null destination"
    );
}

void TestCrc32cKnownVector() {
    minikv::test::Expect(
        minikv::Crc32c("123456789") == 0xE3069283U,
        "CRC32C must match the standard Castagnoli check value"
    );
    minikv::test::Expect(
        minikv::Crc32c("") == 0U,
        "CRC32C of empty input must be zero"
    );
}

}  // namespace

int main() {
    TestLittleEndianRoundTrip();
    TestDecodeBoundaries();
    TestCrc32cKnownVector();
    return minikv::test::Finish("coding");
}
