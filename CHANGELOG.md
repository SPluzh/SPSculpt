# Changelog

All notable changes to this project will be documented in this file.

## [0.3.16]
- **Modeling Tools**:
  - `Extrude`: Extrude selected faces or open boundary edges into new geometry.
  - `Bevel`: Inset faces and bevel sharp edges with uniform perpendicular chamfers, miter corners, multi-segment fillets (1-10 segments), Chamfer/Solid modes, interactive live viewport preview, and watertight welded corner caps.
  - `Bridge`: Connect pairs of faces or open edge loops with clean quad strips.
  - `Merge / Collapse`: Merge vertices to center or collapse edges to midpoints.
  - `Delete`: Cleanly remove faces, edges, or vertices without leaving orphaned points.
  - `Close Holes`: Automatically detect and cap open boundary loops and holes.
- **Crease Visibility**: Added a toggle button to show or hide pink dashed crease lines in the viewport.
- **Inverted Normals Display**: Viewport shading option to highlight flipped/inverted polygon normals with current shading/matcap preserved and customizable color.
- **Primitives Normal Fix**: Fixed inverted normal face winding for `Capsule` and `Torus` primitives.
- **Primitives Creation**: Removed automatic opening of the settings panel when adding primitives.
- **Scene Outliner**: Removed camera focus, duplicate, and delete buttons from object cards (kept in right-click context menu).


## [0.3.15]
- **Mask Extract Tool**: Create clothing, armor, and new parts directly from painted masks into a separate SubTool.
- **Fill Modes**:
  - `Shell`: Hollow layer with thickness (Front, Rim, Back).
  - `Fill`: Watertight solid volume sealed at the base.
  - `None`: Single-sided open surface sheet.
- **3D Viewport Handle**: Drag the 3D handle on the mesh to adjust thickness live in the viewport.
- **Smoothness Modes**: Smooth jagged mask edges (`Border Only`) or relax the entire extracted mesh (`All`).
- **Auto-Creasing & PolyGroups**: Automatic PolyGroups for sides and caps, with optional sharp creases for subdivision.
- **Live Preview & Shortcuts**: Top-center viewport HUD with quick `Enter` to Accept and `Esc` to Cancel.

## [0.3.14]
- **Dynamic Subdivision**: Real-time non-destructive mesh smoothing preview without altering base geometry.
- **Dual Wireframe (ZBrush Style)**: Clear visual hierarchy separating base cage edges on the smoothed surface from interior subdivision grid with adjustable `Subdiv Grid Opacity`.
- **Dashed Crease Edges**: Sharp crease visualization rendered as vibrant dashed lines directly along the limit surface (both for base mesh and dynamic subdiv/thickness).
- **Dynamic Thickness**: Non-destructive solid shell extrusion preview with customizable thickness, inward/outward offset, rim segments, and miter-angle compensation.
- **Pre & Post Subdiv Pipeline**: Apply thickness on the coarse cage to get smoothly beveled rim corners (Pre-Subdiv) or extrude along smooth Catmull-Clark normals (Post-Subdiv).
- **Auto-Crease Rims & PolyGroups**: Automatic boundary edge creasing to preserve crisp rim borders and distinct PolyGroup assignment for Front, Back, and Rim faces.
- **Crease Edges**: Preserve sharp hard edges and PolyGroup borders during subdivision with adjustable crease angles.
- **Continuous Crease Sharpness**: Fractional `CreaseLvl` slider for smooth, continuous edge bevel and rounding control at any subdivision level without dead zones.
- **Outliner Controls**: Toggle button and settings dropdown (`▼`) on each object card to quickly adjust levels or bake.
- **Bake & Multiresolution**: Freeze subdivision into standard geometry or convert it into multiresolution sculpting levels.
- **Fast & Responsive**: Real-time smoothing updates during sculpting with no stroke lag.
- **Full Undo/Redo**: All subdivision settings and creases are fully tracked in undo/redo history.
- **Startup Preferences**: Added a setting in Preferences ("Create default sphere on startup") to toggle creating the default sphere when launching with an empty scene.

## [0.3.13]
- **Interactive Hotkeys Keyboard**: Physical key presses now light up on the virtual keyboard in Settings.
- **Modifier Preview**: Holding Ctrl, Shift, or Alt instantly shows all shortcuts assigned with them.

## [0.3.12]
- **Linux AppImage**: Standalone portable package with universal runtime and persistent user config in `~/.config/spsculpt/`.

## [0.3.11]
- **Primitive Handle Glow**: 3D handles on primitives now highlight and glow on hover, just like the Tube Tool.
- **Window Layering**: Viewport manipulators, curves, and rulers now stay behind open UI panels and windows.
- **Reliable Selection**: Fixed Transform tool gizmo blocking mouse clicks on primitive handles.

## [0.3.10]
- **Native Linux Support**: SPSculpt now runs natively on Linux as a standalone, portable application.
- **Graphic Tablets on Linux**: Full stylus pressure sensitivity, tilt, and eraser support for sculpting.
- **Tablet Preferences**: New calibration tab to customize pressure response curves and test your pen.
- **Native File Dialogs**: Standard Linux file picker for importing and exporting 3D models and scenes.
- **System Fonts & Cyrillic**: Sharp interface rendering using distribution fonts with full Cyrillic support.
- **Undo & Tube Stability**: Fixed heap corruption crash on Linux when undoing procedural Tube modifications and topological changes.

## [0.3.9]
- **Quad Draw Retopology**: Interactive surface retopology tool with real-time snapping to target meshes and full Undo/Redo (Ctrl+Z) support.
- **Point & Quad Creation**: Click to place surface points, Shift+Click to create quads, and drag boundary edges or corner vertices to extrude with magnetic auto-welding.
- **Multi-Edge Extrude & Strip Tool**: Extrude multiple selected edges simultaneously, or sketch freehand strokes to quickly lay down quad strips following organic curves.
- **Tube Retopology**: Rapidly retopologize limbs and fingers using cross-section rings (**Tube Slice**) or an interactive 3D centerline curve (**Tube Curve**).
- **Multi-Cut & Loop Cut**: Slice across polygons with live previews and percentage snapping (25%/50%/75%), or insert Edge Loops via Ctrl+LMB.
- **Tweak & Backface Occlusion**: Drag vertices, edges, or entire polygons directly on the surface, with backface culling to prevent snapping through to the rear of the model.
- **Overlay Themes & Preferences**: Clean viewport overlays that render beneath UI panels, with customizable colors and magnetic weld markers in Preferences.

## [0.3.8]
- **Cylinder Radius Manipulator**: Added interactive 3D viewport manipulator and GUI slider for cylinder radius, seamlessly scaling top and bottom radii together.
- **Primitive Gizmo Independence**: Primitive resize handles stay firmly anchored to the object's geometric center and orientation, even when moving, rotating, or aligning the Transform tool pivot.
- **45° Sphere Radius Handle**: Sphere and Geosphere radius handles are angled at 45° to prevent overlapping with Transform tool axes.
- **Preserved Transform Scaling**: Scaling primitives with the Transform tool is preserved and no longer resets when tweaking procedural handles.
- **Boolean & Trim on Primitives**: Fixed cap quad winding order on Cylinders, Cones, and partial Torus so primitives generate watertight 2-manifold geometry compatible with Boolean, Trim, and Split tools.
- **Boolean & Trim on Tubes**: Fixed tube caps so baked tubes work seamlessly with Boolean, Trim, and Split tools.
- **Bake All Tubes**: Properly syncs and finalizes all active curves before converting tubes into editable meshes.

## [0.3.7]
- **Interactive Primitives**: 3D viewport handles to drag size, radius, height, and bevels directly on the mesh.
- **New Shapes**: Added Cone, Plane, and Capsule primitives.
- **Auto Density**: Mesh resolution automatically recalculates on resize to keep polygon size constant.
- **Explicit Bake**: Primitives stay procedural until clicking Bake — brush clicks no longer trigger auto-bake.
- **Wireframe Support**: Viewport wireframe display now works on procedural primitives.

## [0.3.6]
- **Profile Graphs & Viewport Editing**: Custom cross-sections and thickness profiles with interactive editing at the tube tip.
- **Move & Smooth on Curves**: Move and relax curves of selected tubes using standard Move and Smooth brushes with natural falloff.
- **Multi-Tube Editing**: Selecting multiple unbaked tubes in the Outliner displays and edits all curves simultaneously.
- **Streamlined Workflow**: Drawing across empty space authors a new tube, curves stay pinned on click until dragged, and inactive curves auto-hide.


## [0.3.5]
- **Tube & Curve Tool**: Create 3D curves and tubes by freehand drawing directly on surfaces or in space, or placing points manually.
- **Interactive Editing**: Move control points in 3D, extrude ends with Alt+Drag, click points to toggle sharp right-angle corners, and adjust thickness with dedicated handles.
- **Profiles & Tapering**: Supports Round, Flat Ribbon (hair/straps), Square, and Polygon shapes with start/end scaling, tapering, twist, and cap styles.
- **Symmetry & 1-Click Bake**: Real-time symmetry, persistent curve editing, and instant conversion into a sculptable mesh with full Undo (Ctrl+Z).

## [0.3.4]
- **SnakeHook Brush**: Pull horns, tentacles, and curved hooks smoothly out of meshes into 3D space.
- **Shape Controls**: Built-in Pinch (tapering), Magnify (volume preservation), Twist, and mesh relaxation.
- **Workflow Integration**: Seamless dragging into empty space with full support for symmetry, masks, and layers.

## [0.3.3]
- **Pose Tool**: Pose and bend character limbs using a 3D gizmo or classic Action Line.
- **Natural Deformations**: Smooth joint bending with limb isolation and mask support.
- **Symmetry & Undo**: Full support for symmetry and instant Undo/Redo (Ctrl+Z).

## [0.3.2]
- **Mesh Decimation**: Added topology simplification with volume preservation (QEM + dihedral curvature), decimating flat areas while preserving curved details.
- **Feature Preservation**: Manifold-safe edge collapse with normal flip protection, symmetry plane alignment, and PolyGroup/border retention.
- **UI & Undo**: Background worker with progress modal in "Topology & Remesh" panel and full Undo/Redo support.

## [0.3.1]
- **Undo for Measure & Divider Tools**: Deleting or clearing measure and divider segments in Outliner now supports full undo/redo (Ctrl+Z / Ctrl+Y).
- **Undo for Object Deletion**: Deleting objects in Outliner can now be undone via Ctrl+Z, fully restoring geometry, PolyGroups, and symmetry.
- **Outliner Boolean Tool**: Added a dedicated Boolean button with dropdown menu at the bottom of Scene Outliner supporting CSG operations across 2 or more selected objects with responsive layout and base selection.
- **Larger Object Previews**: Outliner and Bookmark thumbnails now tightly frame 3D objects, making previews significantly larger with minimal empty margins.

## [0.3.0]
- **Topology-Preserving Boolean**: Union, Difference, and Intersection preserve 100% of original mesh quads and density outside the cut seam.
- **Trim Tool**: Added solid Lasso cutting with real-time viewport preview, cutting away unwanted geometry with Nomad-style QuadCap.
- **Split Tool**: Separates objects into independent subtools via Lasso or Line cuts.
- **Quad Capping**: Automatically seals cut surfaces with clean quads matching mesh density.
- **PolyGroup & Shading Retention**: Preserves colors and PolyGroups across boolean cuts, with smooth shading and no dark crease artifacts.

## [0.2.29]
- **Auto PolyGroups**: Every newly created primitive, spawned object, and duplicate now automatically gets a distinct, unique PolyGroup ID and color.
- **Merge PolyGroups**: Merging multiple meshes now preserves the individual PolyGroups of each component.

## [0.2.28]
- **HUD Shading**: Added Matcap & PBR switch buttons with custom icons and settings dropdowns.
- **Matcap Previews**: Added visual thumbnail grid for quick matcap selection.

## [0.2.27]
- **Relax Brush**: Added Relax brush to equalize mesh wireframe without losing volume or surface details.
- **Topology Controls**: Supports Tangential slide, Surface Reprojection, boundary locking, and crease preservation.

## [0.2.26]
- **UI Styling**: Replaced default blue ImGui window resize grips, borders, and interaction highlights with the application's teal accent palette.

## [0.2.25]
- **Record Mode**: Added `REC` button to toggle stroke recording on/off per layer.
- **Invert Slider**: Extended intensity slider from `-100%` to `100%` to invert layers.
- **Rename & Reorder**: Added double-click renaming and drag-and-drop / button reordering.
- **Merge Layers**: Added per-layer merge down and "Merge All" to bake layers into base mesh.

## [0.2.24]
- **Camera Navigation**: Fixed zoom to pivot in Orthographic mode to smoothly focus on the surface point under cursor without drift or clipping when zooming up close.

## [0.2.23]
- **Wireframe Overhaul**: Fixed dotted/broken lines, flickering, and prevented the wireframe from bleeding through objects when zooming out.
- **Wireframe Settings**: Added an arrow dropdown next to the Wireframe button in HUD with an opacity slider and color picker.
- **3D Camera**: Increased maximum zoom-out distance limit and dynamically expanded far clipping planes to allow seamless zooming out on large models and scans without clipping.

## [0.2.22]
- **FBX Import**: Added support for Autodesk FBX (`.fbx`) files (Binary and ASCII) via file dialog and drag-and-drop.

## [0.2.21]
- **Scene Outliner**: Fixed object preview turning into a flat color after Voxel Remesh.
- **Voxel Remesh**: Fixed surface normals not recalculating for the new mesh.

## [0.2.20]
- **Hotkeys & Presets**: Added fully customizable keyboard and mouse shortcuts with built-in **ZBrush**, **Maya**, and **Blender** presets, plus custom `.preset` profile support.
- **Virtual Keyboard**: Added an interactive on-screen keyboard in Preferences (`Hotkeys` tab) to visually view, inspect, and reassign keys across modifier layers.
- **Conflict Detection & Search**: Added real-time shortcut conflict warnings and instant search across all actions and tools.

## [0.2.19]
- **Viewport Navigation**: Added Maya (`Alt + Mouse`) and Blender (`MMB`) navigation styles alongside default ZBrush controls.
- **Maya Navigation**: Resolved `Alt + LMB` orbit conflict by remapping alternative sculpting (negative brush mode) and PolyGroup eyedropper to `Ctrl + Alt + LMB` on mesh.
- **Preferences**: Added navigation preset selector with keybind hints in Preferences (`Camera & Viewport`).

## [0.2.18]
- **Symmetry**: Saved and restored per-mesh symmetry settings persistently in `.spsculpt` project files.
- **Scene Outliner**: Added a symmetry toggle button directly on object cards to view and control symmetry status for each mesh.

## [0.2.17]
- **Recent Files**: Accelerated the Recent Files window with background loading and persistent thumbnail caching to eliminate UI pauses on startup.
- **Recent Files**: Implemented smooth lazy loading for project preview cards to ensure fluid, lag-free scrolling across file histories.
- **Viewport & Rendering**: Fixed single-viewport aspect ratio distortion when opening `.spsculpt` projects with split mode disabled (`SplitMode::OFF`).
- **Camera Pipeline**: Ensured primary camera viewport dimensions automatically synchronize with active renderer aspect ratio during rendering passes and file loading.

## [0.2.16]
- **Project Files**: Saved and restored Split Viewport settings (mode and dual cameras) in `.spsculpt` files.
- **Screenshots & Previews**: Added full dual-viewport captures for screenshots and thumbnails when split screen is active, including reference images.

## [0.2.15]
- **Work Timer**: Added active project time tracking that automatically pauses when idle or when switching away from the app.
- **HUD**: Displayed project work time directly in a dedicated viewport island in full `HH:MM:SS` format.
- **Recent Files**: Showed cumulative work time on project cards in the Recent Files window.
- **Project Files**: Saved total work time into `.spsculpt` project files so session progress continues seamlessly when re-opened.

## [0.2.14]
- **Recent Files**: Added Recent Files modal (`Ctrl + Shift + O`) with card grid and 16:9 previews.
- **Thumbnails**: Preserved viewport aspect ratio for `.spsculpt` thumbnails with auto-refresh on save.
- **Thumbnails**: Fixed orthographic screenshot scaling and framing for transformed meshes.
- **Settings**: Added option in Preferences to save a PNG preview next to `.spsculpt` files.

## [0.2.13]
- **glTF Import**: Added vertex welding and dual-index rendering to fix seam cuts and UV stretching.
- **glTF Import**: Enabled Matcap normal map relief shading and resolved grazing-angle blackening artifacts.
- **glTF Import**: Added support for external `.bin` buffers, texture loading, UTF-8 paths, and preserved scene node names.

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
