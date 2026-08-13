# Changelog

All notable changes to this project will be documented in this file.

## [0.1.0]
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
