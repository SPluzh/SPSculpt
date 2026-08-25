# Changelog

All notable changes to this project will be documented in this file.

## [0.2.13]
- **glTF Import**: Added support for opening `.gltf` 3D scenes that rely on external `.bin` geometry data files.
- **glTF Import**: Preserved original model and object names in the Scene Outliner upon importing glTF files.
- **glTF Import**: Added automatic normal map texture and surface relief rendering for glTF 3D models.
- **glTF Import**: Fixed mesh surface cuts and buffer corruption caused by invalid UV vertex splitting on glTF models.
- **glTF Import**: Resolved an issue with non-ASCII and Unicode file paths when loading glTF textures on Windows.

## [0.2.12]
- **Scene Outliner**: Fixed right-side button alignment on non-square reference image cards.
- **Reference Images**: Restricted reference edit mode (`Z`) to toggle only when reference images are visible in the scene, preventing edit mode activation when images are hidden via `Shift+Z` or panel toggles.

## [0.2.11]
- **Hotkeys**: Bound `F1` key to toggle Scene Outliner panel and `F2` key to toggle Camera Bookmarks panel. Added shortcut hints to UI menus and Hotkey HUD.

## [0.2.10]
- **Reference Images**: Restricted reference edit mode (`Z`) to activate only when visible images exist, preventing accidental cursor hiding.
- **Project Files**: Fixed project loading locking the brush cursor when saved in reference edit mode.

## [0.2.9]
- **Lighting Controls**: Added global light and HDRI rotation around the vertical Y-axis via `Shift + Middle Mouse Button` drag and a dedicated "Global Rotation" UI slider in Light Source Management.
- **License**: Added GNU General Public License v3.0 (GPLv3) to the repository.



## [0.2.8]
- **Voxel Remesh Speed**: Quad Voxel Remesh is now over 5.4x faster (execution time reduced from ~21s down to ~3.8s for heavy 1.7M polygon meshes) thanks to multi-threaded surface reconstruction and optimized spatial indexing.


## [0.2.7]
- **Brush Cursor Deformation**: Fixed oval/elliptical distortion when editing brush parameters (Radius `S`, Intensity `A`, Focal Shift `D`) by orienting the modal preview circle normal directly toward the camera position and bypassing tablet tilt scaling during parameter adjustment.
- **Brush Cursor**: Added open-center crosshair brush cursor for sculpting with a toggle checkbox in Brush Settings and Preferences.
- **Camera Focus on Import**: Automatically focus and frame the camera on imported 3D mesh objects when opening files via command-line arguments, File → Open/Import, or Drag & Drop.
- **OBJ Import Speed**: Accelerated OBJ file loading speed by 10–15x (from ~13s down to ~0.8s for heavy 3.4M polygon models) through optimized text parsing.
- **Transform Tool UI Layering**: Fixed an issue where the Transform Tool gizmo rendered in front of ImGui windows by directing ImGuizmo to the viewport background draw list.
- **Transform Tool & Undo**: Fixed an issue where scaling an object with the gizmo could not be undone via Ctrl+Z.
- **Undo/Redo Acceleration**: Reduced Undo/Redo latency for transform operations on heavy meshes (3.4M+ polygons) from ~500ms down to <10ms by eliminating redundant mesh rebuilds and GPU re-uploads.
- **Transform Gizmo Performance**: Accelerated live gizmo dragging and scale baking (from ~124ms down to ~19ms) using multi-threaded parallel processing.
- **Memory Diagnostics**: Fixed incorrect 0.00 MB memory display for transform actions in the undo stack history.

## [0.2.6]
- **Sculpting Performance**: Significantly accelerated `ClayBuildup` and `Clay` brushes (up to 20x faster vertex deformation) during high-density mesh sculpting.
- **Sculpting Performance**: Added smart surface normal caching during brush strokes to eliminate frame latency.
- **Diagnostics**: Added detailed stroke timing breakdown for brush deformation in performance logs.

## [0.2.5]
- **Scene Outliner**: Added Import and Export buttons to top header toolbar.
- **Symmetry Visualization**: Restricted symmetry plane lines to display strictly on the currently active object, preventing global overlay on inactive background meshes.
- **Sculpting & Symmetry**: Added smooth fade-out transition animation when disabling symmetry to match the enable symmetry visual behavior.
- **Camera Navigation**: Added "Animate Camera Transitions" setting in Preferences to toggle smooth animation when framing objects ('F'), switching camera bookmarks, or snapping views ('Shift').
- **Camera Bookmarks**: Saved camera bookmarks now automatically capture and restore object visibility and polygon hiding states.

## [0.2.4]
- **Sculpting & Symmetry**: Fixed symmetry brush cursor placement and eliminated ghost cursor artifacts on non-proportionally scaled objects.
- **Voxel Remesh**: Fixed polygon stretching during remeshing by automatically baking non-uniform scale, ensuring uniform square quads across all transformed objects.

## [0.2.3]
- **UI Consolidation**: Moved Undo History and Tablet Diagnostics into dedicated Preferences tabs ("Undo & System", "Tablet & Stylus") and removed redundant items from the Panels menu.

## [0.2.2]
- **Reference Images**: Added 4 mid-edge handles for direct side scaling.
- **Reference Images**: Resizing now anchors to the opposite edge/corner to keep it fixed in place.
- **Reference Images**: Added rotation-aware mouse cursors for all transform handles.
- **Reference Images**: Enabled 3D camera navigation during reference image edit mode (`Z`).
- **Reference Images**: Automatically hide sculpt brush cursor while editing reference images.
- **Reference Images**: Rendered transform handles on background layer to prevent UI window overlap.
- **Reference Images**: Drag-and-dropping an image into the viewport automatically activates reference edit mode for immediate manipulation without opening the Scene Outliner panel if it was closed.
- **Reference Images**: Rotation handle and overlay toolbar (opacity slider & lock toggle) automatically relocate across 4 candidate positions (Top → Bottom → Right → Left) to ensure they always remain visible on screen, switching to a vertical slider with a perfectly square lock button shifted off-center when positioned on side edges.
- **Reference Images**: Set `scale = 1.0` to 100% native resolution (1:1 pixel size) when loading images smaller than the viewport.


## [0.2.1]
- **Undo System Diagnostics**: Added stage-by-stage timing instrumentation and real-time diagnostic logging to track performance bottlenecks during undo/redo operations.
- **Sculpting Stability**: Added automatic stroke cancellation when triggering undo during an active brush stroke to prevent history state corruption.
- **Memory & Safety**: Implemented bounds safety checks on vertex delta vectors during state restoration.

## [0.2.0]
- **Single-Row Outliner Header**: Streamlined primitive creation (Sphere, Geosphere, Cube, Cylinder, Torus) and reference image addition buttons into a unified icon-only header row with hover tooltips.
- **Glassmorphism Outliner UI**: Modern card-based Scene Outliner redesign with translucent glass visual style.
- **Card Layout**: Fixed right-side clipping so action buttons and card content are always fully visible.
- **Mouse Wheel Scroll**: Fixed inner card jitter when scrolling over cards so the outliner list scrolls smoothly.
- **HD Card Previews**: Upgraded thumbnail resolution to 160×160 for crisp rendering without blur or pixelation.
- **Tool Items Grouping**: Separated Measure and Divider tools into independent collapsible group cards, with individual division controls for each Divider segment.
- **Reference Image Cards**: Added lock toggle button (lock/unlock transforms) and opacity percentage slider directly to cards in the Scene Outliner.
- **Reference Image Controls Clean-up**: Moved "+ Add Reference Image" button to the top header of the Scene Outliner and removed redundant property sliders/settings from the bottom panel.
- **Search & Category Filters**: Search by object name and quick filtering by categories (All / Meshes / Refs / Tools).
- **Quick Action Buttons**: Direct card controls for per-viewport visibility (V1/V2), camera focus, duplicating, and deleting.

## [0.1.17]
- **Project Files**: Updated default project file format to `.spsculpt`.
- **Backward Compatibility**: Fully retained support to open, import, and drag-and-drop existing `.sgl` project files.

## [0.1.16]
- **Startup Performance**: Eliminated initial black screen by rendering an immediate early frame upon window creation.
- **Shader Caching**: Added binary shader caching in `resources/shader_cache` to accelerate subsequent application launches.
- **Lazy Matcap Loading**: Matcap textures are now loaded on-demand instead of blocking application startup.
- **Mesh Topology**: Accelerated topology calculation algorithm by 3–5x and added precalculated topology storage in `.sgl` project files (SGL v13).

## [0.1.15]
- **File Association & Drag & Drop**: Dragging an `.sgl` project file into the viewport or launching the application with an `.sgl` file path parameter (first command line argument `argv[1]` / dragging file onto executable) now opens the `.sgl` scene project directly.

## [0.1.14]
- **Camera Bookmarks**: Reference images that were not stored in a loaded camera bookmark are now automatically hidden upon applying the bookmark.
- **Unicode Support**: Implemented comprehensive UTF-8/Unicode file path support across Windows, enabling opening, importing, exporting, and saving models, project files (.sgl), reference images, timelapses, and presets with non-ASCII and Cyrillic/Russian characters.
- **Font & Rendering**: Enabled Cyrillic character set support in system fonts so Russian file paths, mesh outliner names, bookmarks, and UI text render clearly in ImGui.
- **Camera**: Saved active camera state (position, rotation, target, FOV, and projection) in `.sgl` project files (SGL v12) and restored it upon opening.
- **Reference Images**: Preserved reference image Edit Mode state in `.sgl` project files so loading a project keeps Edit Mode disabled if it was disabled upon saving.
- **Camera Bookmarks**: Preview thumbnails now automatically frame and display the entire object in the icon.
- **Camera Bookmarks**: Bookmark preview images are now saved directly into `.sgl` project files and restored upon opening.

## [0.1.13]
- **Reference Images**: Implemented full serialization and deserialization of Scene Reference Images in `.sgl` project files (SGL v10). Image properties (file path, opacity, scale, 2D offsets, rotation, global and per-viewport V1/V2 visibility, 2D pinning, and lock state) as well as embedded raw image data are now saved directly inside project files and restored upon opening.

## [0.1.12]
- **Reference Images**: Prevented the manipulation frame, corner handles, rotation stem, and overlay toolbar from appearing when editing mode (`Z`) is enabled for reference images that are hidden in the current viewport.

## [0.1.11]
- **Reference Images**: Fixed edge pixel stretching on rectangular reference images.
- **Reference Images**: Fixed aspect ratio distortion and shearing when rotating 2D reference images.
- **Reference Images**: Made "Master Visible" toggle affect all reference images simultaneously, with global shortcut `Shift + Z`.
- **Reference Images**: Added hotkey `Z` to toggle Edit Mode (interactive manipulation gizmo) for reference images.
- **Reference Images**: Added a rotated overlay toolbar attached to the top-right frame border during Edit Mode (`Z`), featuring a smooth opacity slider and a vector padlock icon button to toggle image locking.

## [0.1.10]
- **Camera Bookmarks**: Fixed an issue where restoring camera bookmarks only restored one reference image when multiple reference images shared the same file path. Updated reference image restoration to use a 3-pass matching algorithm so all duplicate-path reference images maintain independent visibility, transform, and opacity states.

## [0.1.9]
- **Camera Navigation**: Fixed Gizmo/Snap Cube click behavior so snapping to a view angle always re-triggers even after manual rotation on the same side, and preserves the active projection mode (perspective or orthographic).

## [0.1.8]
- **Camera Bookmarks**: Added camera bookmark system with FOV, reference image states, thumbnail previews, `Ctrl+1..9` hotkeys, and persistent `.sgl` saving.

## [0.1.7]
- **Reference Images**: Integrated into Scene Outliner list with thumbnails, shared property inspector, drag-and-drop, and per-viewport (V1/V2) visibility toggles. Removed standalone window.

## [0.1.6]
- **UI**: Added 2D Pan/Zoom mode button to the top-right HUD with a Reset 2D View option.

## [0.1.5]
- **Reference Images**: Added native Drag & Drop support to directly load image files (.png, .jpg, .jpeg, .bmp, .tga, .webp, .gif, .psd, .hdr, etc.) dropped anywhere into the 3D Viewport window.
- **Reference Images**: Automatically opens and selects the newly dropped reference image in the Reference Images panel for quick manipulation.

## [0.1.4]
- **3D Camera**: Fixed 3D camera zoom alignment so zooming in/out accurately targets the 3D surface point under the crosshairs without drifting or flying off screen.
- **3D Camera**: Rendered crosshair pivot marker during active 3D viewport navigation (orbiting and drag zooming).

## [0.1.3]
- **Reference Images**: Directly move, scale, and rotate reference images in the viewport.
- **Reference Images**: Added dedicated image edit mode to prevent accidental sculpting while adjusting images.
- **Reference Images**: Hides the brush cursor and highlights interactive frames in the application accent color.
- **Reference Images**: Added image rotation sliders, quick reset buttons, file browser, and fixed preview orientations.
- **Reference Images**: Added cursor-centric 2D zooming so the view scales directly relative to the mouse position.
- **Reference Images**: Added a 3D-style crosshair pivot marker during 2D zooming to highlight the active zoom center.

## [0.1.2]
- **Wireframe**: Added high-quality anti-aliased wireframe rendering on objects.
- **Floor Grid**: Added anti-aliased procedural floor grid with clear X/Z color axes.

## [0.1.1]
- **Camera**: Automatically framed camera on scene startup to focus properly on the initial model.
- **Primitives**: Replaced default initial mesh with a Geosphere primitive.
- **Primitives**: Welded edge vertices on Subdivided Cube primitives to ensure a continuous manifold mesh for sculpting.
- **Primitives**: Added high-density concentric ring quad subdivisions to Cylinder top and bottom caps for smooth deformation.
- **Rendering**: Set "Matcap FV" as the default matcap preset and fixed reliable matcap loading and saving across scene files (.sgl) and app restarts.
- **Rendering**: Fixed object selection outline distortion and offset when using Split Viewport screen mode.

## [0.1.0]
- **App Icon**: Added custom 3D clay app icon with teal accent branding for Windows executable (.exe) and SDL window title bar.
- **Outliner**: Added real-time thumbnail previews for scene objects with background rendering for smooth performance.
- **Version System**: Established unified application versioning system at 0.1.0 across builds, headers, and UI.

## [0.0.32]
- **Brushes**: Fixed an issue where Move, Drag, and Elastic brushes did not respect "Lock Single PolyGroup" after the first frame of a stroke.

## [0.0.31]
- **UI**: Saved Sculpt Layers panel open/closed visibility across application restarts and added a shortcut in the Panels menu.

## [0.0.30]
- **Brushes**: Removed center pinch from V-Tool brush for cleaner V-grooves. Assigned hotkey `3` and custom tool icon.
- **Sculpt Layers**: Added non-destructive sculpt layer system with multi-layer undo/redo support and disk logging optimizations.

## [0.0.29]
- **Sculpt Layers**: Optimized active layer sculpting performance to eliminate stroke latency and fixed vertex displacement artifacts.

## [0.0.28]
- **Remeshing**: Saved Voxel Remesh settings persistently and set "Align Symmetry Axes" off by default.

## [0.0.27]
- **Tools**: Added Brush Icon Capture tool to frame and export transparent PNG brush icons directly to resources.

## [0.0.26]
- **Brushes**: Fixed vertex explosion bug on Elastic brush boundaries and updated falloff profile for smoother decay.

## [0.0.25]
- **Trim Tool**: Improved cut accuracy along 2D lasso contours with automatic hole filling and interpolated edge attributes.

## [0.0.24]
- **ClipCurve Tool**: Added smooth 2-stage projection and constrained relaxation pipeline for crisp, flat surface cuts.

## [0.0.23]
- **ClipCurve Tool**: Fixed projection flip artifacts and improved symmetry support during curve clipping operations.

## [0.0.22]
- **UI**: Added a symmetry dropdown attached directly to the Floating HUD for quick axis toggles and geometry mirroring.

## [0.0.21]
- **UI**: Consolidated camera, rendering, and log settings into a unified Preferences modal and added a mesh statistics HUD toggle.

## [0.0.19]
- **UI**: Added a vertical Focal Shift slider to the Floating HUD for quick brush parameter adjustments.

## [0.0.18]
- **Performance**: Accelerated lasso mask selection and clear-mask actions on high-poly models with active symmetry.

## [0.0.17]
- **Snapshot**: Added a "Model Snapshot" feature to freeze reference viewports during active sculpting.

## [0.0.16]
- **Brushes**: Expanded max brush radius (1000px) and intensity (1000%), and upgraded Smooth brush to multi-pass smoothing.

## [0.0.15]
- **UI**: Redesigned Floating Island HUD with compact layout, Lucide vector icons, and projection toggles.

## [0.0.14]
- **Camera**: Improved accuracy and scale parity when switching between Perspective and Orthographic projection modes.

## [0.0.13]
- **Symmetry**: Fixed World Space symmetry reflections across active sculpt brushes, raycasts, and cursor indicators.

## [0.0.12]
- **Transform Tool**: Added vertex masking support and multi-axis symmetry handling to transform operations.

## [0.0.11]
- **PolyGroups**: Fixed polygroup isolation toggle when clicking on isolated regions.

## [0.0.10]
- **Camera**: Improved frame-selection (`F` hotkey) bounding calculation for single and multi-object scene framing.

## [0.0.9]
- **Application**: Renamed application executable to SPSculpt, optimized build size, and added optional `--console` launcher mode.

## [0.0.8]
- **Symmetry**: Added multi-axis (X/Y/Z) bitmask symmetry support across all sculpting brushes, cursors, and primitive generators.
- **UI**: Added Solo view mode (`C` hotkey) and quick symmetry toggle (`Alt + X`).

## [0.0.7]
- **Performance**: Multi-threaded grab brushes (Move, Drag, Elastic) using OpenMP for lag-free performance on heavy meshes.

## [0.0.6]
- **Brushes**: Added "Lock Single PolyGroup" setting and upgraded dynamic UI scaling options.

## [0.0.5]
- **Graphics**: Added High-DPI display support, Lucide icon font integration, tablet pressure curve editor, custom stamp parameters, SSAO ambient occlusion, and FXAA antialiasing.

## [0.0.4]
- **Graphics**: Added Wet Clay shader parameters, custom Matcap/Texture loading, split viewport support, smooth camera focus animations, and picking rotation pivots.

## [0.0.3]
- **Input**: Added full graphics tablet support (WinTab and Windows Ink) with pressure and pen tilt sensitivity.
- **Performance**: Async multi-threaded Voxel Remeshing with live progress modal.

## [0.0.2]
- **Tools**: Added Mask Gradient Blur tool and custom color-coded lasso selection overlays.

## [0.0.1]
- **Initial Release**: Extracted native C++ desktop engine with multi-format 3D import/export (SGL, OBJ, STL, PLY, GLTF), PBR/Matcap shaders, performance optimizations, and full ImGui UI integration.
