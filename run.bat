@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
cd build
SPSculpt.exe --console %*
cd ..
pause