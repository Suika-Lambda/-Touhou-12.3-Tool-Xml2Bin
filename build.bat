@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ============================================================
rem  xml2bin one-click build (single .cpp, no 3rd-party deps, ~1-2 s)
rem  Prefer MSVC (VS2019/2022 or Build Tools), fallback to g++
rem ============================================================

set "HAVE_CL="
set "VCVARS="

rem 1) already inside a Visual Studio developer prompt?
where cl >nul 2>nul && set "HAVE_CL=1"

rem 2) locate Visual Studio via vswhere, then call vcvars64.bat
if not defined HAVE_CL (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
    )
    if defined VSDIR if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"
)

if defined VCVARS (
    call "!VCVARS!" >nul
    set "HAVE_CL=1"
)

if defined HAVE_CL (
    echo [BUILD] MSVC: cl /std:c++17 /O2 /utf-8 ...
    cl /nologo /std:c++17 /O2 /EHsc /utf-8 /DNDEBUG /W3 xml2bin.cpp /Fo:xml2bin.obj /link /OUT:xml2bin.exe
    if errorlevel 1 ( echo [FAILED] compile error & exit /b 1 )
    if exist xml2bin.obj del /q xml2bin.obj
    echo [OK] xml2bin.exe generated
    exit /b 0
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe ^(MSVC C++ toolchain^) or g++ not found.
    echo         Install "Visual Studio Build Tools" with the C++ workload,
    echo         or install MinGW and add its bin folder to PATH.
    exit /b 1
)

echo [BUILD] g++: -O2 -std=c++17 ...
g++ -O2 -std=c++17 -o xml2bin.exe xml2bin.cpp
if errorlevel 1 ( echo [FAILED] compile error & exit /b 1 )
echo [OK] xml2bin.exe generated
exit /b 0
