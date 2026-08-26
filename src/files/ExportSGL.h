#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

#include "editing/SculptManager.h"

namespace ExportSGL {

std::vector<uint8_t> exportSGL(const std::vector<Mesh*>& meshes, const Scene& scene, const AngleRenderer& renderer, const SculptManager& sculpt, const std::vector<uint8_t>& thumbnail = {}, uint64_t workTimeSeconds = 0);

} // namespace ExportSGL
