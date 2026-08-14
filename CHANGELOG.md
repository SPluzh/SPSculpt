# Changelog

All notable changes to this project will be documented in this file.

## [0.1.11]
- **Reference Images**: Fixed edge pixel stretching on rectangular reference images.
- **Reference Images**: Fixed aspect ratio distortion and shearing when rotating 2D reference images.
- **Reference Images**: Made "Master Visible" toggle affect all reference images simultaneously, with global shortcut `Shift + Z`.
- **Reference Images**: Added hotkey `Z` to toggle Edit Mode (interactive manipulation gizmo) for reference images.

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
