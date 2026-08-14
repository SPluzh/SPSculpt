#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <GLES3/gl3.h>

struct ReferenceImage {
    std::string path;
    GLuint texId = 0;
    int    width = 0;
    int    height = 0;
    float  opacity = 1.0f;
    float  scale   = 1.0f;
    float  offsetX = 0.0f;
    float  offsetY = 0.0f;
    float  rotation = 0.0f; // degrees
    bool   visible = true;
    bool   visibleV1 = true;
    bool   visibleV2 = true;
    bool   pinned2D = true; // true=overlay, false=3D plane
    bool   locked = false;  // lock transformation/editing
    std::vector<uint8_t> embeddedData;
};

GLuint loadTextureFromFile(const std::string& path, int* outWidth = nullptr, int* outHeight = nullptr);
GLuint loadTextureFromMemory(const uint8_t* buffer, size_t size, int* outWidth = nullptr, int* outHeight = nullptr);

