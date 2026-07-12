@echo off
setlocal enabledelayedexpansion

:: Check if jansson directory already exists
if exist "%~dp0jansson-2.15.1" (
    echo jansson-2.15.1 directory already exists. Skipping download.
    goto build
)

echo Downloading jansson v2.15.1 source code...
curl -L -o v2.15.1.zip https://github.com/akheron/jansson/archive/refs/tags/v2.15.1.zip
if %errorlevel% neq 0 (
    echo Error: Failed to download jansson source code.
    exit /b 1
)

echo Extracting jansson source code...
powershell -Command "Expand-Archive -Path v2.15.1.zip -DestinationPath ."
if %errorlevel% neq 0 (
    echo Error: Failed to extract jansson.
    exit /b 1
)

del v2.15.1.zip

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

where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: cmake not found in PATH. Please install CMake and ensure it is on your PATH.
    exit /b 1
)

:: Build jansson
if exist "%~dp0jansson-2.15.1\build\lib\Release\jansson.lib" (
    echo jansson already compiled. Skipping compilation.
    goto done
)
if exist "%~dp0jansson-2.15.1\build\Release\jansson.lib" (
    echo jansson already compiled. Skipping compilation.
    goto done
)
if exist "%~dp0jansson-2.15.1\build\jansson.lib" (
    echo jansson already compiled. Skipping compilation.
    goto done
)

echo Compiling jansson...
cd /d "%~dp0jansson-2.15.1"
mkdir build 2>nul
cd build
:: If a previous build exists but was configured with the wrong CRT,
:: wipe the build outputs so JANSSON_STATIC_CRT=ON actually takes effect.
if exist CMakeCache.txt (
    findstr /C:"JANSSON_STATIC_CRT:BOOL=ON" CMakeCache.txt >nul
    if !errorlevel! neq 0 (
        echo Previous build used dynamic CRT; cleaning and reconfiguring...
        if exist CMakeFiles rmdir /s /q CMakeFiles
        if exist Release rmdir /s /q Release
        if exist lib rmdir /s /q lib
        if exist CMakeCache.txt del /f /q CMakeCache.txt
    )
)
cmake .. -G "Visual Studio 17 2022" -A x64 -DJANSSON_BUILD_SHARED_LIBS=OFF -DJANSSON_BUILD_DOCS=OFF -DJANSSON_EXAMPLES=OFF -DJANSSON_WITHOUT_TESTS=ON -DJANSSON_INSTALL=OFF -DJANSSON_STATIC_CRT=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if %errorlevel% neq 0 (
    :: Fall back to letting CMake pick the generator (e.g. older VS)
    cmake .. -DJANSSON_BUILD_SHARED_LIBS=OFF -DJANSSON_BUILD_DOCS=OFF -DJANSSON_EXAMPLES=OFF -DJANSSON_WITHOUT_TESTS=ON -DJANSSON_INSTALL=OFF -DJANSSON_STATIC_CRT=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    if !errorlevel! neq 0 (
        echo Error: CMake configuration failed.
        exit /b 1
    )
)

cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Error: Compiling jansson failed.
    exit /b 1
)

:done
cd /d "%~dp0"
echo jansson compiled successfully!
echo To compile GitBranchFS, run:
echo build.bat