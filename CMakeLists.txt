cmake_minimum_required(VERSION 3.16)

project(
    minikv
    VERSION 0.9.0
    DESCRIPTION "A small C++17 LSM-style key-value storage engine"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIKV_BUILD_TESTS "Build MiniKV tests" ON)
option(MINIKV_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINIKV_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINIKV_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(MINIKV_ENABLE_TSAN AND (MINIKV_ENABLE_ASAN OR MINIKV_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan must be enabled in a separate build")
endif()

function(minikv_set_target_options target_name)
    target_compile_options(
        ${target_name}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
    )

    if(MINIKV_ENABLE_ASAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address)
    endif()

    if(MINIKV_ENABLE_UBSAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=undefined)
    endif()

    if(MINIKV_ENABLE_TSAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=thread)
    endif()
endfunction()

add_library(
    minikv
    src/bloom_filter.cpp
    src/coding.cpp
    src/compaction.cpp
    src/database_lock.cpp
    src/database.cpp
    src/file.cpp
    src/flush.cpp
    src/memtable.cpp
    src/manifest.cpp
    src/recovery.cpp
    src/scan.cpp
    src/status.cpp
    src/table.cpp
    src/wal.cpp
    src/write_path.cpp
)
add_library(minikv::minikv ALIAS minikv)

target_include_directories(
    minikv
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(minikv PUBLIC Threads::Threads)
minikv_set_target_options(minikv)

add_executable(minikv_sstable_dump tools/sstable_dump.cpp)
target_link_libraries(minikv_sstable_dump PRIVATE minikv::minikv)
minikv_set_target_options(minikv_sstable_dump)

if(MINIKV_BUILD_TESTS)
    include(CTest)

    add_executable(minikv_memtable_test tests/memtable_test.cpp)
    target_link_libraries(minikv_memtable_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_memtable_test)

    add_test(NAME memtable COMMAND minikv_memtable_test)

    add_executable(minikv_coding_test tests/coding_test.cpp)
    target_link_libraries(minikv_coding_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_coding_test)
    add_test(NAME coding COMMAND minikv_coding_test)

    add_executable(minikv_wal_test tests/wal_test.cpp)
    target_link_libraries(minikv_wal_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_wal_test)
    add_test(NAME wal COMMAND minikv_wal_test)

    add_executable(minikv_write_path_test tests/write_path_test.cpp)
    target_link_libraries(minikv_write_path_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_write_path_test)
    add_test(NAME write_path COMMAND minikv_write_path_test)

    add_executable(minikv_recovery_test tests/recovery_test.cpp)
    target_link_libraries(minikv_recovery_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_recovery_test)
    add_test(NAME recovery COMMAND minikv_recovery_test)

    add_executable(minikv_table_flush_test tests/table_flush_test.cpp)
    target_link_libraries(minikv_table_flush_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_table_flush_test)
    add_test(NAME table_flush COMMAND minikv_table_flush_test)

    add_executable(minikv_database_test tests/database_test.cpp)
    target_link_libraries(minikv_database_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_database_test)
    add_test(NAME database COMMAND minikv_database_test)

    add_executable(minikv_manifest_test tests/manifest_test.cpp)
    target_link_libraries(minikv_manifest_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_manifest_test)
    add_test(NAME manifest COMMAND minikv_manifest_test)

    add_executable(minikv_bloom_filter_test tests/bloom_filter_test.cpp)
    target_link_libraries(minikv_bloom_filter_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_bloom_filter_test)
    add_test(NAME bloom_filter COMMAND minikv_bloom_filter_test)

    add_executable(minikv_compaction_test tests/compaction_test.cpp)
    target_link_libraries(minikv_compaction_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_compaction_test)
    add_test(NAME compaction COMMAND minikv_compaction_test)

    add_executable(minikv_range_scan_test tests/range_scan_test.cpp)
    target_link_libraries(minikv_range_scan_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_range_scan_test)
    add_test(NAME range_scan COMMAND minikv_range_scan_test)

    add_executable(minikv_concurrency_test tests/concurrency_test.cpp)
    target_link_libraries(minikv_concurrency_test PRIVATE minikv::minikv)
    minikv_set_target_options(minikv_concurrency_test)
    add_test(NAME concurrency COMMAND minikv_concurrency_test)
endif()
