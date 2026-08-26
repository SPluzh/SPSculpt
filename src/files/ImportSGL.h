#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

#include "editing/SculptManager.h"

namespace ImportSGL {

std::vector<Mesh*> importSGL(const std::vector<uint8_t>& buffer, Scene& scene, AngleRenderer& renderer, SculptManager* sculpt = nullptr, uint64_t* outWorkTime = nullptr);
std::vector<uint8_t> extractThumbnail(const std::vector<uint8_t>& buffer);
uint64_t extractWorkTime(const std::vector<uint8_t>& buffer);
uint64_t extractWorkTime(const std::string& path);

} // namespace ImportSGL
