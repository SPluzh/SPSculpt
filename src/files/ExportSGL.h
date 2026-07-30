#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"

#include "editing/SculptManager.h"

namespace ExportSGL {

std::vector<uint8_t> exportSGL(const std::vector<Mesh*>& meshes, const Scene& scene, const AngleRenderer& renderer, const SculptManager& sculpt);

} // namespace ExportSGL
