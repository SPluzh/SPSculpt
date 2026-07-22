# Changelog

All notable changes to this project will be documented in this file.

## [1.1.0]
- **Tools**: Added interactive Mask Gradient Blur tool featuring screen-space dashed guides and interactive draggable handle controls.
- **Input**: Fixed camera controls sticking/freezing when using camera shortcuts (Alt + Left Click, Right Click, Middle Click) while the Mask Gradient Blur tool is active.
- **Tools**: Hides the circular brush cursor and center point entirely when the Visibility tool or Mask Gradient Blur tool is active.
- **Input**: Automatically switches the active brush to the Visibility tool when the `Ctrl + Shift` modifier combination is held.
- **Renderer**: Implemented custom lasso overlay colors to match the legacy JavaScript application: green (`#00E676`) for positive selection, red (`#FF3333`) for negative selection (`Ctrl + Shift + Alt`), cyan (`#00E5FF`) for mask lasso, and white (`#FFFFFF`) for negative mask lasso.
- **Renderer**: Added a transparent fill (15% opacity) and a stippled/dashed border to the selection and mask lasso loops.

## [1.0.0]
- **Performance**: Eliminated sculpting cursor latency by polling raw mouse positions directly before rendering and rendering the cursor dot in screen-space.
- **Input**: Added OS-level system mouse cursor hiding when actively sculpting to prevent distracting cursor duplication and improve tactile precision.
- **Performance**: Switched frame rate regulation from a fixed delay to vertical synchronization (VSync).
- **Input**: Added temporary masking brush activation (switches to Mask tool when holding down the `Ctrl` key and restores the previous brush when released), matching parity with the legacy version.
- **Tools**: Fixed masking brush and lasso selection tool behavior to dynamically toggle between brush-based masking (dragging on mesh) and lasso selection (dragging from empty space/background with Ctrl held), matching the legacy JavaScript workflow.
- **Tools**: Added support for masking lasso click actions (invert mask when clicking on background, blur/sharpen mask when clicking on mesh), matching parity with the legacy version.
- **Settings**: Added automatic saving and loading of all brush parameters per brush type to `brush_settings.cfg`.
- **UI**: Added a fully dynamic and interactive Sculpting Settings panel showing custom parameters for each tool, including sliders for spacing and hardness, and toggles for backface culling and topological constraints.
- **UI**: Added interactive Masking buttons (Clear, Invert, Blur, and Sharpen) to the masking tool settings panel.
- **Symmetry**: Exposed symmetry axis and toggle settings inside the Sculpting Settings panel.
- **Core**: Optimized remeshing memory consumption to resolve crashes (out of memory std::bad_alloc) at high grid resolutions (e.g. 900+). Switched to sparse voxel structures for colors/materials, bit-vector tracking, and dynamically-sized stacks.
- **UI**: Added a real-time Mesh Statistics & FPS HUD in the bottom-right corner of the viewport showing active points, total points, and a sliding-window frame rate counter, matching the legacy JavaScript version.
- **UI**: Added a toggle in the Panels menu to show/hide the Mesh Statistics & FPS HUD.
- **Performance**: Optimized sculpting responsiveness and GPU uploads to match WebGL/JavaScript speed.
- **Performance**: Switched GPU buffer uploads to incremental updates (`glBufferSubData`) using vertex range tracking instead of full buffer re-uploads.
- **Performance**: Implemented caching for sculpting area normal and center computations during brush strokes.
- **Performance**: Eliminated per-frame memory allocations by implementing epoch-based tagging for visited arrays and dirty faces tracking.
- **Performance**: Cached shader uniform locations in the renderer to eliminate driver overhead during rendering.
- **Performance**: Removed performance-blocking logging output from the hot sculpting rendering path.
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
