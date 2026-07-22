# Changelog

All notable changes to this project will be documented in this file.

## [1.0.0]
- **Files**: Added native C++ support for importing and exporting all major 3D file formats, including SGL (native scene format), OBJ, STL, PLY, and GLTF/GLB.
- **Files**: Integrated JSON parsing library for native GLTF file loading and scene structure traversal.
- **UI**: Added a new "Import & Export" panel to manage loading/saving models, alongside new options in the main File menu.
- **Core**: Refactored the core sculpting engine and manager to enforce native C++ pointer type-safety, completely eliminating unsafe typecasting and legacy pointer formats.
- **Tools**: Added full native support for the remaining Group B brushes, including Masking, Painting, Twist, and Local Scale tools, and integrated them into the user interface and hotkey systems.
- **Input**: Implemented hardware tablet input support on Windows, allowing automatic detection and pressure-sensitive brush scaling for pens and styluses with seamless fallback to normal mouse input.
- **UI**: Added a "Show Selection Outline" checkbox (along with an outline color picker) to toggle the selected object's outline contour in the "Rendering Quality" panel.
- **Renderer**: Added automatic and manual saving and loading of all render and shading parameters to/from a local configuration file (`render_settings.cfg`).
- **UI**: Added manual Save/Load Profile buttons to the "Rendering Quality" panel and main "File" menu.
- **Renderer**: Fixed a flat shading normal orientation issue by correcting cross-product derivative signs in the `getNormal` GLSL helper, preventing flipped shadows/shading on desktop OpenGL.
- **Renderer**: Fully migrated the Matcap and environment library, enabling local loading and initialization of all 9 matcap textures and environmental maps.
- **UI**: Added interactive selectors for Matcap and Environment presets under the Material Shader section of the Rendering Quality panel.
- **UI**: Exposed metallic and roughness sliders when using the PBR shader.
- **Migration**: Extracted native C++ sculpting engine into an independent standalone desktop application.
- **UI**: Integrated ImGui interface panels natively with SDL2 and OpenGL ES 3.0 (via ANGLE).
- **Core**: Removed WebAssembly (WASM) and Emscripten dependencies from the C++ codebase.
- **Build System**: Implemented automated CMake and Ninja build configuration.
- **Renderer**: Fixed a critical graphics bug where meshes, backgrounds, and the grid failed to render, leaving only the outline of the model visible.
