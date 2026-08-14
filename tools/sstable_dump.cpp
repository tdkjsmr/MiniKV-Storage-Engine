#include "minikv/table.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Hex(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

bool ParseLimit(std::string_view text, std::size_t* limit) {
    if (limit == nullptr || text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *limit);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: minikv_sstable_dump <table.sst> [record-limit]\n";
        return 2;
    }
    std::size_t limit = 20;
    if (argc == 3 && !ParseLimit(argv[2], &limit)) {
        std::cerr << "record-limit must be a non-negative integer\n";
        return 2;
    }

    std::unique_ptr<minikv::SSTableReader> reader;
    const auto status = minikv::SSTableReader::Open(argv[1], {}, &reader);
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }
    const auto& metadata = reader->metadata();
    std::cout << "path=" << reader->path() << '\n'
              << "format_version=" << static_cast<unsigned>(minikv::kTableFormatVersion) << '\n'
              << "generation=" << metadata.generation << '\n'
              << "file_size=" << metadata.file_size << '\n'
              << "record_count=" << metadata.record_count << '\n'
              << "block_count=" << metadata.block_count << '\n'
              << "sequence_range=[" << metadata.minimum_sequence << ','
              << metadata.maximum_sequence << "]\n"
              << "key_range_hex=[" << Hex(metadata.minimum_key) << ','
              << Hex(metadata.maximum_key) << "]\n"
              << "data_region=[" << metadata.data_offset << ','
              << metadata.data_size << "]\n"
              << "index_region=[" << metadata.index_offset << ','
              << metadata.index_size << "]\n"
              << "bloom_region=[" << metadata.bloom_offset << ','
              << metadata.bloom_size << "]\n"
              << "bloom_bits=" << metadata.bloom_bit_count << '\n'
              << "bloom_hashes="
              << static_cast<unsigned>(metadata.bloom_hash_count) << '\n';

    std::vector<minikv::MemTableRecord> records;
    const auto read_status = reader->ReadRecords(limit, &records);
    if (!read_status.ok()) {
        std::cerr << read_status.ToString() << '\n';
        return 1;
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        std::cout << "record[" << index << "] sequence=" << record.sequence
                  << " type="
                  << (record.type == minikv::ValueType::kDeletion ? "deletion" : "value")
                  << " key_hex=" << Hex(record.key)
                  << " value_hex=" << Hex(record.value) << '\n';
    }
    return 0;
}
