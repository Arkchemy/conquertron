cmake_minimum_required(VERSION 3.16)
project(arkchemy_recomp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Capstone has no system package in this project's dev environment; point
# CAPSTONE_PREFIX at a local build (see tools/setup_toolchain.sh) if the
# default doesn't match your machine.
set(CAPSTONE_PREFIX "$ENV{HOME}/devtools/capstone-install" CACHE PATH "Capstone install prefix")

find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h HINTS "${CAPSTONE_PREFIX}/include")
find_library(CAPSTONE_LIBRARY NAMES capstone HINTS "${CAPSTONE_PREFIX}/lib" "${CAPSTONE_PREFIX}/lib64")

if(NOT CAPSTONE_INCLUDE_DIR OR NOT CAPSTONE_LIBRARY)
    message(FATAL_ERROR "Capstone not found. Set -DCAPSTONE_PREFIX=<path> to its install prefix.")
endif()

# Real .rpx/.rpl files commonly store most sections zlib-compressed
# (SHF_RPL_ZLIB) to shrink retail game file size -- confirmed against an
# actual Wii U game disc, not guessed at. `recomp` itself is a host tool,
# so linking the system zlib here has no bearing on anything that
# eventually runs on Switch.
find_package(ZLIB REQUIRED)

add_executable(recomp
    src/main.cpp
    src/elf_loader.cpp
    src/disassembler.cpp
    src/codegen.cpp
    src/func_recovery.cpp
)
target_include_directories(recomp PRIVATE include "${CAPSTONE_INCLUDE_DIR}")
target_link_libraries(recomp PRIVATE "${CAPSTONE_LIBRARY}" ZLIB::ZLIB)
