#include "minikv/compaction.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <queue>
#include <set>
#include <string_view>
#include <utility>

#include "minikv/file.hpp"
#include "minikv/flush.hpp"

namespace minikv {
namespace {

namespace fs = std::filesystem;

struct MergeCursor {
    std::size_t source = 0;
    std::size_t record = 0;
};

struct CursorLater {
    const std::vector<std::vector<MemTableRecord>>* records = nullptr;

    bool operator()(const MergeCursor& left, const MergeCursor& right) const {
        const auto& left_record = (*records)[left.source][left.record];
        const auto& right_record = (*records)[right.source][right.record];
        if (left_record.key != right_record.key) {
            return left_record.key > right_record.key;
        }
        return left_record.sequence < right_record.sequence;
    }
};

struct PreparedOutput {
    std::uint64_t file_number = 0;
    std::string encoded;
    std::string temporary_path;
    std::string final_path;
};

Status CompactionCorruption(std::string message) {
    return Status::Corruption("compaction: " + std::move(message));
}

bool CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output
) {
    if (output == nullptr ||
        left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *output = left + right;
    return true;
}

std::size_t ApproximateRecordSize(const MemTableRecord& record) {
    return sizeof(std::uint64_t) + sizeof(ValueType) + record.key.size() +
           record.value.size();
}

Status AddOutputRecord(
    const MemTableRecord& record,
    const Options& options,
    std::vector<MemTable>* outputs
) {
    if (outputs->empty()) {
        outputs->emplace_back(options);
    }
    auto* current = &outputs->back();
    const std::size_t record_size = ApproximateRecordSize(record);
    if (!current->empty() &&
        (record_size > options.compaction_output_size_limit ||
         current->ApproximateDataSize() >
             options.compaction_output_size_limit - record_size)) {
        outputs->emplace_back(options);
        current = &outputs->back();
    }
    return record.type == ValueType::kDeletion
        ? current->Delete(record.sequence, record.key)
        : current->Put(record.sequence, record.key, record.value);
}

void BestEffortRemove(
    FlushEnvironment& environment,
    const std::vector<PreparedOutput>& outputs
) {
    for (const auto& output : outputs) {
        const auto status = environment.RemoveFile(output.temporary_path);
        (void)status;
    }
}

}  // namespace

Status BuildCompactionOutputs(
    const std::vector<CompactionInput>& inputs,
    const Options& options,
    bool may_drop_tombstones,
    std::vector<MemTable>* outputs,
    CompactionBuildStats* stats
) {
    if (outputs == nullptr || stats == nullptr) {
        return Status::InvalidArgument("compaction outputs must not be null");
    }
    outputs->clear();
    *stats = {};
    if (inputs.empty() || options.compaction_output_size_limit == 0) {
        return Status::InvalidArgument(
            "compaction requires inputs and a non-zero output size limit"
        );
    }

    std::set<std::uint64_t> file_numbers;
    std::vector<std::vector<MemTableRecord>> sources;
    sources.reserve(inputs.size());
    for (const auto& input : inputs) {
        if (input.table == nullptr || input.level > 1 ||
            !file_numbers.insert(input.table->metadata().generation).second ||
            input.table->metadata().record_count >
                std::numeric_limits<std::size_t>::max()) {
            return Status::InvalidArgument("compaction input is invalid");
        }
        std::vector<MemTableRecord> records;
        const auto status = input.table->ReadRecords(
            static_cast<std::size_t>(input.table->metadata().record_count),
            &records
        );
        if (!status.ok()) {
            return status;
        }
        if (records.size() != input.table->metadata().record_count) {
            return CompactionCorruption("input record count changed while reading");
        }
        if (!CheckedAdd(stats->input_records, records.size(), &stats->input_records)) {
            return Status::InvalidArgument("compaction record count overflows");
        }
        sources.push_back(std::move(records));
    }

    CursorLater comparison{&sources};
    std::priority_queue<MergeCursor, std::vector<MergeCursor>, CursorLater> heap(
        comparison
    );
    for (std::size_t source = 0; source < sources.size(); ++source) {
        if (!sources[source].empty()) {
            heap.push({source, 0});
        }
    }

    while (!heap.empty()) {
        const std::string key =
            sources[heap.top().source][heap.top().record].key;
        const MemTableRecord* winner = nullptr;
        std::vector<MergeCursor> same_key;
        while (!heap.empty()) {
            const auto cursor = heap.top();
            const auto& record = sources[cursor.source][cursor.record];
            if (record.key != key) {
                break;
            }
            heap.pop();
            same_key.push_back(cursor);
            if (winner == nullptr || record.sequence > winner->sequence) {
                winner = &record;
            } else if (record.sequence == winner->sequence) {
                outputs->clear();
                return CompactionCorruption(
                    "two input records reuse one global Sequence number"
                );
            }
        }

        if (winner == nullptr) {
            outputs->clear();
            return CompactionCorruption("merge heap lost a user key");
        }
        stats->duplicate_records_dropped +=
            static_cast<std::uint64_t>(same_key.size() - 1U);
        if (winner->type == ValueType::kDeletion && may_drop_tombstones) {
            ++stats->tombstones_dropped;
        } else {
            const auto status = AddOutputRecord(*winner, options, outputs);
            if (!status.ok()) {
                outputs->clear();
                return status;
            }
            ++stats->output_records;
        }

        for (auto cursor : same_key) {
            ++cursor.record;
            if (cursor.record < sources[cursor.source].size()) {
                heap.push(cursor);
            }
        }
    }
    return Status::Ok();
}

Status PublishCompactionTables(
    const std::string& directory,
    std::uint64_t first_file_number,
    const std::vector<MemTable>& outputs,
    const Options& options,
    FlushEnvironment& environment,
    std::vector<std::unique_ptr<SSTableReader>>* published
) {
    if (published == nullptr) {
        return Status::InvalidArgument("published compaction output is null");
    }
    published->clear();
    if (directory.empty() || first_file_number == 0) {
        return Status::InvalidArgument("compaction publication arguments are invalid");
    }
    if (outputs.empty()) {
        return Status::Ok();
    }
    if (outputs.size() > std::numeric_limits<std::uint64_t>::max() -
            first_file_number) {
        return Status::InvalidArgument("compaction file numbers overflow");
    }

    std::vector<PreparedOutput> prepared;
    prepared.reserve(outputs.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        PreparedOutput output;
        output.file_number =
            first_file_number + static_cast<std::uint64_t>(index);
        auto status = EncodeTable(
            output.file_number,
            outputs[index],
            options,
            &output.encoded
        );
        if (!status.ok()) {
            return status;
        }
        output.final_path = (
            fs::path(directory) /
            GenerationFileName(output.file_number, "sst")
        ).string();
        output.temporary_path = output.final_path + ".tmp";
        prepared.push_back(std::move(output));
    }

    for (const auto& output : prepared) {
        std::unique_ptr<WritableFile> file;
        auto status = environment.CreateTruncated(output.temporary_path, &file);
        if (!status.ok()) {
            BestEffortRemove(environment, prepared);
            return status;
        }
        status = WriteAll(*file, output.encoded);
        if (!status.ok()) {
            file.reset();
            BestEffortRemove(environment, prepared);
            return status;
        }
        status = file->Sync();
        file.reset();
        if (!status.ok()) {
            BestEffortRemove(environment, prepared);
            return status;
        }
    }

    for (const auto& output : prepared) {
        const auto status = environment.Rename(
            output.temporary_path,
            output.final_path
        );
        if (!status.ok()) {
            BestEffortRemove(environment, prepared);
            return status;
        }
    }
    auto status = environment.SyncDirectory(directory);
    if (!status.ok()) {
        return status;
    }

    published->reserve(prepared.size());
    for (const auto& output : prepared) {
        std::unique_ptr<SSTableReader> table;
        status = SSTableReader::Open(output.final_path, options, &table);
        if (!status.ok()) {
            published->clear();
            return status;
        }
        published->push_back(std::move(table));
    }
    return Status::Ok();
}

}  // namespace minikv
