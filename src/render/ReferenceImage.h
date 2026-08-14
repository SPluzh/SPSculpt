#pragma once
#include <string>
#include <GLES3/gl3.h>

struct ReferenceImage {
    std::string path;
    GLuint texId = 0;
    int    width = 0;
    int    height = 0;
    float  opacity = 0.5f;
    float  scale   = 1.0f;
    float  offsetX = 0.0f;
    float  offsetY = 0.0f;
    float  rotation = 0.0f; // degrees
    bool   visible = true;
    bool   visibleV1 = true;
    bool   visibleV2 = true;
    bool   pinned2D = true; // true=overlay, false=3D plane
};

GLuint loadTextureFromFile(const std::string& path, int* outWidth = nullptr, int* outHeight = nullptr);
