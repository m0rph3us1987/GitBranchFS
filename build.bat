@echo off
setlocal enabledelayedexpansion

:: Default paths
set WINFSP_DIR=C:\Program Files (x86)\WinFsp
set LIBGIT2_DIR=
set JANSSON_DIR=
set JANSSON_AUTOBUILD=1
set JANSSON_BUILT_DIR=%~dp0jansson-2.15.1
set JANSSON_BUILT_BUILD=%JANSSON_BUILT_DIR%\build
set JANSSON_AUTOBUILD_SCRIPT=%~dp0get_jansson.bat
if exist "%~dp0libgit2-1.7.2" (
    set "LIBGIT2_DIR=%~dp0libgit2-1.7.2"
) else if exist "%~dp0vcpkg_installed\x64-windows" (
    set "LIBGIT2_DIR=%~dp0vcpkg_installed\x64-windows"
    set "JANSSON_DIR=%~dp0vcpkg_installed\x64-windows"
)

:: Parse arguments
:parse
if "%~1"=="\n" goto end_parse
if "%~1"=="" goto end_parse
if /i "%~1"=="--libgit2" (
    set LIBGIT2_DIR=%~2
    shift
    shift
    goto parse
)
if /i "%~1"=="--winfsp" (
    set WINFSP_DIR=%~2
    shift
    shift
    goto parse
)
if /i "%~1"=="--jansson" (
    set JANSSON_DIR=%~2
    shift
    shift
    goto parse
)
if /i "%~1"=="--no-jansson-autobuild" (
    set JANSSON_AUTOBUILD=0
    shift
    goto parse
)
echo Unknown option: %1
exit /b 1
:end_parse

:: Check WinFSP
if exist "%WINFSP_DIR%" goto winfsp_ok
echo Error: WinFSP not found at "%WINFSP_DIR%".
echo Please install WinFSP or specify its location with --winfsp.
exit /b 1
:winfsp_ok

:: Check libgit2
if "%LIBGIT2_DIR%"=="" (
    echo Warning: LIBGIT2_DIR is not specified.
    echo Please specify it with --libgit2 "C:\path\to\libgit2" if compilation fails.
)

:: Find MSVC compiler
where cl.exe >nul 2>nul
if %errorlevel% equ 0 goto msvc_ok

echo cl.exe not in PATH, searching for Visual Studio...
set "VSWHERE_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE_PATH%" set "VSWHERE_PATH=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE_PATH%" goto vswhere_ok
echo Error: Visual Studio Installer (vswhere.exe) not found.
echo Please run this script from a Visual Studio Developer Command Prompt.
exit /b 1

:vswhere_ok
for /f "usebackq tokens=*" %%i in (`"%VSWHERE_PATH%" -latest -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not "%VS_PATH%"=="" goto vs_found
echo Error: No Visual Studio installation found.
exit /b 1

:vs_found
set "VCVARS_PATH=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS_PATH%" goto vcvars_ok
echo Error: vcvars64.bat not found at "%VCVARS_PATH%".
exit /b 1

:vcvars_ok
echo Initializing MSVC x64 build environment...
call "%VCVARS_PATH%"
if %errorlevel% equ 0 goto msvc_ok
echo Error: Failed to initialize MSVC build environment.
exit /b 1

:msvc_ok

:: Resolve jansson: explicit --jansson > already-built local tree > vcpkg-installed > auto-fetch
if "%JANSSON_DIR%"=="" goto maybe_jansson_built
echo Using jansson from "%JANSSON_DIR%".
goto jansson_resolved

:maybe_jansson_built
if exist "%JANSSON_BUILT_DIR%" (
    echo Using locally-built jansson at "%JANSSON_BUILT_DIR%".
    set "JANSSON_DIR=%JANSSON_BUILT_DIR%"
    goto jansson_resolved
)

if "%JANSSON_AUTOBUILD%"=="0" goto jansson_autobuild_skip
if exist "%JANSSON_AUTOBUILD_SCRIPT%" (
    echo jansson not found locally. Running get_jansson.bat to download and build it...
    call "%JANSSON_AUTOBUILD_SCRIPT%"
    if !errorlevel! neq 0 (
        echo Error: get_jansson.bat failed.
        exit /b 1
    )
    if exist "%JANSSON_BUILT_DIR%" (
        set "JANSSON_DIR=%JANSSON_BUILT_DIR%"
        goto jansson_resolved
    )
    echo Error: get_jansson.bat did not produce "%JANSSON_BUILT_DIR%".
    exit /b 1
) else (
    echo Error: "%JANSSON_AUTOBUILD_SCRIPT%" not found.
    echo Please download jansson manually and pass --jansson "C:\path\to\jansson".
    exit /b 1
)
:jansson_autobuild_skip
echo Warning: jansson not configured and autobuild disabled. Build may fail to link.

:jansson_resolved

:: Prepare build paths
set INCLUDES=-Iinclude -I"%WINFSP_DIR%\inc"
if "%LIBGIT2_DIR%"=="" goto no_libgit2_inc
set INCLUDES=%INCLUDES% -I"%LIBGIT2_DIR%\include"
:no_libgit2_inc
if "%JANSSON_DIR%"=="" goto no_jansson_inc
:: If using the locally-built tree (built by get_jansson.bat via CMake), we
:: need both the public src/ headers and the generated build\include\ config header.
if /i "%JANSSON_DIR%"=="%JANSSON_BUILT_DIR%" (
    set INCLUDES=%INCLUDES% -I"%JANSSON_BUILT_DIR%\src" -I"%JANSSON_BUILT_BUILD%\include"
    set DEFINES=/DJANSSON_USING_CMAKE
) else (
    set INCLUDES=%INCLUDES% -I"%JANSSON_DIR%\include"
)
:no_jansson_inc

set LIBS="%WINFSP_DIR%\lib\winfsp-x64.lib" winhttp.lib rpcrt4.lib crypt32.lib secur32.lib ws2_32.lib advapi32.lib ole32.lib
if "%LIBGIT2_DIR%"=="" goto no_libgit2_lib
if exist "%LIBGIT2_DIR%\build\src\libgit2\Release\git2.lib" (
    set LIBS=%LIBS% "%LIBGIT2_DIR%\build\src\libgit2\Release\git2.lib"
    goto maybe_jansson_lib
)
if exist "%LIBGIT2_DIR%\build\Release\git2.lib" (
    set LIBS=%LIBS% "%LIBGIT2_DIR%\build\Release\git2.lib"
    goto maybe_jansson_lib
)
if exist "%LIBGIT2_DIR%\lib\git2.lib" (
    set LIBS=%LIBS% "%LIBGIT2_DIR%\lib\git2.lib"
    goto maybe_jansson_lib
)
set LIBS=%LIBS% "%LIBGIT2_DIR%\git2.lib"
goto maybe_jansson_lib
:no_libgit2_lib
set LIBS=%LIBS% git2.lib

:maybe_jansson_lib
if "%JANSSON_DIR%"=="" goto no_jansson_lib
if /i "%JANSSON_DIR%"=="%JANSSON_BUILT_DIR%" (
    if exist "%JANSSON_BUILT_BUILD%\lib\Release\jansson.lib" (
        set LIBS=%LIBS% "%JANSSON_BUILT_BUILD%\lib\Release\jansson.lib"
        goto compile
    )
    if exist "%JANSSON_BUILT_BUILD%\Release\jansson.lib" (
        set LIBS=%LIBS% "%JANSSON_BUILT_BUILD%\Release\jansson.lib"
        goto compile
    )
    if exist "%JANSSON_BUILT_BUILD%\jansson.lib" (
        set LIBS=%LIBS% "%JANSSON_BUILT_BUILD%\jansson.lib"
        goto compile
    )
    echo Error: jansson.lib not found under "%JANSSON_BUILT_BUILD%".
    echo Searched for:
    echo   "%JANSSON_BUILT_BUILD%\lib\Release\jansson.lib"   (Visual Studio generator)
    echo   "%JANSSON_BUILT_BUILD%\Release\jansson.lib"       (alternative multi-config)
    echo   "%JANSSON_BUILT_BUILD%\jansson.lib"               (single-config generator)
    echo Run get_jansson.bat to build it.
    exit /b 1
)
if exist "%JANSSON_DIR%\lib\jansson.lib" (
    set LIBS=%LIBS% "%JANSSON_DIR%\lib\jansson.lib"
    goto compile
)
if exist "%JANSSON_DIR%\bin\jansson.lib" (
    set LIBS=%LIBS% "%JANSSON_DIR%\bin\jansson.lib"
    goto compile
)
:no_jansson_lib
echo Warning: JANSSON_DIR not found. Build will fail unless jansson is on the LIB search path.

:compile
:: Compile
echo Compiling GitBranchFS...
cl.exe /nologo /O2 /MT /W3 /std:c11 /D_FILE_OFFSET_BITS=64 %DEFINES% src\*.c /Fe:gbfs.exe /Iinclude %INCLUDES% /link /LTCG %LIBS%

if %errorlevel% equ 0 goto build_ok
echo Build Failed!
exit /b 1

:build_ok
echo Build Succeeded: gbfs.exe
if "%LIBGIT2_DIR%"=="" goto no_git2_dll_copy
if exist "%LIBGIT2_DIR%\build\Release\git2.dll" (
    copy /y "%LIBGIT2_DIR%\build\Release\git2.dll" "%~dp0" >nul
    echo Copied git2.dll next to gbfs.exe
    goto no_git2_dll_copy
)
if exist "%LIBGIT2_DIR%\bin\git2.dll" (
    copy /y "%LIBGIT2_DIR%\bin\git2.dll" "%~dp0" >nul
    echo Copied git2.dll next to gbfs.exe
    goto no_git2_dll_copy
)
if exist "%LIBGIT2_DIR%\git2.dll" (
    copy /y "%LIBGIT2_DIR%\git2.dll" "%~dp0" >nul
    echo Copied git2.dll next to gbfs.exe
)
:no_git2_dll_copy

for /r "%WINFSP_DIR%" %%f in (winfsp-x64.dll) do (
    if exist "%%f" (
        copy /y "%%f" "%~dp0" >nul
        echo Copied winfsp-x64.dll next to gbfs.exe
        goto winfsp_dll_done
    )
)
:winfsp_dll_done
