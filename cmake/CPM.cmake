# CPM.cmake - CMake's Package Manager
# https://github.com/cpm-cmake/CPM.cmake
#
# This file will be downloaded automatically on first configure if not present.

set(CPM_DOWNLOAD_VERSION 0.42.1)
set(CPM_DOWNLOAD_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
    message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        STATUS download_status
    )
    list(GET download_status 0 download_status_code)
    if(NOT download_status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download CPM.cmake")
    endif()
endif()

include(${CPM_DOWNLOAD_LOCATION})
