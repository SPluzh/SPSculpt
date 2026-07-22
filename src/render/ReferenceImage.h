#pragma once
#include <string>
#include <GLES3/gl3.h>

struct ReferenceImage {
    std::string path;
    GLuint texId = 0;
    float  opacity = 0.5f;
    float  scale   = 1.0f;
    float  offsetX = 0.0f;
    float  offsetY = 0.0f;
    bool   visible = true;
    bool   pinned2D = true; // true=overlay, false=3D plane
};

GLuint loadTextureFromFile(const std::string& path);
