@echo off
setlocal
set "TB_PRESET=%~1"
if "%TB_PRESET%"=="" set "TB_PRESET=debug"
if /I "%TB_PRESET%"=="fmt" goto fmt
if /I "%TB_PRESET%"=="check-format" goto checkformat
if /I "%TB_PRESET%"=="test" set "TB_PRESET=debug"
if /I "%TB_PRESET%"=="debug" goto build
if /I "%TB_PRESET%"=="release" goto build
if /I "%TB_PRESET%"=="sanitizer" goto build
echo Usage: build.bat [debug^|release^|test^|sanitizer^|fmt^|check-format]
exit /b 1
:build
rem Use an x64 Native Tools command prompt so Ninja finds MSVC and ASan DLLs.
cmake --preset %TB_PRESET%
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset %TB_PRESET%
if errorlevel 1 exit /b %errorlevel%
ctest --preset %TB_PRESET%
exit /b %errorlevel%
:fmt
cmake -DTB_FORMAT_MODE=write -P cmake/Format.cmake
exit /b %errorlevel%
:checkformat
cmake -DTB_FORMAT_MODE=check -P cmake/Format.cmake
exit /b %errorlevel%
