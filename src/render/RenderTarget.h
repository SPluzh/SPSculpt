#pragma once

#include <GLES3/gl3.h>

struct RenderTarget {
    GLuint fbo = 0;
    GLuint texture = 0;
    GLuint depth = 0; // Renderbuffer (shared or owned)
    float invW = 0.0f;
    float invH = 0.0f;
    bool ownsDepth = true;
    bool depthAsTexture = false;

    bool init(int w, int h, bool hasDepth = true, GLuint sharedDepth = 0, bool depthAsTexture = false);
    void resize(int w, int h);
    void release();
};
