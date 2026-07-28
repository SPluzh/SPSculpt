@echo off
echo ========================================================
echo        Building Production Release (Optimized)
echo ========================================================

set PATH=C:\msys64\ucrt64\bin;%PATH%

if not exist build mkdir build
cd build

cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto error

cmake --build . --config Release
if errorlevel 1 goto error

cd ..

echo.
echo [1/3] Stripping debug symbols from executable...
strip --strip-all build\SPSculpt.exe

if not exist dist mkdir dist

echo [2/3] Copying executable and DLLs to dist...
copy /Y build\SPSculpt.exe dist\ >nul
copy /Y C:\msys64\ucrt64\bin\SDL2.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\libEGL.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\libGLESv2.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\libwinpthread-1.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\zlib1.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\libgcc_s_seh-1.dll dist\ >nul
copy /Y C:\msys64\ucrt64\bin\libstdc++-6.dll dist\ >nul

echo [3/3] Copying assets to dist...
if exist ZBrushes (
    xcopy /E /I /Y ZBrushes dist\ZBrushes >nul
)

echo.
echo ========================================================
echo SUCCESS: Production build complete!
echo Output folder: dist\
echo Final EXE Size:
for %%I in (dist\SPSculpt.exe) do echo %%~zI bytes (%%~zI / 1024 / 1024 MB)
echo ========================================================
goto end

:error
echo.
echo BUILD ERROR: Production build failed!
exit /b 1

:end
