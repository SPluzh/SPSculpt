#pragma once
#include <vector>
#include <string>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

#include "editing/SculptManager.h"

class FileManager {
public:
    // Import: determines format by extension, returns a vector of loaded meshes
    static std::vector<Mesh*> importFiles(const std::string& path,
                                          Scene* scene = nullptr,
                                          AngleRenderer* renderer = nullptr,
                                          SculptManager* sculpt = nullptr,
                                          uint64_t* outWorkTime = nullptr);

    // Export: determines format by extension
    static bool exportMeshes(const std::string& path,
                             const std::vector<Mesh*>& meshes,
                             const Scene* scene = nullptr,
                             const AngleRenderer* renderer = nullptr,
                             const SculptManager* sculpt = nullptr,
                             const std::vector<uint8_t>& thumbnail = {},
                             bool savePngNextToProject = false,
                             uint64_t workTimeSeconds = 0);

    static std::string getExtension(const std::string& path);
};
