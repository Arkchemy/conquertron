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

# Ground-truth verification tools. These answer "what does the REAL retail
# binary actually contain at this address", by reusing this project's own
# already-tested elf_loader/disassembler against a real dumped RPX -- no
# emulator involved, so nothing to crash or misreport. Both were written
# while root-causing real recompiler bugs (verify_vtable.cpp found the
# ADDIC relocation-folding bug by comparing a computed vtable address
# against the real .rodata; find_synth_addr.cpp reverses a synthetic
# PpcContext::mem address observed live on hardware back to its real Wii U
# vaddr, which is how the igCafeSystemMemoryPool vtable methods were
# identified without another hardware round).
#
# They were previously buildable only by hand-typing a g++ command with the
# right include/lib paths -- easy to get wrong, and easy to forget these
# exist at all. Wired into the normal build so `cmake --build` produces
# them alongside recomp itself.
foreach(_tool verify_vtable find_synth_addr)
    add_executable(${_tool}
        src/${_tool}.cpp
        src/elf_loader.cpp
        src/disassembler.cpp
        src/func_recovery.cpp
    )
    target_include_directories(${_tool} PRIVATE include "${CAPSTONE_INCLUDE_DIR}")
    target_link_libraries(${_tool} PRIVATE "${CAPSTONE_LIBRARY}" ZLIB::ZLIB)
endforeach()
