@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
if not exist build mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build .
cd ..
