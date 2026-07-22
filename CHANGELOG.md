# Changelog

All notable changes to this project will be documented in this file.

## [1.0.0]
- **Renderer**: Fixed a flat shading normal orientation issue by correcting cross-product derivative signs in the `getNormal` GLSL helper, preventing flipped shadows/shading on desktop OpenGL.
- **Renderer**: Fully migrated the Matcap and environment library, enabling local loading and initialization of all 9 matcap textures and environmental maps.
- **UI**: Added interactive selectors for Matcap and Environment presets under the Material Shader section of the Rendering Quality panel.
- **UI**: Exposed metallic and roughness sliders when using the PBR shader.
- **Migration**: Extracted native C++ sculpting engine into an independent standalone desktop application.
- **UI**: Integrated ImGui interface panels natively with SDL2 and OpenGL ES 3.0 (via ANGLE).
- **Core**: Removed WebAssembly (WASM) and Emscripten dependencies from the C++ codebase.
- **Build System**: Implemented automated CMake and Ninja build configuration.
- **Renderer**: Fixed a critical graphics bug where meshes, backgrounds, and the grid failed to render, leaving only the outline of the model visible.
