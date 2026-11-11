cmake_minimum_required(VERSION 3.20)

# Read version from single source of truth (version.txt)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" APP_VERSION_RAW)
string(STRIP "${APP_VERSION_RAW}" APP_VERSION_RAW)

if(APP_VERSION_RAW MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)$")
    set(APP_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(APP_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(APP_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(APP_VERSION_SUFFIX "${CMAKE_MATCH_4}")
    set(APP_VERSION_SEMVER "${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_VERSION_PATCH}")
else()
    set(APP_VERSION_MAJOR "0")
    set(APP_VERSION_MINOR "1")
    set(APP_VERSION_PATCH "0")
    set(APP_VERSION_SUFFIX "")
    set(APP_VERSION_SEMVER "0.1.0")
endif()

project(SPSculpt VERSION ${APP_VERSION_SEMVER} LANGUAGES C CXX)

# Auto-generate src/common/Version.h from template
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/Version.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/Version.h"
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)

add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC third_party/ufbx)
target_compile_options(ufbx PRIVATE -O3)

add_library(lz4 STATIC third_party/lz4/lz4.c)
target_include_directories(lz4 PUBLIC third_party/lz4)
target_compile_options(lz4 PRIVATE -O3)

find_package(glm REQUIRED)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

set(MANIFOLD_PAR "NONE" CACHE STRING "Parallel backend" FORCE)
set(MANIFOLD_EXPORT OFF CACHE BOOL "" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)

FetchContent_Declare(manifold
    GIT_REPOSITORY https://github.com/elalish/manifold.git
    GIT_TAG v2.5.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(cdt
    GIT_REPOSITORY https://github.com/artem-ogre/CDT.git
    GIT_TAG 1.4.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(cdt)
if(NOT cdt_POPULATED)
    FetchContent_Populate(cdt)
    add_library(CDT INTERFACE)
    target_include_directories(CDT INTERFACE ${cdt_SOURCE_DIR}/CDT/include)
    add_library(CDT::CDT ALIAS CDT)
endif()

FetchContent_MakeAvailable(json manifold)


add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo/ImGuizmo.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)


find_package(SDL2 REQUIRED)
find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
find_path(GLES_INCLUDE_DIR NAMES GLES3/gl3.h GLES2/gl2.h)
find_library(EGL_LIBRARY NAMES EGL libEGL)
find_library(GLES_LIBRARY NAMES GLESv2 libGLESv2)

if(TARGET SDL2::SDL2)
    target_link_libraries(imgui PRIVATE SDL2::SDL2)
else()
    target_link_libraries(imgui PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(imgui PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
target_include_directories(imgui PRIVATE ${EGL_INCLUDE_DIR} ${GLES_INCLUDE_DIR})

include_directories(
    ${EGL_INCLUDE_DIR}
    ${GLES_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

add_executable(SPSculpt
    # common
    src/common/CrashReporter.cpp
    src/common/IniFile.cpp
    src/common/RecentFiles.cpp
    src/common/StringUtils.cpp
    src/common/SystemStats.cpp
    src/common/WorkTimer.cpp
    # scene
    src/scene/Camera.cpp
    src/scene/Scene.cpp
    # mesh
    src/mesh/Mesh.cpp
    src/mesh/MeshResolution.cpp
    src/mesh/Multimesh.cpp
    src/mesh/BVH.cpp
    src/mesh/NormalCalc.cpp
    src/mesh/Topology.cpp
    src/mesh/Layer.cpp
    src/mesh/Decimation.cpp
    src/mesh/dynamic/DynamicTopology.cpp
    # sculpt algorithms
    src/sculpt/SculptEngine.cpp
    src/sculpt/Remesh.cpp
    src/sculpt/KeepMasked.cpp
    src/sculpt/RelaxEngine.cpp
    src/sculpt/PrimitiveGenerator.cpp
    src/editing/Subdivision.cpp
    src/editing/Thickness.cpp
    src/editing/Reversion.cpp
    # render
    src/render/AngleRenderer.cpp
    src/render/ReferenceImage.cpp
    src/render/RenderTarget.cpp
    src/render/RenderSettings.cpp
    # files
    src/files/FileManager.cpp
    src/files/ImportSGL.cpp
    src/files/ExportSGL.cpp
    src/files/ImportOBJ.cpp
    src/files/ExportOBJ.cpp
    src/files/ImportSTL.cpp
    src/files/ExportSTL.cpp
    src/files/ImportPLY.cpp
    src/files/ExportPLY.cpp
    src/files/ImportGLTF.cpp
    src/files/ExportGLTF.cpp
    src/files/ImportFBX.cpp
    src/files/ImportGoZ.cpp
    src/files/ExportGoZ.cpp
    src/files/GoZProfiler.cpp
    src/files/MeshUtils.cpp
    # zbrush bridge
    src/zbrush_bridge/ZBrushBridgeConfig.cpp
    src/zbrush_bridge/ZBrushWatcher.cpp
    src/zbrush_bridge/ZBrushSender.cpp
    # editing
    src/editing/manifold/ManifoldBridge.cpp
    src/editing/manifold/LassoCutter.cpp
    src/editing/manifold/QuadCapGenerator.cpp
    src/editing/SculptManager.cpp
    src/editing/ClipCurveTool.cpp
    src/editing/TrimTool.cpp
    src/editing/SplitTool.cpp
    src/editing/BooleanTool.cpp
    src/editing/CameraController.cpp
    src/editing/BrushCursor.cpp
    src/editing/ArmatureTool.cpp
    src/editing/PoseTool.cpp
    src/editing/TubeTool.cpp
    src/editing/CurveFoldsTool.cpp
    src/editing/PrimitiveTool.cpp
    src/editing/PolyGroupTool.cpp
    src/editing/QuadDrawTool.cpp
    src/editing/MaskExtractTool.cpp
    src/editing/SelectionManager.cpp
    src/editing/ComponentSelectTool.cpp
    src/editing/EdgeCreaseTool.cpp
    src/editing/ComponentOps.cpp
    src/editing/undo/UndoManager.cpp
    src/sculpt/ArmatureGraph.cpp
    src/sculpt/CurveSpline.cpp
    # retopo
    src/retopo/SurfaceSnapper.cpp
    src/retopo/QuadDrawMeshBuilder.cpp
    # timelapse
    src/timelapse/TimelapsePlayer.cpp
    src/timelapse/TimelapseSerializer.cpp
    # brushes
    src/brushes/BrushPreset.cpp
    src/brushes/BrushPresetManager.cpp
    # gui
    src/gui/GuiManager.cpp
    src/gui/HudCustomization.cpp
    src/gui/ToolOverlayTheme.cpp
    # platform
    src/platform/HotkeyPreset.cpp
    src/platform/HotkeyManager.cpp
    src/platform/HotkeyDispatcher.cpp
    src/platform/TabletInput.cpp
    src/platform/FileDialog.cpp
    src/platform/HardwareCursor.cpp
    src/platform/NativeMain.cpp
)

if(WIN32)
    set_target_properties(SPSculpt PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "SPSculpt")
    target_sources(SPSculpt PRIVATE resources/app.rc)
    target_link_libraries(SPSculpt PRIVATE comdlg32 psapi dbghelp shell32)
endif()

if(TARGET SDL2::SDL2)
    target_link_libraries(SPSculpt PRIVATE SDL2::SDL2)
else()
    target_link_libraries(SPSculpt PRIVATE ${SDL2_LIBRARIES})
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

target_link_libraries(SPSculpt PRIVATE imgui ${EGL_LIBRARY} ${GLES_LIBRARY} glm::glm nlohmann_json::nlohmann_json ufbx manifold Clipper2 CDT lz4)
target_compile_options(SPSculpt PRIVATE -O3 -g)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(SPSculpt PRIVATE OpenMP::OpenMP_CXX)
endif()

if(WIN32)
    target_compile_definitions(SPSculpt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MINGW)
    target_link_options(SPSculpt PRIVATE -static-libgcc -static-libstdc++)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(SPSculpt PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(SPSculpt PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

add_custom_command(
    TARGET SPSculpt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/resources
        $<TARGET_FILE_DIR:SPSculpt>/resources
    COMMENT "Copying resources to binary output directory"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/_build_/dist")
    add_custom_command(
        TARGET SPSculpt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SPSculpt>
            "${CMAKE_SOURCE_DIR}/_build_/dist/SPSculpt.exe"
        COMMENT "Auto-syncing SPSculpt.exe to dist directory"
    )
endif()

if(UNIX AND NOT APPLE)
    add_custom_command(
        TARGET SPSculpt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_SONAME_FILE:manifold>
            $<TARGET_FILE_DIR:SPSculpt>/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_SONAME_FILE:Clipper2>
            $<TARGET_FILE_DIR:SPSculpt>/
    )
endif()

if(WIN32)
    add_library(SPSculptBridge SHARED
        src/zbrush_bridge/dll/SPSculptBridge.cpp
    )
    set_target_properties(SPSculptBridge PROPERTIES
        PREFIX ""
        OUTPUT_NAME "SPSculptBridge"
    )
    target_link_libraries(SPSculptBridge PRIVATE comctl32 user32)
    if(MINGW)
        target_link_options(SPSculptBridge PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_custom_command(
        TARGET SPSculptBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/zbrush_plugin/SPSculptBridgeData
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SPSculptBridge>
            ${CMAKE_SOURCE_DIR}/zbrush_plugin/SPSculptBridgeData/SPSculptBridge.dll
    )
endif()


