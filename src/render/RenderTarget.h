#pragma once

#include <cstddef>
#include <GLES3/gl3.h>

struct RenderTarget {
    GLuint fbo = 0;
    GLuint texture = 0;
    GLuint depth = 0; // Renderbuffer (shared or owned)
    float invW = 0.0f;
    float invH = 0.0f;
    int width = 0;
    int height = 0;
    int bytesPerColorPixel = 0;
    int bytesPerDepthPixel = 0;
    bool ownsDepth = true;
    bool depthAsTexture = false;

    bool init(int w, int h, bool hasDepth = true, GLuint sharedDepth = 0, bool depthAsTexture = false);
    bool initDepthOnly(int w, int h);
    bool initFloat(int w, int h, bool hasDepth = false);
    void resize(int w, int h);
    void release();
    size_t getGpuMemoryBytes() const {
        if (width <= 0 || height <= 0) return 0;
        return static_cast<size_t>(width) * static_cast<size_t>(height) *
               static_cast<size_t>(bytesPerColorPixel + bytesPerDepthPixel);
    }
};
