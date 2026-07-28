#pragma once
#include <string>

class AngleRenderer;
class Scene;
class IniFile;

namespace RenderSettings {
    bool save(IniFile& ini, const AngleRenderer& renderer, const Scene& scene);
    bool load(const IniFile& ini, AngleRenderer& renderer, Scene& scene);
}

