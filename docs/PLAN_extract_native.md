# Migration Plan: Extract Native C++ standalone desktop application from WASM-dependent repository (Completed)

## Current Status: DONE ✅

The native part of the SculptSP project has been fully copied and migrated into an independent project under `c:\Users\user\Desktop\cpp\sculptsp-native\`.

## 1. Directory Structure Created
```
sculptsp-native/
├── CMakeLists.txt        - Complete native CMake configuration
├── build.bat             - Windows Ninja-based build script using MSYS2 UCRT64
├── run.bat               - Windows launch script prepending MSYS2 DLL paths
├── README.md             - Setup and usage documentation
├── .gitignore            - Configured for builds and VS files
└── src/
    ├── common/
    │   └── Enums.h       - Centralized brush and shader enums
    ├── scene/
    │   ├── Camera.h / .cpp
    │   └── Scene.h / .cpp
    ├── mesh/
    │   ├── Mesh.h / .cpp
    │   ├── Octree.h / .cpp
    │   ├── NormalCalc.h / .cpp
    │   └── Topology.h / .cpp
    ├── sculpt/
    │   ├── SculptEngine.h / .cpp
    │   └── Remesh.h / .cpp
    ├── render/
    │   ├── AngleRenderer.h / .cpp
    │   └── ReferenceImage.h / .cpp
    ├── editing/
    │   ├── SculptManager.h / .cpp
    │   ├── CameraController.h / .cpp
    │   └── BrushCursor.h / .cpp
    ├── gui/
    │   └── GuiManager.h / .cpp
    └── platform/
        ├── HotkeyDispatcher.h / .cpp
        └── NativeMain.cpp
```

## 2. Refactoring Actions Completed
1. **Renamed Files & Classes**: Stripped `Cpp` suffix from files and class names (e.g. `CameraCpp` -> `Camera`, `SceneCpp` -> `Scene`, `MeshCpp` -> `Mesh`) for a clean, professional C++ API.
2. **Normalized `#include` Directives**: All headers now refer to relative paths from `src/` (e.g., `#include "scene/Scene.h"`), allowing smooth compilation.
3. **Removed WASM Residue**: 
   - Cleaned `Octree.h` and `Octree.cpp` of all Emscripten/WASM bindings.
   - Refactored `Mesh` and `Octree` methods to accept and return standard native pointers (`const float*`, `const uint32_t*`) instead of legacy WASM-binders (`uintptr_t` or `emscripten::val`).
   - Cleaned local definitions of `BrushType` from `SculptManager.h` to use `common/Enums.h`.

## 3. Native Desktop Compilation Verified
- Build environment was located in `C:\msys64\ucrt64\bin`.
- Compiles smoothly via `CMake` with `Ninja` generator and `GNU 15.2.0 C++20`.
- Verified compilation and linking: successfully produced `sculptsp.exe` without errors.

## 4. How to Build & Run
- Run `.\build.bat` to build the standalone desktop application using Ninja.
- Run `.\run.bat` to run the application (automatically resolves all required system/MSYS2 DLLs).
