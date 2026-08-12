#pragma once
#include <cstdint>

int strokeFlatten(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float ax, float ay, float az,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative, bool accumulate, bool lockPosition,
    float focalShift, bool focalShiftFalloff,
    // Alpha params
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokeSmooth(
    float* verts,
    const float* normals,
    const float* materials,
    const uint32_t* vrvStartCount,
    const uint32_t* vertRingVert,
    const uint8_t* vertOnEdge,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool tangent,
    float focalShift, bool focalShiftFalloff,
    // Alpha params
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

uint32_t getFacesFromVerticesFast(
    const uint32_t* iVerts, uint32_t nbIVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    uint32_t* outIFaces,
    uint32_t* tagFlags,
    uint32_t* tagEpoch,
    uint32_t nbFaces
);

bool computeAreaNormalAndCenter(
    const float* verts,
    const float* normals,
    const float* materials,
    const uint32_t* iVerts, int nbIVerts,
    float* outResults
);

int strokeInflate(
    float* verts,
    const float* vertProxy,
    const float* materials,
    const float* normals,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokePinch(
    float* verts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeCrease(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeVTool(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeMove(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeDrag(
    float* verts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeElastic(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius, float elasticity,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeMask(
    float* verts,
    float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity, float hardness,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokePaint(
    float* verts,
    float* colors,
    float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity, float hardness,
    float cr, float cg, float cb,
    float roughness, float metallic,
    bool writeAlbedo, bool writeRoughness, bool writeMetalness,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokePaintAll(
    float* colors,
    float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cr, float cg, float cb,
    float roughness, float metallic,
    bool writeAlbedo, bool writeRoughness, bool writeMetalness
);

int strokeLocalScale(
    float* verts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokeTwist(
    float* verts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float nx, float ny, float nz,
    float radius, float angle,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokeDamStandard(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);

int strokeSquareBrush(
    float* verts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float ax, float ay, float az,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

int strokeBrush(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float ax, float ay, float az,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative, bool clay,
    float focalShift, bool focalShiftFalloff,
    int stampType, int stampSides, float stampInnerRatio, float stampAngle, float stampBlur,
    const float* alphaLookAt, bool alphaXSym
);

int strokeDeleteLayer(
    const float* baseVerts,
    float* layerDeltaVerts,
    float* finalVerts,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    float layerIntensity,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);



int blurMask(
    const uint32_t* iVerts, int nbIVerts,
    const uint32_t* vrvStartCount,
    const uint32_t* vertRingVert,
    const uint8_t* vertOnEdge,
    int iterations,
    float* tempMasks,
    int stride = 1,
    int offset = 0
);

int applyGradientMask(
    const float* verts,
    float* materials,
    const uint32_t* activeVerts, int nbActiveVerts,
    const float* origMasks,
    const float* blurredMasks,
    const float* localToScreen,
    float height,
    float ax, float ay, float bx, float by,
    bool symmetry,
    float ptPlaneX, float ptPlaneY, float ptPlaneZ,
    float nPlaneX, float nPlaneY, float nPlaneZ,
    bool blurMaskedOnly,
    int totalNbVerts
);
