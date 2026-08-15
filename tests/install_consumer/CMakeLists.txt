cmake_minimum_required(VERSION 3.16)

project(minikv_install_consumer LANGUAGES CXX)

find_package(MiniKV 1.0 CONFIG REQUIRED)

add_executable(minikv_install_consumer main.cpp)
target_link_libraries(minikv_install_consumer PRIVATE minikv::minikv)
target_compile_features(minikv_install_consumer PRIVATE cxx_std_17)
