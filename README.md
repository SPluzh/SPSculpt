# SPSculpt Desktop Native

Standalone C++ desktop sculpt application using SDL2, OpenGL ES 3.0 (via ANGLE), and ImGui.

## Prerequisites

- **CMake** 3.20+
- **Compiler**: GCC / MinGW (MSYS2) or MSVC with C++20 support
- **SDL2** development libraries
- **ANGLE** (EGL/GLESv2) development libraries

## How to Build

Run `build.bat` on Windows or:

```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
cmake --build . --config Release
```

## How to Run

Run `run.bat` on Windows or:

```bash
cd build
./SPSculpt
```
