cmake_minimum_required(VERSION 3.15)
project(udp_transfer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(raylib REQUIRED)

add_executable(udp_transfer
    src/main.c
    src/model.c
    src/viewmodel.c
    src/view.c
    src/transport.c
    src/bbr.c
    src/proxy.c
)

target_include_directories(udp_transfer PRIVATE
    include
    third_party
)

target_link_libraries(udp_transfer PRIVATE raylib m pthread)

if(UNIX AND NOT APPLE)
    target_link_libraries(udp_transfer PRIVATE dl rt X11)
endif()

add_custom_command(TARGET udp_transfer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:udp_transfer>/resources"
    COMMENT "Copying resources/ to build output directory"
)

