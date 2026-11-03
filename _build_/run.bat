@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
cd /d "%~dp0"
if exist dist\SPSculpt.exe (
    cd dist
    SPSculpt.exe --console %*
    cd ..
) else if exist build\SPSculpt.exe (
    cd build
    SPSculpt.exe --console %*
    cd ..
) else (
    echo [ERROR] SPSculpt.exe not found in dist or build. Please build the project first.
)