#include "minikv/write_path.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fake_writable_file.hpp"
#include "test_harness.hpp"

namespace {

void TestStrictWriteVisibilityAndSequence() {
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>();
    auto* observed = file.get();
    std::unique_ptr<minikv::WritePath> write_path;
    minikv::test::Expect(
        minikv::WritePath::Create({}, std::move(file), &write_path).ok(),
        "WritePath must be created"
    );

    minikv::test::Expect(write_path->Put("key", "old").ok(), "first Put must succeed");
    minikv::test::Expect(write_path->last_sequence() == 1, "first Put must use sequence 1");
    minikv::test::Expect(write_path->Get("key").value == "old", "Put must become visible");

    minikv::test::Expect(write_path->Put("key", "new").ok(), "overwrite must succeed");
    minikv::test::Expect(write_path->last_sequence() == 2, "overwrite must advance sequence");
    minikv::test::Expect(write_path->Get("key").value == "new", "new value must win");

    minikv::test::Expect(write_path->Delete("key").ok(), "Delete must succeed");
    minikv::test::Expect(write_path->last_sequence() == 3, "Delete must advance sequence");
    minikv::test::Expect(
        write_path->Get("key").status.IsNotFound(),
        "Delete must become user-visible as NotFound"
    );
    minikv::test::Expect(observed->sync_calls() == 3, "strict writes must each sync once");

    std::string_view remaining = observed->contents();
    for (std::uint64_t expected_sequence = 1; expected_sequence <= 3; ++expected_sequence) {
        const auto decoded = minikv::DecodeWalRecord(remaining, {});
        minikv::test::Expect(decoded.status.ok(), "each coordinated write must decode");
        minikv::test::Expect(
            decoded.record.sequence == expected_sequence,
            "coordinated WAL sequences must be monotonic"
        );
        remaining.remove_prefix(decoded.bytes_consumed);
    }
    minikv::test::Expect(remaining.empty(), "WAL must contain exactly three records");
}

void TestSyncFailureIsNotVisible() {
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>();
    file->set_sync_status(minikv::Status::IOError("injected fdatasync failure"));
    auto* observed = file.get();
    std::unique_ptr<minikv::WritePath> write_path;
    minikv::test::Expect(
        minikv::WritePath::Create({}, std::move(file), &write_path).ok(),
        "sync-failure WritePath must be created"
    );

    minikv::test::Expect(
        write_path->Put("key", "value").code() == minikv::StatusCode::kIOError,
        "fdatasync failure must make Put fail"
    );
    minikv::test::Expect(
        write_path->Get("key").status.IsNotFound(),
        "failed strict Put must not update MemTable"
    );
    minikv::test::Expect(
        write_path->last_sequence() == 1,
        "sequence used by a failed I/O attempt must not be rolled back"
    );
    minikv::test::Expect(
        minikv::DecodeWalRecord(observed->contents(), {}).status.ok(),
        "sync failure may leave a complete but unconfirmed WAL record"
    );

    const std::size_t bytes_after_failure = observed->contents().size();
    minikv::test::Expect(
        write_path->Put("other", "value").code() == minikv::StatusCode::kIOError,
        "failed WritePath must reject future writes"
    );
    minikv::test::Expect(
        write_path->last_sequence() == 1 &&
            observed->contents().size() == bytes_after_failure,
        "failed WritePath must not reuse or allocate another sequence"
    );
}

void TestPartialWriteFailureIsNotVisible() {
    using Action = minikv::test::ScriptedWritableFile::Action;
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>(
        std::vector<Action>{Action::Write(7), Action::Error("injected disk failure")}
    );
    std::unique_ptr<minikv::WritePath> write_path;
    minikv::test::Expect(
        minikv::WritePath::Create({}, std::move(file), &write_path).ok(),
        "partial-failure WritePath must be created"
    );
    minikv::test::Expect(
        write_path->Put("key", "value").code() == minikv::StatusCode::kIOError,
        "partial WAL failure must propagate"
    );
    minikv::test::Expect(
        write_path->Get("key").status.IsNotFound(),
        "partial WAL record must never become visible"
    );
}

void TestAsyncModeAndInputValidation() {
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>();
    file->set_sync_status(minikv::Status::IOError("sync must not be called"));
    auto* observed = file.get();
    std::unique_ptr<minikv::WritePath> write_path;
    minikv::test::Expect(
        minikv::WritePath::Create({}, std::move(file), &write_path).ok(),
        "async WritePath must be created"
    );

    minikv::test::Expect(
        write_path->Put("", "value").code() == minikv::StatusCode::kInvalidArgument,
        "invalid key must fail before sequence allocation and I/O"
    );
    minikv::test::Expect(
        write_path->last_sequence() == 0 && observed->contents().empty(),
        "invalid input must not consume a sequence or write WAL bytes"
    );

    minikv::WriteOptions invalid_options;
    invalid_options.sync_mode = static_cast<minikv::SyncMode>(99);
    minikv::test::Expect(
        write_path->Put("key", "value", invalid_options).code() ==
            minikv::StatusCode::kInvalidArgument,
        "invalid sync mode must fail before sequence allocation"
    );
    minikv::test::Expect(
        write_path->last_sequence() == 0 && observed->contents().empty(),
        "invalid sync mode must not consume a sequence or write WAL bytes"
    );

    minikv::WriteOptions options;
    options.sync_mode = minikv::SyncMode::kAsync;
    minikv::test::Expect(
        write_path->Put("key", "value", options).ok(),
        "async Put must succeed without calling Sync"
    );
    minikv::test::Expect(observed->sync_calls() == 0, "async Put must skip Sync");
    minikv::test::Expect(
        write_path->Get("key").value == "value",
        "successful async Put must update MemTable"
    );
}

void TestFactoryValidation() {
    std::unique_ptr<minikv::WritePath> output;
    minikv::test::Expect(
        minikv::WritePath::Create({}, nullptr, &output).code() ==
            minikv::StatusCode::kInvalidArgument,
        "WritePath must reject a null WAL file"
    );
    auto file = std::make_unique<minikv::test::ScriptedWritableFile>();
    minikv::test::Expect(
        minikv::WritePath::Create({}, std::move(file), nullptr).code() ==
            minikv::StatusCode::kInvalidArgument,
        "WritePath must reject a null output pointer"
    );
}

}  // namespace

int main() {
    TestStrictWriteVisibilityAndSequence();
    TestSyncFailureIsNotVisible();
    TestPartialWriteFailureIsNotVisible();
    TestAsyncModeAndInputValidation();
    TestFactoryValidation();
    return minikv::test::Finish("write path");
}
