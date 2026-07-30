@echo off
if not exist dist mkdir dist
copy /Y build\SPSculpt.exe dist\
copy /Y C:\msys64\ucrt64\bin\SDL2.dll dist\
copy /Y C:\msys64\ucrt64\bin\libEGL.dll dist\
copy /Y C:\msys64\ucrt64\bin\libGLESv2.dll dist\
copy /Y C:\msys64\ucrt64\bin\libwinpthread-1.dll dist\
copy /Y C:\msys64\ucrt64\bin\zlib1.dll dist\
copy /Y C:\msys64\ucrt64\bin\libgcc_s_seh-1.dll dist\
copy /Y C:\msys64\ucrt64\bin\libstdc++-6.dll dist\
if exist resources xcopy /E /I /Y resources dist\resources >nul
echo Package created in 'dist' folder!

