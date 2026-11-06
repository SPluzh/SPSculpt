#pragma once

#include <string>
#include <vector>
#include "mesh/Mesh.h"
#include "scene/Scene.h"

namespace ZBrushBridge {

struct SendResult {
    bool success = false;
    int meshesSent = 0;
    double elapsedMs = 0.0;
    std::string message;
};

// Checks if ZBrush main window is currently open/running on the system
bool isZBrushRunning();

// Exports active mesh to out_subtools/ and sends WM_COPYDATA to ZBrush to import it
SendResult sendActiveMeshToZBrush(const Mesh* mesh);

// Exports all (or visible) meshes to out_subtools/ and sends WM_COPYDATA to ZBrush to import scene
SendResult sendSceneToZBrush(const Scene& scene, bool visibleOnly = false);

} // namespace ZBrushBridge

