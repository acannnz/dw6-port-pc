# Dibangun lewat tools/CMakeLists.txt supaya bisa memakai library XenonUtils dari XenonRecomp.
add_executable(XexDump "main.cpp")
target_link_libraries(XexDump PRIVATE XenonUtils)
