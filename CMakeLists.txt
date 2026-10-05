cmake_minimum_required(VERSION 3.15)
project(udp_transfer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(raylib REQUIRED)

find_library(ENET_LIB enet)
find_path(ENET_INCLUDE enet/enet.h)

if(NOT ENET_LIB OR NOT ENET_INCLUDE)
    message(FATAL_ERROR "ENet not found. Install libenet-dev or build from source.")
endif()

add_executable(udp_transfer
    src/main.c
    src/model.c
    src/viewmodel.c
    src/view.c
    src/transport.c
    src/proxy.c
)

target_include_directories(udp_transfer PRIVATE
    include
    third_party
    ${ENET_INCLUDE}
)

target_link_libraries(udp_transfer PRIVATE raylib ${ENET_LIB} m pthread)

if(UNIX AND NOT APPLE)
    target_link_libraries(udp_transfer PRIVATE dl rt X11)
endif()

# ---- Copy resources/ into the build directory after each build ----
add_custom_command(TARGET udp_transfer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:udp_transfer>/resources"
    COMMENT "Copying resources/ to build output directory"
)

# Optional: also copy resources/ next to the source tree's own build dir
# if you build out-of-source (the default). The command above handles that.

