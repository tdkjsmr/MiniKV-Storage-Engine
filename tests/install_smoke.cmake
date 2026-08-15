if(NOT DEFINED MINIKV_SOURCE_DIR OR NOT DEFINED MINIKV_BINARY_DIR)
    message(FATAL_ERROR "install smoke paths are required")
endif()

set(smoke_root "${MINIKV_BINARY_DIR}/install-smoke")
set(install_prefix "${smoke_root}/prefix")
set(consumer_build "${smoke_root}/consumer-build")
set(database_path "${smoke_root}/database")
file(REMOVE_RECURSE "${smoke_root}")

set(install_command
    "${CMAKE_COMMAND}" --install "${MINIKV_BINARY_DIR}"
    --prefix "${install_prefix}"
)
if(DEFINED MINIKV_INSTALL_CONFIG AND NOT MINIKV_INSTALL_CONFIG STREQUAL "")
    list(APPEND install_command --config "${MINIKV_INSTALL_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "MiniKV installation failed: ${install_result}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${MINIKV_SOURCE_DIR}/tests/install_consumer"
        -B "${consumer_build}"
        "-DCMAKE_PREFIX_PATH=${install_prefix}"
        "-DCMAKE_BUILD_TYPE=Release"
        "-G" "${MINIKV_GENERATOR}"
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "installed-package configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --parallel 2
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "installed-package build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${consumer_build}/minikv_install_consumer" "${database_path}"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "installed-package consumer failed: ${run_result}")
endif()
