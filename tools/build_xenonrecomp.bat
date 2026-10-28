@echo off
rem Build semua tool (XenonAnalyse, XenonRecomp, XexDump) ke tools\build.
call "D:\Projek\Visual Studio\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set SRC=%~dp0
cmake -S "%SRC%." -B "%SRC%build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
cmake --build "%SRC%build"
