cmake_minimum_required(VERSION 3.20)

# Proyek induk untuk semua tool: XenonRecomp (upstream, tidak diubah) + tool buatan kita.
project("DW6-Tools")

# Samakan dengan pengaturan XenonRecomp (C++17, runtime statis) supaya bisa di-link bersama.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(XenonRecomp)
add_subdirectory(XexDump)
