if(NOT DEFINED MINIKV_SOURCE_DIR OR
   NOT DEFINED MINIKV_BINARY_DIR OR
   NOT DEFINED MINIKV_ARCHIVE)
    message(FATAL_ERROR "package smoke paths are required")
endif()

set(smoke_root "${MINIKV_BINARY_DIR}/package-smoke")
set(extract_root "${smoke_root}/extract")
set(consumer_build "${smoke_root}/consumer-build")
set(database_path "${smoke_root}/database")
file(REMOVE_RECURSE "${smoke_root}")
file(MAKE_DIRECTORY "${extract_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${MINIKV_ARCHIVE}"
    WORKING_DIRECTORY "${extract_root}"
    RESULT_VARIABLE extract_result
)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "MiniKV package extraction failed: ${extract_result}")
endif()

file(GLOB package_roots LIST_DIRECTORIES true "${extract_root}/minikv-*")
list(LENGTH package_roots package_root_count)
if(NOT package_root_count EQUAL 1)
    message(FATAL_ERROR "expected one MiniKV package root, found ${package_root_count}")
endif()
list(GET package_roots 0 package_prefix)
if(NOT EXISTS "${package_prefix}/lib/cmake/MiniKV/MiniKVConfig.cmake")
    message(FATAL_ERROR "extracted package does not contain MiniKVConfig.cmake")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${MINIKV_SOURCE_DIR}/tests/install_consumer"
    -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${package_prefix}"
    "-DCMAKE_BUILD_TYPE=Release"
)
if(DEFINED MINIKV_GENERATOR AND NOT MINIKV_GENERATOR STREQUAL "")
    list(APPEND configure_command -G "${MINIKV_GENERATOR}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "packaged consumer configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --parallel 2
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "packaged consumer build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${consumer_build}/minikv_install_consumer" "${database_path}"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "packaged consumer failed: ${run_result}")
endif()
