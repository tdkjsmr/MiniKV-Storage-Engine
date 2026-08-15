[English](README.md) | [中文](README_zh.md)

# MiniKV Storage Engine

MiniKV is a small embedded key-value storage engine for Linux, written in
C++17. It is an educational LSM-style engine with durable writes, recovery,
SSTables, Bloom filters, compaction, range scans, background maintenance, and
reproducible tests and benchmarks.

Current package version: **1.0.0**.

## Highlights

- binary-safe `Put`, `Get`, and `Delete`;
- strict or asynchronous WAL durability;
- crash recovery and checked on-disk formats;
- indexed SSTables, Bloom filters, and L0-to-L1 compaction;
- range and prefix scans with continuation tokens;
- single-writer/multi-reader access and optional background maintenance;
- CMake installation and the exported `minikv::minikv` target.

For storage formats, invariants, recovery boundaries, concurrency semantics,
benchmark methodology, and the V0–V11 roadmap, see
[Development Details](DEVELOPMENT_DETAILS.md).

## Requirements

- Linux
- CMake 3.16 or newer
- a C++17 compiler such as GCC 9 or newer

## Download a prebuilt package

The GitHub Release provides a Linux x86-64 SDK that can be used without
cloning this repository:

```bash
curl -LO https://github.com/tdkjsmr/MiniKV-Storage-Engine/releases/download/v1.0.0/minikv-1.0.0-linux-x86_64.tar.gz
curl -LO https://github.com/tdkjsmr/MiniKV-Storage-Engine/releases/download/v1.0.0/SHA256SUMS
sha256sum --check SHA256SUMS
tar -xzf minikv-1.0.0-linux-x86_64.tar.gz
```

Use the extracted directory as the CMake prefix:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/minikv-1.0.0-linux-x86_64
```

The package is built on Ubuntu 22.04 and contains a static C++ library, headers,
tools, and CMake metadata. Consumers still need a C++17 toolchain and compatible
Linux, glibc, and libstdc++ runtimes.

## Build and test from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The normal test suite includes a package-install smoke test. ASan and UBSan
can be enabled with `MINIKV_ENABLE_ASAN` and `MINIKV_ENABLE_UBSAN` in separate
build directories.

## Install from source

Install into a local prefix without changing the host system:

```bash
cmake --install build --prefix "$PWD/minikv-prefix"
```

The prefix contains:

```text
bin/minikv_benchmark
bin/minikv_sstable_dump
include/minikv/*.hpp
lib/libminikv.a
lib/cmake/MiniKV/
```

Generate a TGZ package with:

```bash
cpack --config build/CPackConfig.cmake -G TGZ
```

## Use from another CMake project

Configure the consumer with the MiniKV installation prefix:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/minikv-prefix
cmake --build build --parallel
```

Consumer `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_store LANGUAGES CXX)

find_package(MiniKV 1.0 CONFIG REQUIRED)

add_executable(my_store main.cpp)
target_link_libraries(my_store PRIVATE minikv::minikv)
target_compile_features(my_store PRIVATE cxx_std_17)
```

Minimal `main.cpp`:

```cpp
#include <iostream>
#include <memory>

#include "minikv/database.hpp"

int main() {
    minikv::Options options;
    std::unique_ptr<minikv::Database> database;
    minikv::DatabaseOpenResult open_result;

    auto status = minikv::Database::Open(
        "./example-data",
        options,
        &database,
        &open_result
    );
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }

    status = database->Put("hello", "MiniKV");
    const auto result = database->Get("hello");
    if (!status.ok() || !result.status.ok()) {
        return 2;
    }

    std::cout << result.value << '\n';
    return database->Close().ok() ? 0 : 3;
}
```

Strict WAL synchronization is the default. Applications that explicitly
accept possible loss of recently acknowledged writes after a crash may select
asynchronous writes through `WriteOptions`.

## Included tools

Run the fixed benchmark matrix:

```bash
./bench/run_matrix.sh ./build/minikv_benchmark \
  benchmark-results/matrix > benchmark-results/matrix.jsonl
```

Inspect an SSTable without opening the database:

```bash
./build/minikv_sstable_dump /path/to/table.sst 20
```

## Scope

MiniKV is a single-node embedded engine. It does not provide SQL, networking,
replication, distributed consensus, MVCC, or multi-key transactions. It is a
learning-oriented storage engine rather than a production replacement for
RocksDB.
