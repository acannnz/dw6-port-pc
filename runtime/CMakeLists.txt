cmake_minimum_required(VERSION 3.20)
project("DW6Runtime" CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PPC_DIR ${PROJECT_ROOT}/private/ppc)

# XenonUtils (pembaca XEX) dari XenonRecomp; hanya library yang dipakai yang ikut di-build.
add_subdirectory(${PROJECT_ROOT}/tools/XenonRecomp ${CMAKE_BINARY_DIR}/XenonRecomp EXCLUDE_FROM_ALL)

# Kode C++ hasil XenonRecomp (private/ppc, tidak dibagikan).
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/*.cpp)
add_library(DW6Recomp STATIC ${PPC_SOURCES})
target_include_directories(DW6Recomp PUBLIC ${PPC_DIR} ${PROJECT_ROOT}/tools/XenonRecomp/thirdparty/simde)
# Kode hasil recompile sangat besar: -O1 supaya compile tidak terlalu lama, peringatan dimatikan.
target_compile_options(DW6Recomp PRIVATE -msse4.1 -Wno-everything /clang:-O1 -fno-strict-aliasing -gline-tables-only)

file(GLOB KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)
add_executable(DW6 main.cpp sampler.cpp patches.cpp ${KERNEL_SOURCES})
target_compile_options(DW6 PRIVATE -msse4.1 -gline-tables-only)
# Simbol PDB (nama fungsi sub_XXXXXXXX) untuk sampler debug dan crash log.
target_link_options(DW6 PRIVATE /DEBUG)
target_link_libraries(DW6 PRIVATE DW6Recomp XenonUtils dbghelp)
