# Changelog

All notable changes to this project will be documented in this file.

## [1.2.3]
- **UI**: Added support for 2D View Mode, enabling viewport and reference images to be panned and zoomed in 2D space. Panning is performed by dragging the Right Mouse button while holding Alt, and zooming by dragging while holding Ctrl. Double right-clicking resets the 2D view offset and zoom. Toggles for 2D Pan/Zoom Mode and Reference Drag, along with a Reset button, have been added to the Reference Images panel. When 2D mode is active, standard 3D camera navigation (such as right-click dragging and mouse wheel zoom) is automatically blocked.
- **UI**: Increased the size of the Navigation Cube window, increased the cube's render scale, and enlarged the text labels on the cube faces for improved readability. Aligned, capitalized, and projected the label text directly onto the 3D planes of the cube faces so they appear natively written on the faces in uppercase rather than billboarding toward the camera.
- **UI**: Normalized camera snap and view transition logic for all 26 gizmo interaction parts (faces, edges, and corners), ensuring camera snaps always follow the shortest angular path without erratic spinning or rotational jumps.
- **UI**: Added double-click prevention on the Snap Cube (navigation cube/gizmo) faces, preventing accidental view changes when double-clicking. Both JavaScript and C++ implementations now delay the view change by 250ms and cancel the snap transition entirely if a double-click is registered on the same face.
- **UI**: Refined Snap Cube (navigation gizmo) text labels to be rendered directly onto the cube faces, aligned to the 3D plane of the faces. Changed labels to mixed-case, scaled down the font, rotated the text vertices to match the face's orientation on screen, and added face-angle thresholds so labels are only shown when a face is mostly facing the camera. Prevented redundant camera snaps and view transitions when repeatedly clicking on Snap Cube faces, including during active transitions, by comparing clicks to target camera orientations.
- **UI**: Frozen brush cursor coordinates while modal hotkey adjustments are active, keeping the visual cursor circle projected stably on the mesh surface even as the mouse moves.
- **UI**: Added a sleek, floating HUD popup card that displays real-time value and progress information directly under the cursor when adjusting brush settings (like Intensity, Focal Shift, and Radius) or scene parameters via keyboard hotkeys.
- **Geometry**: Added a Geosphere (quad-sphere / cubed-sphere) primitive generator (constructed by subdividing a cube, projecting its vertices to a sphere of radius R, and merging boundary vertices for watertight topology) with quad polygons. Exposed it under both default scene loading and "At Masked BBox" spawning mechanisms.
- **Fix**: Corrected the face winding order of the UV Sphere primitive (from clockwise to counter-clockwise), resolving the issue where sphere normals were inverted relative to other primitives.
- **Input**: Implemented Alt + Left Click on any viewport object to select and make it the active mesh in the scene, facilitating faster multi-object scene navigation.
- **Renderer**: Globalized all material settings (`albedo`, `roughness`, `metallic`, `alpha`, and texture maps) across all scene meshes, moving them from individual mesh properties to central renderer fields to maintain consistency when creating/rendering objects.
- **Settings**: Updated `render_settings.cfg` serialization to save and load global material attributes under the `[Renderer]` section, with legacy compatibility fallbacks to restore older settings from `Mesh_0` when reading legacy configurations.
- **UI**: Updated the "Rendering Quality" panel to directly display and modify the global material settings on the `AngleRenderer`, ensuring all meshes in the scene share a uniform material representation.
- **Files**: Restructured SGL file import and export routines (`ImportSGL` / `ExportSGL`) to use global alpha values rather than storing opacity per-mesh.
- **Renderer**: Globalized shading settings (`shaderType`, `matcapIdx`, `flatShading`, `showWireframe`, `curvature`) across all scene meshes, moving them from individual mesh properties to the central renderer.
- **Settings**: Refactored the serialization of shading settings in `render_settings.cfg` to save them globally under the `[Renderer]` section, with backward-compatibility fallbacks to restore older mesh-specific settings.
- **Settings**: Updated SGL scene importer and exporter to read/write shading settings globally instead of per-mesh.
- **UI**: Reworked the Rendering Quality panel to separate global scene shading controls from active object material parameters (albedo, roughness, metallic, transparency, and textures).
- **Settings**: Added persistence for camera movement speeds (rotation, panning, zoom, and roll), ensuring they are automatically saved to `render_settings.cfg` on exit and restored on startup.
- **UI**: Added automatic saving and loading of GUI panel visibility states (toolbar, sculpting settings, scene outliner, etc.) to a local configuration file `gui_settings.cfg` on startup and shutdown.
- **UI**: Added "Save GUI Settings" and "Load GUI Settings" options to the main File menu to allow manual GUI state persistence.
- **Fix**: Resolved viewport visibility toggles (V1/V2) not correctly updating mesh rendering by routing the viewport index to the geometry drawing pass and filtering out hidden meshes.
- **Fix**: Prevented brush cursor projection and sculpting raycasts on hidden meshes in the active viewport.
- **UI**: Added a dedicated "Act" checkbox column to indicate and control the active mesh in the Scene Outliner, removing full-row selection highlighting.
- **UI**: Redesigned the Scene Outliner panel with a comprehensive, professional table layout displaying each mesh's name, vertex count, and dual-viewport visibility toggles (V1 and V2) with interactive checkboxes.
- **UI**: Added interactive inline renaming for meshes directly in the Scene Outliner by double-clicking on any mesh name.
- **UI**: Added range selection support (holding Shift) and toggle selection support (holding Ctrl) to select multiple meshes simultaneously in the Outliner.
- **UI**: Integrated quick-action buttons in the Scene Outliner to duplicate selection, merge multiple selected meshes into a single object, delete selected meshes, or clear the entire scene.
- **UI**: Added primitive spawning options to create Spheres, Cubes, Cylinders, and Toruses with toggles to place them at the masked bounding box coordinates of the active mesh or spawn them symmetrically across the selected mirror axis.
- **UI**: Exposes dedicated selection buttons for the Measure and Divider tools in the Outliner, along with sliders and check-boxes for active tool parameters and a button to clear all measurements.

## [1.2.2]
- **UI**: Added a Curvature control slider, Wet Clay rendering sliders (Wetness, Bump Strength, Noise Scale, Subsurface Scattering Intensity and Color), and a Filmic Tonemapping checkbox to the Rendering Quality panel.
- **UI**: Added path inputs and buttons to import custom UV textures and custom Matcap images from local files in the Rendering Quality panel.
- **Renderer**: Dynamic rendering parameters (wetness, bump strength, noise scale, subsurface scattering intensity/color) are now correctly updated in the wet clay shader.
- **Settings**: Serialized the new wet clay rendering options to `render_settings.cfg` for persistence across sessions.
- **Camera**: Ported the legacy smooth camera transitions and zoom-to-focus behavior. Pressing the 'F' key to focus on a selected object, resetting the view, or switching between orthographic angles now smoothly animates the camera transition using quartic easing instead of instantly snapping. Focusing on the object preserves the current camera rotation (view angle) and calculates the correct zoom fit factor to fit the mesh bounds perfectly within the viewport, matching the legacy application's behavior.
- **Input**: Configured camera animations to cancel instantly if the user initiates manual navigation (orbiting, panning, zooming) during a transition, ensuring seamless and responsive control.
- **Brushes**: Fixed Move, Drag, and Elastic brush behavior to match the legacy JavaScript project:
  - Vertices are now deformed relative to their initial (proxy) positions at the start of the stroke, rather than accumulating displacement cumulatively across stroke frames.
  - Implemented the `vertexOnLine` mouse ray projection algorithm in C++ to compute drag directions stably relative to the initial intersection coordinates instead of relying on shifting dynamic raycast hits.
  - Locked the grabbed vertex group (`pickedVertices`) on the first frame of the stroke for these grab-based brushes, preventing new vertices from being picked and old ones dropped as the mouse moves.
  - Scaled the calculated drag direction by the brush's `intensity` parameter.
  - Properly initialized and mirrored the start coordinates for the symmetry pass (`m_initialSymIntersection`), ensuring symmetrical strokes work correctly.

## [1.2.1]
- **UI**: Fixed a bug where the inner ring of the brush cursor did not scale dynamically when the "Focal Shift" parameter was adjusted.
- **Brushes**: Fixed a bug where the clay and flatten brushes did not update their plane normals and centers dynamically per-frame, causing them to flatten geometry incorrectly relative to the starting point of the stroke.
- **Brushes**: Implemented support for loading, managing, and applying 28 custom ZBrush brush presets directly from JSON files.
- **UI**: Added a "ZBrush Brush Presets" collapsing header to the Sculpting Settings panel, featuring a preset selection dropdown, real-time read-only details of the active preset, and a button to load custom preset JSON files.
- **Core**: Integrated a parallel brush settings track that dynamically maps ZBrush preset deformation and stroke modes to native brush tools at runtime.
- **Viewport**: Fixed a bug where the brush cursor would render across the entire screen in the right viewport during split viewport modes, ensuring correct camera mapping and border-aligned clipping are applied to both viewports.
- **UI**: Added a "Show cursor in inactive viewport" toggle checkbox under the Camera Settings panel when split viewport mode is active.
- **Settings**: Serialized the "Show cursor in inactive viewport" setting to `render_settings.cfg` for cross-session persistence.
- **Viewport**: Fixed the split viewport brush cursor rendering so that the cursor displays correctly in the active viewport (left or right).
- **Viewport**: Implemented projection support for drawing the brush cursor in the inactive viewport when "Show cursor in inactive viewport" is enabled, fully compatible with both the smooth vector cursor and standard shader cursor.
- **UI**: Added a "Split Viewport" setting ("Off", "Mirror", "Independent") with interactive radio buttons in the Camera Settings panel.
- **Viewport**: Implemented scissor-based multi-viewport rendering, allowing the main viewport to be split into two separate screens.
- **Camera**: Added secondary camera support with automatic synchronization and alignment to the orthographic right view upon entering independent split-screen mode.
- **Input**: Added automatic active viewport detection based on mouse cursor position, routing navigation, sculpting brush strokes, and measuring/divider tools to the correct viewport camera with local coordinate space translation.
- **Settings**: Serialized the split viewport mode setting to `render_settings.cfg` for cross-session persistence.
- **Camera**: Ported the legacy "Plane Trackball" and "Spherical Trackball" camera rotation methods from JavaScript to C++.
- **Camera**: Implemented camera "Roll" functionality (rotating the camera around the view direction Z-axis), triggered by the Shift + Alt key modifier combination during right-click or Alt + left-click viewport drags. Rolling the view with "Picking pivot" enabled correctly performs a raycast check on mouse-down to center the roll rotation directly around the mesh intersection point.
- **UI**: Added a "Camera Mode" combo box select and a "Roll Speed" slider under the Camera Settings panel to configure and toggle between Orbit, Plane, and Spherical camera modes.
- **Camera**: Ported the legacy "Picking pivot" camera rotation feature. When starting a viewport rotate/orbit drag with "Picking pivot" enabled, the camera performs a raycast intersection check against scene meshes and centers its rotation pivot directly on the surface intersection point.
- **UI**: Renamed the "Use Pivot" camera setting checkbox to "Picking pivot" to align with the legacy JavaScript design.
- **UI**: Implemented a visual pivot point indicator at the camera's pivot coordinates (red ring, center dot, and crosshair ticks) using the ImGui foreground draw list. The marker is gated by the "Picking pivot" state, is visible only when the camera is actively orbiting or rolling, and is automatically hidden when occluded by active ImGui panels or when split-screen viewports require offset projection.
- **Symmetry**: Implemented highly-optimized CPU-based raycasting check using the mesh's octree traversal and Möller-Trumbore ray-triangle intersections. Symmetry cursor dots are dynamically dimmed/darkened to 0.3x opacity when hidden behind the mesh geometry, fully supporting both the vector-based Smooth Cursor (drawn in ImGui) and the standard OpenGL shader cursor.
- **Symmetry**: Extracted brush logic switch into `doStrokePass` and implemented double-pass brush execution (primary coordinate and mirrored coordinate across the selected axis) inside `executeStroke`, resolving issues with broken symmetry for all brush types (including drag-based brushes).
- **Symmetry**: Modified stroke frame throttling to cache the last valid raycast intersection, preventing the sculpting cursor from flickering and snapping back to screen-space coordinates during active strokes.

## [1.2.0]
- **UI**: Added "Use Pressure for Size" and "Use Pressure for Cursor Dot" toggles to both the Sculpting Settings and Tablet Diagnostics panels to dynamically scale the brush size and the cursor dot based on stylus pressure.
- **Settings**: Serialized tablet pressure, pressure-size, pressure-cursor, and tilt settings to the local configuration file for persistence across sessions.
- **Input**: Finalized high-precision tablet support by integrating thread-safe polling for both WinTab and Windows Ink APIs, retrieving real-time stylus pressure and tilt data engine-wide.
- **Renderer**: Implemented interactive brush cursor deformation that dynamically squeezes/flattens the cursor circle along the tool's local tangent axes when the pen is tilted.
- **UI**: Added a "Tablet Diagnostics" panel under the main toolbar to monitor connection health, active input mode (WinTab/WinInk/Auto), and raw packets in real-time, complete with a pressure-sensitive test canvas.
- **Core**: Integrated `src/platform/TabletInput.cpp` into `CMakeLists.txt` and initialized/closed the Wintab context cleanly during application lifecycle.
- **Renderer**: Added a smooth, vector-based rendering option for the brush cursor using screen-space projection, providing perfect anti-aliasing and subpixel precision. The cursor is automatically hidden when the mouse hovers over menu bars and settings panels.
- **UI**: Added a "Smooth (Antialiased) Cursor" toggle option under Shading & Rendering settings to switch between the new smooth vector cursor and the legacy hardware shader cursor.
- **Settings**: Serialized the smooth cursor toggle and cursor line thickness settings to the local configuration file for persistence across sessions.
- **Performance**: Optimized floodFill by replacing `std::vector<bool>` with `std::vector<uint8_t>` to avoid bit-manipulation overhead and speed up the BFS traversal during remeshing.
- **UI**: Customized the ImGui progress bar to use the premium teal accent color and replaced the default white/gray modal dimming background color with a dark translucent overlay to prevent white-washing.
- **Performance**: Converted the remeshing process (Remesh) to run asynchronously in a background worker thread, eliminating application freezes during voxelization and surface reconstruction.
- **UI**: Added a thread-safe progress modal popup in ImGui that tracks and displays real-time progress for voxelization, flood-filling, and reconstruction stages.
- **Performance**: Optimized Marching Cubes reconstruction by replacing the expensive string-based vertex hash map with zero-overhead integer edge lookup arrays, achieving a massive speedup.
- **Performance**: Optimized voxelization distance checks by comparing squared distances, avoiding millions of costly square root calculations.
- **Performance**: Replaced sparse maps (`std::unordered_map`) for color and material fields with flat pre-allocated vectors for O(1) cache-friendly direct memory access.
- **Input**: Blocked keyboard hotkeys and brush interactions while remeshing is active to prevent race conditions and ensure mesh data integrity.
- **Tools**: Replaced the central sphere handle of the transform gizmo with camera-plane aligned corner brackets of a square that matches the diameter of the rotation rings (providing feature parity with the legacy JS `planeW` camera translation indicator), updating both visual rendering (with a subtle transparent background) and the screen-space picking boundaries so that activation/hovering only occurs when the cursor is positioned directly over the corners.
- **Tools**: Replaced scale end-handle circles with squares (representing 3D cubes) and configured a yellow square in the center representing the universal/global scale cube (configured to be yellow always, larger, with dedicated center hover picking box supporting both `SCALE` and `SCALEU` universal operations) when scaling.
- **Tools**: Increased the visual size and length of the Transform Tool gizmo axes by default (clip-space size 0.20) and decreased the diameter of the rotation rings and screen-space rotation circles to make the gizmo layout cleaner and more compact.
- **Tools**: Fixed the Transform Tool gizmo axes orientation so they remain completely stable and do not rotate or shift when the camera angle is changed.
- **Input**: Resolved conflict between camera navigation and gizmo manipulation by prioritizing gizmo interaction when the mouse is over the handles and disabling gizmo input during active viewport navigation.
- **Tools**: Added interactive Measure and Divider tools featuring screen-space overlay lines, dynamic hover feedback, custom ticks/subdivisions, and rounded semi-transparent text badges showing distance or relative scale reference.
- **Tools**: Upgraded the Transform Tool gizmo to support full feature and visual parity with the legacy JS application. Configured custom color coding and sizing via ImGuizmo styling, implemented interactive pivot translation/rotation (Alt hotkey or UI toggle) that offsets vertex geometry dynamically via the edit matrix, added a CPU baking step on drag release to commit the pivot modifications with automatic octree/normal/bounding-box rebuild, and integrated a floating screen-space Lock/Unlock Pivot toggle button projected at the gizmo's center.
- **Core**: Added mesh matrix serialization to save/restore mesh transforms within the history stack for undo/redo support.
- **Settings**: Serialized divider divisions and measure perspective settings to the general section of `brush_settings.cfg`.
- **Camera**: Fixed a major perspective depth unprojection bug where clicking outside the mesh (`FREE` anchors) caused points to fly away to extreme distances due to linear z-depth interpolation in `Camera::unproject`. Now correctly uses the inverted viewport-view-projection matrix for exact, linear screen-to-world mapping.
- **Input**: Fixed camera navigation getting stuck and orbiting/spinning continuously if the mouse cursor crossed over ImGui interface elements (such as the floating gizmo lock button or side panels) during drag rotation/panning.


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
