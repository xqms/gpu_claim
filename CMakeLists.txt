
cmake_minimum_required(VERSION 3.10)
project(gpu_claim)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 3)
set(VERSION_PATCH 2)

set(CMAKE_CXX_FLAGS "-std=c++20")
add_definitions(
    -DVERSION_MAJOR=${VERSION_MAJOR}
    -DVERSION_MINOR=${VERSION_MINOR}
    -DVERSION_PATCH=${VERSION_PATCH}
)

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

install(TARGETS gpu
    RUNTIME DESTINATION bin
)
install(TARGETS gpu_server
    RUNTIME DESTINATION sbin
)
install(FILES files/gpu_claim.conf
    DESTINATION /etc/modprobe.d/
)
install(FILES files/gpu_server.service
    DESTINATION /lib/systemd/system/
)

set(CPACK_GENERATOR DEB)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Max Schwarz <max.schwarz@ais.uni-bonn.de")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "")
set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/files/postinst;${CMAKE_CURRENT_SOURCE_DIR}/files/prerm")
include(CPack)
