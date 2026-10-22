cmake_minimum_required(VERSION 3.15)
project(udp_transfer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(udp_transfer
    src/main.c
    src/transport.c
    src/transport_utils.c
    src/transport_sender.c
    src/transport_receiver.c
    src/bbr.c
    src/unit_tests.c
)

target_include_directories(udp_transfer PRIVATE
    include
)

target_link_libraries(udp_transfer PRIVATE m pthread)

if(UNIX AND NOT APPLE)
    target_link_libraries(udp_transfer PRIVATE dl rt)
endif()