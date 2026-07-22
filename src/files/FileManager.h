#pragma once
#include <vector>
#include <string>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

class FileManager {
public:
    // Import: determines format by extension, returns a vector of loaded meshes
    static std::vector<Mesh*> importFiles(const std::string& path,
                                          Scene* scene = nullptr,
                                          AngleRenderer* renderer = nullptr);

    // Export: determines format by extension
    static bool exportMeshes(const std::string& path,
                             const std::vector<Mesh*>& meshes,
                             const Scene* scene = nullptr,
                             const AngleRenderer* renderer = nullptr);

    static std::string getExtension(const std::string& path);
};
