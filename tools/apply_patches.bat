@echo off
rem Terapkan perubahan DW6 ke XenonRecomp (submodule) setelah clone baru.
rem Jalankan sekali setelah: git submodule update --init --recursive
git -C "%~dp0XenonRecomp" apply "%~dp0..\patches\XenonRecomp-dw6.patch"
