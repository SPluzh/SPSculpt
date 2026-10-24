@echo off
setlocal enabledelayedexpansion

pushd "%~dp0"

:: Read version from version.txt (or fallback to src\common\Version.h)
if not exist "version.txt" (
    if exist "src\common\Version.h" (
        for /f "tokens=3 delims=^ " %%A in ('findstr /C:"constexpr const char* STRING" src\common\Version.h') do (
            set "RAW_VER=%%~A"
            set "VERSION=!RAW_VER:;=!"
            set "VERSION=!VERSION:"=!"
        )
    ) else (
        echo [ERROR] version.txt not found!
        popd
        exit /b 1
    )
) else (
    set /p VERSION=<version.txt
)
set VERSION=%VERSION: =%

if "%VERSION%"=="" (
    echo [ERROR] Could not determine version!
    popd
    exit /b 1
)

echo ========================================================
echo        Packaging SPSculpt v%VERSION% (Win64 Release)
echo ========================================================

set "STAGING=dist_staging"
set "TARGET_DIR=%STAGING%\SPSculpt"
set "ZIP_NAME=SPSculpt_v%VERSION%_win64.zip"

:: Clean old staging and zip
if exist "%STAGING%" rd /s /q "%STAGING%"
if exist "%ZIP_NAME%" del /q "%ZIP_NAME%"

:: Create staging structure
mkdir "%TARGET_DIR%"

:: 1. Executable
if exist "build\SPSculpt.exe" (
    echo Copying SPSculpt.exe from build...
    copy /Y "build\SPSculpt.exe" "%TARGET_DIR%\" >nul
) else if exist "dist\SPSculpt.exe" (
    echo Copying SPSculpt.exe from dist...
    copy /Y "dist\SPSculpt.exe" "%TARGET_DIR%\" >nul
) else (
    echo [ERROR] SPSculpt.exe not found! Please build the project first.
    rd /s /q "%STAGING%"
    popd
    exit /b 1
)

:: Strip debug symbols if strip tool is available
set "STRIP_TOOL="
where strip >nul 2>&1 && set "STRIP_TOOL=strip"
if not defined STRIP_TOOL if exist "C:\msys64\ucrt64\bin\strip.exe" set "STRIP_TOOL=C:\msys64\ucrt64\bin\strip.exe"

if defined STRIP_TOOL (
    echo Stripping debug symbols from SPSculpt.exe...
    "%STRIP_TOOL%" --strip-all "%TARGET_DIR%\SPSculpt.exe" >nul 2>&1
)

:: 2. Copy required runtime DLLs
echo Copying dependencies (DLLs)...
if exist "dist\*.dll" (
    copy /Y "dist\*.dll" "%TARGET_DIR%\" >nul
)
if exist "build\*.dll" (
    copy /Y "build\*.dll" "%TARGET_DIR%\" >nul 2>&1
)

:: Ensure critical MinGW / UCRT64 runtime DLLs are present
set "UCRT_BIN=C:\msys64\ucrt64\bin"
if exist "%UCRT_BIN%" (
    for %%D in (SDL2.dll libEGL.dll libGLESv2.dll libwinpthread-1.dll zlib1.dll libgcc_s_seh-1.dll libstdc++-6.dll libgomp-1.dll libtbb12.dll) do (
        if not exist "%TARGET_DIR%\%%D" (
            if exist "%UCRT_BIN%\%%D" copy /Y "%UCRT_BIN%\%%D" "%TARGET_DIR%\" >nul
        )
    )
)

:: Remove placeholder text files if copied by mistake
if exist "%TARGET_DIR%\COMPILE AND ADD Clipper2_64.dll HERE" (
    del /f /q "%TARGET_DIR%\COMPILE AND ADD Clipper2_64.dll HERE" >nul 2>&1
)

:: 3. Copy resources
if exist "resources" (
    echo Copying resources...
    xcopy /E /I /Y "resources" "%TARGET_DIR%\resources" >nul
)

:: 4. Create ZIP using PowerShell
echo Creating ZIP archive: %ZIP_NAME%...
powershell -NoProfile -Command "Compress-Archive -Path '%STAGING%\*' -DestinationPath '%ZIP_NAME%' -Force"

:: 5. Finalize
if not exist "%ZIP_NAME%" (
    echo.
    echo [ERROR] Failed to create ZIP archive.
    if exist "%STAGING%" rd /s /q "%STAGING%"
    popd
    exit /b 1
)

echo.
echo ========================================================
echo [SUCCESS] Release package created: %ZIP_NAME%
for %%I in ("%ZIP_NAME%") do (
    set /a SIZE_MB=%%~zI / 1048576
    echo Size: %%~zI bytes ^(~!SIZE_MB! MB^)
)
echo ========================================================
if exist "%STAGING%" rd /s /q "%STAGING%"

pause
popd
