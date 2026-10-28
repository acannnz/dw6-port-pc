@echo off
rem Cek apakah semua file C++ hasil XenonRecomp (private\ppc) bisa di-compile (tanpa link).
rem Hasil: private\ppc_check\<nama>.log, satu per file; file kosong = lolos.
call "D:\Projek\Visual Studio\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set ROOT=%~dp0..
set SIMDE=%ROOT%\tools\XenonRecomp\thirdparty\simde
if not exist "%ROOT%\private\ppc_check" mkdir "%ROOT%\private\ppc_check"
for %%F in ("%ROOT%\private\ppc\*.cpp") do (
    clang-cl /nologo /std:c++17 /EHsc -msse4.1 -Wno-everything -fsyntax-only -I"%ROOT%\private\ppc" -I"%SIMDE%" "%%F" > "%ROOT%\private\ppc_check\%%~nF.log" 2>&1
)
