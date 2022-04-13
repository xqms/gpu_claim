
cmake_minimum_required(VERSION 3.10)
project(gpu_claim)

set(CMAKE_CXX_STANDARD 20)

find_package(CUDAToolkit REQUIRED)

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(gpu_server
    src/server.cpp
    src/priority_queue.cpp
)
target_include_directories(gpu_server PRIVATE
    contrib/zpp_bits
)
target_link_libraries(gpu_server PRIVATE
    CUDA::nvml
)
target_link_options(gpu_server PRIVATE
    "-static-libstdc++" "-static-libgcc"
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
target_link_options(gpu PRIVATE
    "-static-libstdc++" "-static-libgcc"
)
