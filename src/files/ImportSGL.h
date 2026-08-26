#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

#include "editing/SculptManager.h"

namespace ImportSGL {

struct ProjectMetadata {
    std::vector<uint8_t> thumbnailPng;
    uint64_t             workTime = 0;
};

std::vector<Mesh*> importSGL(const std::vector<uint8_t>& buffer, Scene& scene, AngleRenderer& renderer, SculptManager* sculpt = nullptr, uint64_t* outWorkTime = nullptr);
ProjectMetadata extractProjectMetadata(const std::vector<uint8_t>& buffer);
ProjectMetadata extractProjectMetadata(const std::string& path);
std::vector<uint8_t> extractThumbnail(const std::vector<uint8_t>& buffer);
uint64_t extractWorkTime(const std::vector<uint8_t>& buffer);
uint64_t extractWorkTime(const std::string& path);

} // namespace ImportSGL
