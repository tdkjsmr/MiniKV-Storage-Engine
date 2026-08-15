#include "minikv/wal.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "fake_writable_file.hpp"
#include "minikv/coding.hpp"
#include "test_harness.hpp"

namespace {

minikv::WalRecord SampleRecord() {
    minikv::WalRecord record;
    record.type = minikv::ValueType::kValue;
    record.sequence = 0x0102030405060708ULL;
    record.key = std::string("k\0y", 3);
    record.value = std::string("v\0l", 3);
    return record;
}

void TestEncodeDecodeRoundTrip() {
    const minikv::WalRecord record = SampleRecord();
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &encoded).ok(),
        "valid WAL record must encode"
    );
    minikv::test::Expect(
        encoded.size() == minikv::kWalHeaderSize + 6U,
        "encoded record size must match header plus payload"
    );
    minikv::test::Expect(
        encoded.substr(0, 4) == "MKVW",
        "WAL magic must be stable"
    );

    const auto decoded = minikv::DecodeWalRecord(encoded, {});
    minikv::test::Expect(decoded.status.ok(), "encoded WAL record must decode");
    minikv::test::Expect(
        decoded.bytes_consumed == encoded.size(),
        "decoder must report one complete record"
    );
    minikv::test::Expect(decoded.record.type == record.type, "record type must round-trip");
    minikv::test::Expect(
        decoded.record.sequence == record.sequence,
        "sequence number must round-trip"
    );
    minikv::test::Expect(decoded.record.key == record.key, "binary key must round-trip");
    minikv::test::Expect(decoded.record.value == record.value, "binary value must round-trip");
}

void TestGoldenBytes() {
    minikv::WalRecord record;
    record.type = minikv::ValueType::kValue;
    record.sequence = 1;
    record.key = "k";
    record.value = "v";

    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &encoded).ok(),
        "golden record must encode"
    );

    // Complete version-1 Golden Bytes. The CRC32C is 0xDDC9D0B8 and is stored
    // in little-endian order as B8 D0 C9 DD.
    const std::string expected(
        "MKVW"
        "\x01\x01"
        "\x20\x00"
        "\x22\x00\x00\x00"
        "\x01\x00\x00\x00\x00\x00\x00\x00"
        "\x01\x00\x00\x00"
        "\x01\x00\x00\x00"
        "\xB8\xD0\xC9\xDD"
        "kv",
        34
    );

    minikv::test::Expect(encoded == expected, "complete WAL Golden Bytes must be stable");
}

void TestValidation() {
    minikv::WalRecord record = SampleRecord();
    std::string output = "must be cleared";

    record.sequence = 0;
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &output).code() ==
            minikv::StatusCode::kInvalidArgument,
        "sequence zero must be rejected"
    );
    minikv::test::Expect(output.empty(), "failed encoding must clear its destination");

    record = SampleRecord();
    record.key.clear();
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &output).code() ==
            minikv::StatusCode::kInvalidArgument,
        "empty WAL key must be rejected"
    );

    record = SampleRecord();
    record.type = minikv::ValueType::kDeletion;
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &output).code() ==
            minikv::StatusCode::kInvalidArgument,
        "deletion with a value must be rejected"
    );

    minikv::Options small_options;
    small_options.max_key_size = 2;
    record = SampleRecord();
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, small_options, &output).code() ==
            minikv::StatusCode::kInvalidArgument,
        "configured key limit must be enforced"
    );
}

void TestIncompleteAndCorruption() {
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeWalRecord(SampleRecord(), {}, &encoded).ok(),
        "corruption fixture must encode"
    );

    minikv::test::Expect(
        minikv::DecodeWalRecord(encoded.substr(0, 10), {}).status.IsIncomplete(),
        "short header must be incomplete, not corruption"
    );
    minikv::test::Expect(
        minikv::DecodeWalRecord(encoded.substr(0, encoded.size() - 1U), {})
            .status.IsIncomplete(),
        "short payload must be incomplete, not corruption"
    );

    std::string damaged = encoded;
    damaged[0] = 'X';
    minikv::test::Expect(
        damaged.size() == encoded.size() &&
            minikv::DecodeWalRecord(damaged, {}).status.code() ==
                minikv::StatusCode::kCorruption,
        "bad magic must be corruption"
    );

    damaged = encoded;
    damaged.back() ^= static_cast<char>(0x01);
    minikv::test::Expect(
        minikv::DecodeWalRecord(damaged, {}).status.code() ==
            minikv::StatusCode::kCorruption,
        "payload checksum mismatch must be corruption"
    );

    damaged = encoded;
    damaged[28] ^= static_cast<char>(0x01);
    minikv::test::Expect(
        minikv::DecodeWalRecord(damaged, {}).status.code() ==
            minikv::StatusCode::kCorruption,
        "stored checksum corruption must be detected"
    );

    damaged = encoded;
    damaged[20] = '\0';
    damaged[21] = '\0';
    damaged[22] = '\0';
    damaged[23] = '\0';
    minikv::test::Expect(
        minikv::DecodeWalRecord(damaged, {}).status.code() ==
            minikv::StatusCode::kCorruption,
        "zero key length in a WAL header must be corruption"
    );
}

void TestShortWritesInterruptsAndSyncModes() {
    using Action = minikv::test::ScriptedWritableFile::Action;
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>(
        std::vector<Action>{Action::Interrupt(), Action::Write(2), Action::Write(3)}
    );
    auto* observed = file.get();
    minikv::WalWriter writer({}, std::move(file));

    minikv::WalRecord record = SampleRecord();
    minikv::test::Expect(writer.Append(record).ok(), "short writes must be completed");
    minikv::test::Expect(observed->write_calls() == 4, "writer must retry each partial result");
    minikv::test::Expect(observed->sync_calls() == 1, "strict mode must sync once");
    minikv::test::Expect(
        minikv::DecodeWalRecord(observed->contents(), {}).status.ok(),
        "completed short writes must form a valid record"
    );

    auto async_file = std::make_unique<minikv::test::ScriptedWritableFile>();
    auto* async_observed = async_file.get();
    minikv::WalWriter async_writer({}, std::move(async_file));
    minikv::WriteOptions options;
    options.sync_mode = minikv::SyncMode::kAsync;
    record.sequence = 2;
    minikv::test::Expect(async_writer.Append(record, options).ok(), "async append must succeed");
    minikv::test::Expect(
        async_observed->sync_calls() == 0,
        "async mode must not call fdatasync"
    );

    minikv::WriteOptions invalid_options;
    invalid_options.sync_mode = static_cast<minikv::SyncMode>(99);
    record.sequence = 3;
    minikv::test::Expect(
        async_writer.Append(record, invalid_options).code() ==
            minikv::StatusCode::kInvalidArgument,
        "unknown sync mode must not silently become asynchronous"
    );
}

void TestWriteFailuresPoisonWriter() {
    using Action = minikv::test::ScriptedWritableFile::Action;
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>(
        std::vector<Action>{Action::Write(5), Action::Error("injected write failure")}
    );
    auto* observed = file.get();
    minikv::WalWriter writer({}, std::move(file));

    minikv::WalRecord record = SampleRecord();
    minikv::test::Expect(
        writer.Append(record).code() == minikv::StatusCode::kIOError,
        "injected write failure must propagate"
    );
    const std::size_t bytes_after_failure = observed->contents().size();
    const std::size_t calls_after_failure = observed->write_calls();
    record.sequence = 2;
    minikv::test::Expect(
        writer.Append(record).code() == minikv::StatusCode::kIOError,
        "poisoned writer must reject later appends"
    );
    minikv::test::Expect(
        observed->contents().size() == bytes_after_failure &&
            observed->write_calls() == calls_after_failure,
        "poisoned writer must not write additional bytes"
    );

    auto zero_file = std::make_unique<minikv::test::ScriptedWritableFile>(
        std::vector<Action>{Action::ZeroProgress()}
    );
    minikv::WalWriter zero_writer({}, std::move(zero_file));
    minikv::test::Expect(
        zero_writer.Append(SampleRecord()).code() == minikv::StatusCode::kIOError,
        "zero-progress writes must fail instead of looping forever"
    );

    auto over_file = std::make_unique<minikv::test::ScriptedWritableFile>(
        std::vector<Action>{Action::OverReport()}
    );
    minikv::WalWriter over_writer({}, std::move(over_file));
    minikv::test::Expect(
        over_writer.Append(SampleRecord()).code() == minikv::StatusCode::kIOError,
        "invalid byte counts from a file implementation must fail"
    );
}

void TestPosixFileRoundTrip() {
    char path[] = "/tmp/minikv-v1-wal-XXXXXX";
    const int descriptor = ::mkstemp(path);
    minikv::test::Expect(descriptor >= 0, "temporary WAL file must be created");
    if (descriptor < 0) {
        return;
    }
    ::close(descriptor);

    std::unique_ptr<minikv::WalWriter> writer;
    const auto open_status = minikv::WalWriter::Open(path, {}, &writer);
    minikv::test::Expect(open_status.ok(), "POSIX WAL writer must open");
    if (open_status.ok()) {
        minikv::test::Expect(
            writer->Append(SampleRecord()).ok(),
            "POSIX WAL append and fdatasync must succeed"
        );
    }
    writer.reset();

    std::ifstream input(path, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    minikv::test::Expect(
        minikv::DecodeWalRecord(bytes, {}).status.ok(),
        "bytes written through POSIX file must decode"
    );
    minikv::test::Expect(std::remove(path) == 0, "temporary WAL file must be removed");
}

}  // namespace

int main() {
    TestEncodeDecodeRoundTrip();
    TestGoldenBytes();
    TestValidation();
    TestIncompleteAndCorruption();
    TestShortWritesInterruptsAndSyncModes();
    TestWriteFailuresPoisonWriter();
    TestPosixFileRoundTrip();
    return minikv::test::Finish("WAL");
}
