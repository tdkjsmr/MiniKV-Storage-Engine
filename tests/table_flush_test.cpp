#include "minikv/flush.hpp"
#include "minikv/table.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "fault_injecting_flush_environment.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* pattern) {
        path_ = pattern;
        char* result = ::mkdtemp(path_.data());
        minikv::test::Expect(result != nullptr, "temporary directory must be created");
        valid_ = result != nullptr;
    }

    ~TemporaryDirectory() {
        if (valid_) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

minikv::MemTable SampleMemTable() {
    minikv::MemTable table;
    minikv::test::Expect(table.Put(1, "alpha", "one").ok(), "sample Put must work");
    minikv::test::Expect(table.Delete(2, "beta").ok(), "sample Delete must work");
    minikv::test::Expect(table.Put(3, "gamma", "three").ok(), "sample Put must work");
    return table;
}

void TestTableRoundTripAndCorruption() {
    const auto memtable = SampleMemTable();
    std::string encoded;
    minikv::test::Expect(
        minikv::EncodeTable(7, memtable, {}, &encoded).ok(),
        "non-empty MemTable must encode as a V3 table"
    );
    minikv::test::Expect(
        encoded.substr(0, 4) == "MKST",
        "V3 table magic must be stable"
    );

    std::unique_ptr<minikv::TableData> decoded;
    minikv::test::Expect(
        minikv::DecodeTable(encoded, {}, &decoded).ok(),
        "encoded table must decode"
    );
    minikv::test::Expect(
        decoded->generation == 7 && decoded->max_sequence == 3,
        "table generation and maximum sequence must round-trip"
    );
    minikv::test::Expect(
        decoded->records.Get("alpha").value == "one",
        "table value must round-trip"
    );
    minikv::test::Expect(
        decoded->records.Lookup("beta").deleted(),
        "table tombstone must round-trip"
    );

    for (std::size_t index = 0; index < encoded.size(); ++index) {
        std::string damaged = encoded;
        damaged[index] ^= static_cast<char>(0x01);
        decoded = std::make_unique<minikv::TableData>(99, minikv::Options{});
        minikv::test::Expect(
            minikv::DecodeTable(damaged, {}, &decoded).code() ==
                minikv::StatusCode::kCorruption,
            "every single-byte table mutation must be rejected"
        );
        minikv::test::Expect(
            decoded == nullptr,
            "failed table decoding must clear its output"
        );
    }
}

void TestPublishCreatesOnlyFinalSyncedTable() {
    TemporaryDirectory directory("/tmp/minikv-v3-table-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    minikv::PosixFlushEnvironment environment;
    std::unique_ptr<minikv::TableData> published;
    minikv::test::Expect(
        minikv::PublishTable(
            directory.path(),
            11,
            SampleMemTable(),
            {},
            environment,
            &published
        ).ok(),
        "POSIX table publication must succeed"
    );
    const fs::path final_path =
        fs::path(directory.path()) / minikv::GenerationFileName(11, "sst");
    minikv::test::Expect(fs::exists(final_path), "final table must exist");
    minikv::test::Expect(
        !fs::exists(final_path.string() + ".tmp"),
        "temporary table must not remain after publication"
    );
    minikv::test::Expect(
        published != nullptr && published->records.Get("gamma").value == "three",
        "publication must return the immutable read view"
    );
}

void TestPublishFailureOutputs() {
    TemporaryDirectory directory("/tmp/minikv-v3-table-fail-XXXXXX");
    if (!directory.valid()) {
        return;
    }
    const minikv::test::FlushFailurePoint failures[] = {
        minikv::test::FlushFailurePoint::kCreate,
        minikv::test::FlushFailurePoint::kWrite,
        minikv::test::FlushFailurePoint::kFileSync,
        minikv::test::FlushFailurePoint::kRename,
        minikv::test::FlushFailurePoint::kDirectorySync,
    };
    for (const auto failure : failures) {
        const std::uint64_t generation =
            static_cast<std::uint64_t>(20) +
            static_cast<std::uint64_t>(failure);
        const fs::path final_path =
            fs::path(directory.path()) /
            minikv::GenerationFileName(generation, "sst");
        auto environment =
            std::make_shared<minikv::test::FaultInjectingFlushEnvironment>();
        environment->Arm(failure);
        std::unique_ptr<minikv::TableData> published =
            std::make_unique<minikv::TableData>(99, minikv::Options{});
        minikv::test::Expect(
            minikv::PublishTable(
                directory.path(),
                generation,
                SampleMemTable(),
                {},
                *environment,
                &published
            ).code() == minikv::StatusCode::kIOError,
            "each publication failure point must propagate"
        );
        minikv::test::Expect(
            published == nullptr,
            "failed publication must not expose a table"
        );
        minikv::test::Expect(
            fs::exists(final_path) ==
                (failure == minikv::test::FlushFailurePoint::kDirectorySync),
            "final table name may appear only after rename succeeds"
        );
        minikv::test::Expect(
            !fs::exists(final_path.string() + ".tmp"),
            "handled publication failures must clean the temporary table"
        );
    }
}

}  // namespace

int main() {
    TestTableRoundTripAndCorruption();
    TestPublishCreatesOnlyFinalSyncedTable();
    TestPublishFailureOutputs();
    return minikv::test::Finish("table and flush");
}
