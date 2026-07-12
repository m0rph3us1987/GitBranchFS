@echo off
setlocal enabledelayedexpansion

:: Check if libgit2 directory already exists
if exist "%~dp0libgit2-1.7.2" (
    echo libgit2-1.7.2 directory already exists. Skipping download.
    goto build
)

echo Downloading libgit2 v1.7.2 source code...
curl -LO https://github.com/libgit2/libgit2/archive/refs/tags/v1.7.2.zip
if %errorlevel% neq 0 (
    echo Error: Failed to download libgit2 source code.
    exit /b 1
)

echo Extracting libgit2 source code...
powershell -Command "Expand-Archive -Path v1.7.2.zip -DestinationPath ."
if %errorlevel% neq 0 (
    echo Error: Failed to extract libgit2.
    exit /b 1
)

del v1.7.2.zip

:build
:: Find MSVC compiler and CMake
where cl.exe >nul 2>nul
if %errorlevel% neq 0 (
    echo cl.exe not in PATH, searching for Visual Studio...
    set "VSWHERE_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE_PATH!" set "VSWHERE_PATH=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    
    if not exist "!VSWHERE_PATH!" (
        echo Error: Visual Studio Installer not found.
        exit /b 1
    )
    
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE_PATH!" -latest -property installationPath`) do (
        set "VS_PATH=%%i"
    )
    
    if "!VS_PATH!"=="" (
        echo Error: No Visual Studio installation found.
        exit /b 1
    )
    
    set "VCVARS_PATH=!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS_PATH!" (
        echo Error: vcvars64.bat not found at "!VCVARS_PATH!".
        exit /b 1
    )
    
    echo Initializing MSVC x64 build environment...
    call "!VCVARS_PATH!" || (
        echo Error: Failed to initialize MSVC build environment.
        exit /b 1
    )
)

:: Build libgit2
if exist "%~dp0libgit2-1.7.2\build\src\libgit2\Release\git2.lib" (
    echo libgit2 already compiled. Skipping compilation.
    goto done
)
if exist "%~dp0libgit2-1.7.2\build\Release\git2.lib" (
    echo libgit2 already compiled. Skipping compilation.
    goto done
)

echo Compiling libgit2...
cd "%~dp0libgit2-1.7.2"
mkdir build 2>nul
cd build
if exist CMakeCache.txt del /f /q CMakeCache.txt
cmake .. -DREGEX_BACKEND=builtin -DUSE_SSH=OFF -DUSE_HTTPS=WinHTTP -DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if %errorlevel% neq 0 (
    echo Error: CMake configuration failed.
    exit /b 1
)

cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Error: Compiling libgit2 failed.
    exit /b 1
)

:done
echo libgit2 compiled successfully!
echo To compile GitBranchFS, run:
echo build.bat
