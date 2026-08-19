#pragma once
#include "scene/Camera.h"
#include <string>
#include <vector>
#include <GLES3/gl3.h>

struct RefImageSnapshot {
    std::string path;
    bool visible   = true;
    bool visibleV1 = true;
    bool visibleV2 = true;
    float offsetX  = 0.0f;
    float offsetY  = 0.0f;
    float scale    = 1.0f;
    float rotation = 0.0f;
    float opacity  = 1.0f;
    bool  locked   = false;
};

struct MeshFaceHideSnapshot {
    uint32_t meshId = 0;
    std::string meshName;
    bool visibleV1 = true;
    bool visibleV2 = true;
    std::vector<uint8_t> faceVisible;
};

struct CameraBookmark {
    std::string          name;
    Camera::CameraState  camState;
    Camera::CameraState  camStateRight;
    bool                 hasRightCam = false;
    std::vector<RefImageSnapshot> refImages;
    std::vector<MeshFaceHideSnapshot> meshHideSnapshots;

    GLuint previewTexId = 0;
    int    previewW     = 128;
    int    previewH     = 128;
    std::string previewPath;
    std::vector<uint8_t> previewData;
};

