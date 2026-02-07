cmake_minimum_required(VERSION 3.16)
project(bldr_saucer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependency source directories (default to go mod vendor paths).
if(NOT DEFINED SAUCER_SOURCE_DIR)
    set(SAUCER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/vendor/github.com/aperturerobotics/saucer")
endif()
if(NOT DEFINED YAMUX_SOURCE_DIR)
    set(YAMUX_SOURCE_DIR "${CMAKE_SOURCE_DIR}/vendor/github.com/aperturerobotics/cpp-yamux")
endif()

# Saucer options.
set(saucer_static ON CACHE BOOL "" FORCE)
set(saucer_serializer "Glaze" CACHE STRING "" FORCE)
set(saucer_no_version_check ON CACHE BOOL "" FORCE)
set(saucer_examples OFF CACHE BOOL "" FORCE)
set(saucer_tests OFF CACHE BOOL "" FORCE)

# cpp-yamux options.
set(YAMUX_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${SAUCER_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/saucer)
add_subdirectory(${YAMUX_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/yamux)

add_executable(bldr-saucer
    src/main.cpp
    src/pipe_client.cpp
    src/fetch_proto.cpp
    src/scheme_forwarder.cpp
)

target_link_libraries(bldr-saucer PRIVATE saucer::saucer yamux)
target_compile_features(bldr-saucer PRIVATE cxx_std_23)

# Platform-specific socket libraries.
if(WIN32)
    target_link_libraries(bldr-saucer PRIVATE ws2_32)
endif()

install(TARGETS bldr-saucer RUNTIME DESTINATION bin)
