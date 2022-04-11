
cmake_minimum_required(VERSION 3.10)
project(gpu_claim)

set(CMAKE_CXX_STANDARD 17)

find_package(CUDAToolkit)

add_executable(gpu_server
    src/server.cpp
)
target_link_libraries(gpu_server PRIVATE
    CUDA::nvml
)
