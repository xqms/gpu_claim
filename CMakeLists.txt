
cmake_minimum_required(VERSION 3.10)
project(gpu_claim)

set(CMAKE_CXX_STANDARD 20)

find_package(CUDAToolkit REQUIRED)

find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(gpu_server
    src/server.cpp
)
target_include_directories(gpu_server PRIVATE
    contrib/zpp_bits
)
target_link_libraries(gpu_server PRIVATE
    CUDA::nvml
)

add_executable(gpu
    src/client.cpp
)
target_include_directories(gpu PRIVATE
    contrib/zpp_bits
)
target_link_libraries(gpu PRIVATE
    Boost::program_options
)
