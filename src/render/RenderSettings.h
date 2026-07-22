#pragma once
#include <string>

class AngleRenderer;
class Scene;

namespace RenderSettings {
    bool save(const std::string& filepath, const AngleRenderer& renderer, const Scene& scene);
    bool load(const std::string& filepath, AngleRenderer& renderer, Scene& scene);
}
