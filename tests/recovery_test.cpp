#include "minikv/recovery.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "fake_recovery_file.hpp"
#include "minikv/write_path.hpp"
#include "test_harness.hpp"

namespace {

std::string Encode(
    std::uint64_t sequence,
    minikv::ValueType type,
    std::string_view key,
    std::string_view value = {}
) {
    minikv::WalRecord record;
    record.sequence = sequence;
    record.type = type;
    record.key.assign(key);
    record.value.assign(value);
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeWalRecord(record, {}, &encoded).ok(),
        "recovery fixture must encode"
    );
    return encoded;
}

class TemporaryFile {
public:
    explicit TemporaryFile(const char* pattern) {
        path_ = pattern;
        const int descriptor = ::mkstemp(path_.data());
        minikv::test::Expect(descriptor >= 0, "temporary WAL must be created");
        if (descriptor >= 0) {
            const int close_result = ::close(descriptor);
            minikv::test::Expect(close_result == 0, "temporary WAL descriptor must close");
            valid_ = true;
        }
    }

    ~TemporaryFile() {
        if (valid_) {
            const int remove_result = std::remove(path_.c_str());
            (void)remove_result;
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    std::string path_;
    bool valid_ = false;
};

void WriteBytes(const std::string& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    minikv::test::Expect(output.is_open(), "fixture WAL must open for writing");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    minikv::test::Expect(output.good(), "fixture WAL bytes must be written");
}

std::string ReadBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

void TestReplayAndShortReads() {
    const std::string first = Encode(1, minikv::ValueType::kValue, "alpha", "old");
    const std::string second = Encode(2, minikv::ValueType::kValue, "alpha", "new");
    const std::string third = Encode(3, minikv::ValueType::kDeletion, "alpha");
    const std::string fourth = Encode(4, minikv::ValueType::kValue, "beta", "value");
    minikv::test::ScriptedRecoveryFile file(first + second + third + fourth);
    file.set_maximum_chunk_size(3);
    file.set_interrupts(2);

    minikv::MemTable memtable;
    minikv::WalRecoveryResult result;
    const auto status = minikv::RecoverWal(file, {}, &memtable, &result);
    minikv::test::Expect(status.ok(), "valid WAL must recover through short reads");
    minikv::test::Expect(
        result.records_recovered == 4 && result.max_sequence == 4,
        "recovery must count records and retain the maximum sequence"
    );
    minikv::test::Expect(
        result.valid_bytes == first.size() + second.size() + third.size() + fourth.size(),
        "all valid bytes must be reported"
    );
    minikv::test::Expect(!result.tail_truncated, "complete WAL must not be truncated");
    minikv::test::Expect(
        memtable.Get("alpha").status.IsNotFound(),
        "replayed tombstone must hide an older value"
    );
    minikv::test::Expect(
        memtable.Get("beta").value == "value",
        "replayed value must be visible"
    );
    minikv::test::Expect(file.read_calls() > 8, "short reads and EINTR must be retried");
}

void TestEveryTruncatedTailAndIdempotence() {
    const std::string first = Encode(1, minikv::ValueType::kValue, "first", "one");
    const std::string second = Encode(2, minikv::ValueType::kValue, "second", "two");

    for (std::size_t cut = 1; cut < first.size(); ++cut) {
        minikv::test::ScriptedRecoveryFile file(first.substr(0, cut));
        minikv::MemTable memtable;
        minikv::WalRecoveryResult result;
        minikv::test::Expect(
            minikv::RecoverWal(file, {}, &memtable, &result).ok(),
            "every incomplete first-record tail must be recoverable"
        );
        minikv::test::Expect(
            file.contents().empty() && result.records_recovered == 0,
            "an incomplete first record must truncate to an empty WAL"
        );
    }

    for (std::size_t cut = 1; cut < second.size(); ++cut) {
        minikv::test::ScriptedRecoveryFile file(first + second.substr(0, cut));
        minikv::MemTable memtable;
        minikv::WalRecoveryResult result;
        minikv::test::Expect(
            minikv::RecoverWal(file, {}, &memtable, &result).ok(),
            "every incomplete final-record tail must be recoverable"
        );
        minikv::test::Expect(
            file.contents() == first && result.records_recovered == 1,
            "recovery must preserve the complete prefix and discard only the tail"
        );
        minikv::test::Expect(
            result.tail_truncated && result.discarded_tail_bytes == cut,
            "tail repair must report exactly the discarded bytes"
        );

        minikv::MemTable second_memtable;
        minikv::WalRecoveryResult second_result;
        minikv::test::Expect(
            minikv::RecoverWal(file, {}, &second_memtable, &second_result).ok(),
            "recovery must be idempotent after repairing a tail"
        );
        minikv::test::Expect(
            !second_result.tail_truncated && second_result.records_recovered == 1,
            "the second recovery must observe a clean record boundary"
        );
    }
}

void TestCorruptionIsNeverSilentlyTruncated() {
    const std::string first = Encode(1, minikv::ValueType::kValue, "safe", "value");
    const std::string second = Encode(2, minikv::ValueType::kValue, "target", "payload");

    for (std::size_t index = 0; index < second.size(); ++index) {
        std::string damaged = first + second;
        damaged[first.size() + index] ^= static_cast<char>(0x01);
        minikv::test::ScriptedRecoveryFile file(damaged);
        minikv::MemTable memtable;
        minikv::test::Expect(
            memtable.Put(99, "sentinel", "unchanged").ok(),
            "transactional recovery fixture must initialize"
        );
        minikv::WalRecoveryResult result;
        result.records_recovered = 77;

        const auto status = minikv::RecoverWal(file, {}, &memtable, &result);
        minikv::test::Expect(
            status.code() == minikv::StatusCode::kCorruption,
            "any flipped byte in a complete record must be corruption"
        );
        minikv::test::Expect(
            file.truncate_calls() == 0 && file.contents() == damaged,
            "complete corruption must not be repaired by truncation"
        );
        minikv::test::Expect(
            memtable.Get("sentinel").value == "unchanged" &&
                result.records_recovered == 77,
            "failed recovery must leave caller outputs unchanged"
        );
    }
}

void TestSequenceAndIoFailures() {
    const std::string out_of_order =
        Encode(2, minikv::ValueType::kValue, "a", "one") +
        Encode(1, minikv::ValueType::kValue, "b", "two");
    minikv::test::ScriptedRecoveryFile sequence_file(out_of_order);
    minikv::MemTable memtable;
    minikv::WalRecoveryResult result;
    minikv::test::Expect(
        minikv::RecoverWal(sequence_file, {}, &memtable, &result).code() ==
            minikv::StatusCode::kCorruption,
        "non-increasing WAL sequence numbers must be corruption"
    );

    const std::string incomplete =
        Encode(1, minikv::ValueType::kValue, "a", "one").substr(0, 7);
    minikv::test::ScriptedRecoveryFile truncate_file(incomplete);
    truncate_file.set_truncate_status(minikv::Status::IOError("injected truncate"));
    minikv::test::Expect(
        minikv::RecoverWal(truncate_file, {}, &memtable, &result).code() ==
            minikv::StatusCode::kIOError,
        "tail truncate failure must propagate"
    );

    minikv::test::ScriptedRecoveryFile sync_file(incomplete);
    sync_file.set_sync_status(minikv::Status::IOError("injected sync"));
    minikv::test::Expect(
        minikv::RecoverWal(sync_file, {}, &memtable, &result).code() ==
            minikv::StatusCode::kIOError,
        "tail repair sync failure must propagate"
    );

    minikv::test::ScriptedRecoveryFile zero_file(
        Encode(1, minikv::ValueType::kValue, "a", "one")
    );
    zero_file.set_zero_progress(true);
    minikv::test::Expect(
        minikv::RecoverWal(zero_file, {}, &memtable, &result).code() ==
            minikv::StatusCode::kIOError,
        "zero-progress reads must fail instead of looping"
    );

    minikv::test::ScriptedRecoveryFile over_file(
        Encode(1, minikv::ValueType::kValue, "a", "one")
    );
    over_file.set_over_report(true);
    minikv::test::Expect(
        minikv::RecoverWal(over_file, {}, &memtable, &result).code() ==
            minikv::StatusCode::kIOError,
        "invalid read byte counts must fail"
    );
}

void TestWritePathReopenAndPhysicalTailRepair() {
    TemporaryFile file("/tmp/minikv-v2-reopen-XXXXXX");
    if (!file.valid()) {
        return;
    }

    std::unique_ptr<minikv::WritePath> database;
    minikv::WalRecoveryResult result;
    minikv::test::Expect(
        minikv::WritePath::Open(file.path(), {}, &database, &result).ok(),
        "empty WAL must open"
    );
    minikv::test::Expect(database->Put("key", "old").ok(), "first strict Put must sync");
    minikv::test::Expect(database->Put("key", "new").ok(), "second strict Put must sync");
    minikv::test::Expect(database->Delete("missing").ok(), "strict Delete must sync");
    database.reset();

    minikv::test::Expect(
        minikv::WritePath::Open(file.path(), {}, &database, &result).ok(),
        "complete WAL must reopen"
    );
    minikv::test::Expect(
        result.records_recovered == 3 && database->last_sequence() == 3,
        "reopen must restore all records and the sequence allocator"
    );
    minikv::test::Expect(
        database->Get("key").value == "new",
        "reopen must reconstruct the latest in-memory value"
    );
    minikv::test::Expect(database->Put("after", "restart").ok(), "write after recovery must work");
    minikv::test::Expect(
        database->last_sequence() == 4,
        "first post-recovery write must continue at max sequence plus one"
    );
    database.reset();

    const std::string clean = ReadBytes(file.path());
    const std::string partial =
        Encode(5, minikv::ValueType::kValue, "torn", "record").substr(0, 19);
    WriteBytes(file.path(), clean + partial);
    minikv::test::Expect(
        minikv::WritePath::Open(file.path(), {}, &database, &result).ok(),
        "a physically truncated final record must be repaired during Open"
    );
    minikv::test::Expect(
        result.tail_truncated && result.discarded_tail_bytes == partial.size(),
        "Open must report its physical tail repair"
    );
    database.reset();
    minikv::test::Expect(
        ReadBytes(file.path()) == clean,
        "repaired POSIX WAL must end at the previous record boundary"
    );
}

void TestSigkillAfterAcknowledgedStrictWrite() {
    TemporaryFile file("/tmp/minikv-v2-crash-XXXXXX");
    if (!file.valid()) {
        return;
    }

    int ready_pipe[2] = {-1, -1};
    minikv::test::Expect(::pipe(ready_pipe) == 0, "crash-test pipe must be created");
    if (ready_pipe[0] < 0) {
        return;
    }

    const pid_t child = ::fork();
    minikv::test::Expect(child >= 0, "crash-test process must fork");
    if (child == 0) {
        ::close(ready_pipe[0]);
        std::unique_ptr<minikv::WritePath> database;
        minikv::WalRecoveryResult result;
        if (!minikv::WritePath::Open(file.path(), {}, &database, &result).ok() ||
            !database->Put("durable", "yes").ok() ||
            !database->Put("deleted", "old").ok() ||
            !database->Delete("deleted").ok()) {
            _exit(2);
        }
        const char marker = 'R';
        if (::write(ready_pipe[1], &marker, 1) != 1) {
            _exit(3);
        }
        for (;;) {
            ::pause();
        }
    }
    if (child < 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        return;
    }

    ::close(ready_pipe[1]);
    char marker = 0;
    ssize_t read_result = -1;
    do {
        read_result = ::read(ready_pipe[0], &marker, 1);
    } while (read_result < 0 && errno == EINTR);
    ::close(ready_pipe[0]);
    minikv::test::Expect(
        read_result == 1 && marker == 'R',
        "child must acknowledge only after strict writes complete"
    );
    if (read_result == 1) {
        minikv::test::Expect(::kill(child, SIGKILL) == 0, "child must be killed abruptly");
    }
    int wait_status = 0;
    minikv::test::Expect(
        ::waitpid(child, &wait_status, 0) == child,
        "crash-test child must be collected"
    );
    minikv::test::Expect(
        WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGKILL,
        "crash-test child must terminate through SIGKILL"
    );

    std::unique_ptr<minikv::WritePath> recovered;
    minikv::WalRecoveryResult result;
    minikv::test::Expect(
        minikv::WritePath::Open(file.path(), {}, &recovered, &result).ok(),
        "strict writes acknowledged before SIGKILL must recover"
    );
    minikv::test::Expect(
        recovered != nullptr && recovered->Get("durable").value == "yes",
        "acknowledged value must survive process death"
    );
    minikv::test::Expect(
        recovered != nullptr && recovered->Get("deleted").status.IsNotFound(),
        "acknowledged tombstone must survive process death"
    );
    minikv::test::Expect(
        recovered != nullptr && recovered->last_sequence() == 3,
        "crash recovery must restore the acknowledged sequence frontier"
    );
}

void TestOpenValidationAndMiddleCorruption() {
    std::unique_ptr<minikv::WritePath> database;
    minikv::WalRecoveryResult result;
    minikv::test::Expect(
        minikv::WritePath::Open("", {}, &database, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "Open must reject an empty path"
    );
    minikv::test::Expect(
        minikv::WritePath::Open("/tmp/unused", {}, nullptr, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "Open must reject a null database output"
    );
    minikv::test::Expect(
        minikv::WritePath::Open("/tmp/unused", {}, &database, nullptr).code() ==
            minikv::StatusCode::kInvalidArgument,
        "Open must reject a null recovery-result output"
    );

    minikv::test::ScriptedRecoveryFile valid_file(
        Encode(1, minikv::ValueType::kValue, "key", "value")
    );
    minikv::test::Expect(
        minikv::RecoverWal(valid_file, {}, nullptr, &result).code() ==
            minikv::StatusCode::kInvalidArgument,
        "RecoverWal must reject a null MemTable output"
    );

    TemporaryFile file("/tmp/minikv-v2-corrupt-XXXXXX");
    if (!file.valid()) {
        return;
    }
    std::string bytes =
        Encode(1, minikv::ValueType::kValue, "first", "one") +
        Encode(2, minikv::ValueType::kValue, "second", "two");
    bytes.back() ^= static_cast<char>(0x40);
    WriteBytes(file.path(), bytes);
    result.records_recovered = 42;
    minikv::test::Expect(
        minikv::WritePath::Open(file.path(), {}, &database, &result).code() ==
            minikv::StatusCode::kCorruption,
        "Open must reject complete corruption"
    );
    minikv::test::Expect(
        database == nullptr && result.records_recovered == 0 &&
            ReadBytes(file.path()) == bytes,
        "failed Open must not expose a database or modify corrupt bytes"
    );
}

}  // namespace

int main() {
    TestReplayAndShortReads();
    TestEveryTruncatedTailAndIdempotence();
    TestCorruptionIsNeverSilentlyTruncated();
    TestSequenceAndIoFailures();
    TestWritePathReopenAndPhysicalTailRepair();
    TestSigkillAfterAcknowledgedStrictWrite();
    TestOpenValidationAndMiddleCorruption();
    return minikv::test::Finish("recovery");
}
