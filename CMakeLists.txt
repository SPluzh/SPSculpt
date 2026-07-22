cmake_minimum_required(VERSION 3.20)
project(sculptsp CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(third_party/glm)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
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

add_executable(sculptsp
    # scene
    src/scene/Camera.cpp
    src/scene/Scene.cpp
    # mesh
    src/mesh/Mesh.cpp
    src/mesh/Octree.cpp
    src/mesh/NormalCalc.cpp
    src/mesh/Topology.cpp
    # sculpt algorithms
    src/sculpt/SculptEngine.cpp
    src/sculpt/Remesh.cpp
    # render
    src/render/AngleRenderer.cpp
    src/render/ReferenceImage.cpp
    src/render/RenderTarget.cpp
    src/render/RenderSettings.cpp
    # editing
    src/editing/SculptManager.cpp
    src/editing/CameraController.cpp
    src/editing/BrushCursor.cpp
    # gui
    src/gui/GuiManager.cpp
    # platform
    src/platform/HotkeyDispatcher.cpp
    src/platform/NativeMain.cpp
)

if(TARGET SDL2::SDL2)
    target_link_libraries(sculptsp PRIVATE SDL2::SDL2)
else()
    target_link_libraries(sculptsp PRIVATE ${SDL2_LIBRARIES})
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

target_link_libraries(sculptsp PRIVATE imgui ${EGL_LIBRARY} ${GLES_LIBRARY} glm)
target_compile_options(sculptsp PRIVATE -O3 -g)

if(MINGW)
    target_link_options(sculptsp PRIVATE -static-libgcc -static-libstdc++)
endif()
