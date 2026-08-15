[English](README.md) | [中文](README_zh.md)

# MiniKV 存储引擎

MiniKV 是一个面向 Linux、使用 C++17 编写的小型嵌入式 Key-Value
存储引擎。它采用教学型 LSM 设计，支持持久化写入、崩溃恢复、
SSTable、Bloom Filter、Compaction、范围扫描、后台维护以及可复现的
测试与 Benchmark。

当前软件包版本：**1.0.0**。

## 主要能力

- 二进制安全的 `Put`、`Get` 和 `Delete`；
- strict 或 asynchronous 两种 WAL 持久化策略；
- 崩溃恢复和带边界、校验和检查的磁盘格式；
- 带索引的 SSTable、Bloom Filter 和 L0-to-L1 Compaction；
- 支持 continuation token 的范围与前缀扫描；
- 单写者、多读者访问以及可选后台维护；
- 标准 CMake 安装和导出的 `minikv::minikv` target。

磁盘格式、核心不变量、恢复边界、并发语义、Benchmark 方法和 V0–V11
路线图，请阅读[开发细节](DEVELOPMENT_DETAILS_zh.md)。

## 环境要求

- Linux
- CMake 3.16 或更高版本
- 支持 C++17 的编译器，例如 GCC 9 或更高版本

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

普通测试套件包含安装包冒烟测试。ASan 和 UBSan 可分别在独立构建目录中
通过 `MINIKV_ENABLE_ASAN` 和 `MINIKV_ENABLE_UBSAN` 开启。

## 安装

安装到本地前缀，不修改主机的系统目录：

```bash
cmake --install build --prefix "$PWD/minikv-prefix"
```

安装目录包含：

```text
bin/minikv_benchmark
bin/minikv_sstable_dump
include/minikv/*.hpp
lib/libminikv.a
lib/cmake/MiniKV/
```

生成 TGZ 安装包：

```bash
cpack --config build/CPackConfig.cmake -G TGZ
```

## 在其他 CMake 项目中使用

使用 MiniKV 安装目录配置 consumer：

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/minikv-prefix
cmake --build build --parallel
```

Consumer 的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_store LANGUAGES CXX)

find_package(MiniKV 1.0 CONFIG REQUIRED)

add_executable(my_store main.cpp)
target_link_libraries(my_store PRIVATE minikv::minikv)
target_compile_features(my_store PRIVATE cxx_std_17)
```

最小 `main.cpp`：

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

strict WAL 同步是默认策略。只有在明确接受崩溃后可能丢失最近已确认写入时，
应用才应通过 `WriteOptions` 选择 asynchronous write。

## 附带工具

运行固定 Benchmark 矩阵：

```bash
./bench/run_matrix.sh ./build/minikv_benchmark \
  benchmark-results/matrix > benchmark-results/matrix.jsonl
```

在不打开数据库的情况下检查 SSTable：

```bash
./build/minikv_sstable_dump /path/to/table.sst 20
```

## 项目边界

MiniKV 是单机嵌入式引擎，不提供 SQL、网络协议、复制、分布式共识、
MVCC 或多 Key 事务。它是用于学习存储研发的项目，不是 RocksDB 的
生产级替代品。
