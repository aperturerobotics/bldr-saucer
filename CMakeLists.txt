cmake_minimum_required(VERSION 3.16)
project(bldr_saucer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CPM.cmake for dependency management
# Use .cache/cpm for downloaded sources
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cache/cpm")
include(cmake/CPM.cmake)

# Fetch Saucer library via CPM
# Branch streaming-custom-scheme with handle_stream_scheme API
CPMFindPackage(
    NAME           saucer
    GIT_REPOSITORY "https://github.com/paralin/saucer"
    GIT_TAG        "cf1d208e333a9fc3584a279f09d5f2583248ce11"
    OPTIONS
        "saucer_static ON"
        "saucer_serializer Glaze"
)

# Fetch cpp-yamux via CPM
CPMFindPackage(
    NAME           yamux
    GIT_REPOSITORY "https://github.com/aperturerobotics/cpp-yamux"
    GIT_TAG        "0e230930a899982a93de91a7ce6fa10a8d503bfb"
    OPTIONS
        "YAMUX_BUILD_TESTS OFF"
)

add_executable(bldr-saucer
    src/main.cpp
    src/pipe_client.cpp
    src/fetch_proto.cpp
    src/scheme_forwarder.cpp
)

target_link_libraries(bldr-saucer PRIVATE saucer::saucer yamux)
target_compile_features(bldr-saucer PRIVATE cxx_std_23)

# Platform-specific socket libraries
if(WIN32)
    target_link_libraries(bldr-saucer PRIVATE ws2_32)
endif()

# Install target
install(TARGETS bldr-saucer RUNTIME DESTINATION bin)
