@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
if not exist build mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build .
cd ..
if exist build\SPSculpt.exe (
    if not exist dist mkdir dist
    copy /Y build\SPSculpt.exe dist\ >nul
    if exist resources xcopy /E /I /Y resources dist\resources >nul
)

