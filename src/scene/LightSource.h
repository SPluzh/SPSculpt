#pragma once

#include <glm/glm.hpp>
#include <string>

enum class LightType {
    DIRECTIONAL = 0,
    POINT = 1,
    SPOT = 2
};

struct LightSource {
    LightType   type        = LightType::DIRECTIONAL;
    glm::vec3   position    = {0.f, 5.f, 5.f};
    glm::vec3   direction   = glm::normalize(glm::vec3(-0.5f, -0.8f, -1.f));
    glm::vec3   color       = {1.f, 1.f, 1.f};
    float       intensity   = 1.f;
    float       range       = 50.f;        // point/spot range
    float       innerAngle  = 15.f;        // spot inner angle (degrees)
    float       outerAngle  = 30.f;        // spot outer angle (degrees)
    bool        castShadow  = true;
    bool        enabled     = true;
    std::string name        = "Light";
};
