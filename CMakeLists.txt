cmake_minimum_required(VERSION 3.16)
project(OceanEterna VERSION 9.1 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# release flags — -O3 -march=native mirrors the original g++ invocations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

# ---------------------------------------------------------------------------
# dependency detection
# ---------------------------------------------------------------------------

# zstd (required for both bulk_build and server)
find_library(ZSTD_LIB zstd)
find_path(ZSTD_INCLUDE zstd.h)
if(NOT ZSTD_LIB OR NOT ZSTD_INCLUDE)
    message(FATAL_ERROR
        "Could not find zstd. Install with: sudo apt install libzstd-dev")
endif()
message(STATUS "Found zstd: ${ZSTD_LIB}")

# lz4 (required for server)
find_library(LZ4_LIB lz4)
find_path(LZ4_INCLUDE lz4frame.h)
if(NOT LZ4_LIB OR NOT LZ4_INCLUDE)
    message(FATAL_ERROR
        "Could not find lz4. Install with: sudo apt install liblz4-dev")
endif()
message(STATUS "Found lz4: ${LZ4_LIB}")

# curl (required for server)
find_package(CURL REQUIRED)
if(NOT CURL_FOUND)
    message(FATAL_ERROR
        "Could not find curl. Install with: sudo apt install libcurl4-openssl-dev")
endif()
message(STATUS "Found curl: ${CURL_LIBRARIES}")

# pthreads (required)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
if(NOT Threads_FOUND)
    message(FATAL_ERROR
        "Could not find pthreads. Install with: sudo apt install libc6-dev")
endif()

# openmp (optional but recommended)
find_package(OpenMP)
if(NOT OpenMP_CXX_FOUND)
    message(WARNING
        "OpenMP not found — builds will work but be single-threaded. "
        "Install with: sudo apt install libomp-dev")
endif()

# ---------------------------------------------------------------------------
# helper: add targets for one version directory
# ---------------------------------------------------------------------------
function(add_version_targets VERSION_DIR)
    set(SRC_DIR ${CMAKE_SOURCE_DIR}/${VERSION_DIR})

    # --- bulk_build ---
    set(BB_TARGET "bulk_build_${VERSION_DIR}")
    add_executable(${BB_TARGET} ${SRC_DIR}/bulk_build.cpp)

    target_include_directories(${BB_TARGET} PRIVATE
        ${SRC_DIR}              # porter_stemmer.hpp etc.
        ${ZSTD_INCLUDE}
    )

    target_link_libraries(${BB_TARGET} PRIVATE
        ${ZSTD_LIB}
        Threads::Threads
    )

    if(OpenMP_CXX_FOUND)
        target_link_libraries(${BB_TARGET} PRIVATE OpenMP::OpenMP_CXX)
    endif()

    # --- ocean_chat_server ---
    set(SRV_TARGET "ocean_chat_server_${VERSION_DIR}")
    add_executable(${SRV_TARGET} ${SRC_DIR}/ocean_chat_server.cpp)

    target_include_directories(${SRV_TARGET} PRIVATE
        ${SRC_DIR}              # porter_stemmer.hpp, config.hpp, search_engine.hpp, etc.
        ${CMAKE_SOURCE_DIR}     # httplib.h, json.hpp (parent directory)
        ${ZSTD_INCLUDE}
        ${LZ4_INCLUDE}
        ${CURL_INCLUDE_DIRS}
    )

    target_link_libraries(${SRV_TARGET} PRIVATE
        ${ZSTD_LIB}
        ${LZ4_LIB}
        CURL::libcurl
        Threads::Threads
    )

    if(OpenMP_CXX_FOUND)
        target_link_libraries(${SRV_TARGET} PRIVATE OpenMP::OpenMP_CXX)
    endif()

    # put binaries into the version directory
    set_target_properties(${BB_TARGET} ${SRV_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${VERSION_DIR}
    )

    message(STATUS "Configured targets for ${VERSION_DIR}: ${BB_TARGET}, ${SRV_TARGET}")
endfunction()

# ---------------------------------------------------------------------------
# build both versions
# ---------------------------------------------------------------------------
add_version_targets("v8.0")
add_version_targets("v9.0")

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== Ocean Eterna v9.1 Build Configuration ===")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  zstd:        ${ZSTD_LIB}")
message(STATUS "  lz4:         ${LZ4_LIB}")
message(STATUS "  curl:        ${CURL_LIBRARIES}")
message(STATUS "  OpenMP:      ${OpenMP_CXX_FOUND}")
message(STATUS "  Targets:     bulk_build_v8.0  ocean_chat_server_v8.0")
message(STATUS "               bulk_build_v9.0  ocean_chat_server_v9.0")
message(STATUS "==============================================")
