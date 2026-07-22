#pragma once
#include <cstdint>

int strokeFlatten(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float ax, float ay, float az,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative, bool accumulate, bool lockPosition,
    float focalShift, bool focalShiftFalloff,
    // Alpha params
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

int strokeSmooth(
    uintptr_t vertsPtr,
    uintptr_t normalsPtr,
    uintptr_t materialsPtr,
    uintptr_t vrvStartCountPtr,
    uintptr_t vertRingVertPtr,
    uintptr_t vertOnEdgePtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool tangent,
    float focalShift, bool focalShiftFalloff,
    // Alpha params
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

uint32_t getFacesFromVerticesFast(
    uintptr_t iVertsPtr,    uint32_t nbIVerts,
    uintptr_t vrfStartCountPtr,
    uintptr_t vertRingFacePtr,
    uintptr_t outIFacesPtr,
    uintptr_t tagFlagsPtr,
    uintptr_t tagEpochPtr,
    uint32_t nbFaces
);

bool computeAreaNormalAndCenter(
    uintptr_t vertsPtr,
    uintptr_t normalsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    uintptr_t outResultsPtr
);

int strokeInflate(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t normalsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokePinch(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeCrease(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeVTool(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeMove(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeDrag(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeElastic(
    uintptr_t vertsPtr,
    uintptr_t vertProxyPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius, float elasticity,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym,
    bool useAccuCurve, uintptr_t accuCurveLutPtr
);

int strokeMask(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity, float hardness,
    bool negative,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

int strokePaint(
    uintptr_t vertsPtr,
    uintptr_t colorsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity, float hardness,
    float cr, float cg, float cb,
    float roughness, float metallic,
    bool writeAlbedo, bool writeRoughness, bool writeMetalness,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

int strokePaintAll(
    uintptr_t colorsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cr, float cg, float cb,
    float roughness, float metallic,
    bool writeAlbedo, bool writeRoughness, bool writeMetalness
);

int strokeLocalScale(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

int strokeTwist(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t iVertsPtr, int nbIVerts,
    float cx, float cy, float cz,
    float nx, float ny, float nz,
    float radius, float angle,
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, uintptr_t alphaTexPtr, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    uintptr_t alphaLookAtPtr, bool alphaXSym
);

int blurMask(
    uintptr_t iVertsPtr, int nbIVerts,
    uintptr_t vrvStartCountPtr,
    uintptr_t vertRingVertPtr,
    uintptr_t vertOnEdgePtr,
    int iterations,
    uintptr_t tempMasksPtr
);

int applyGradientMask(
    uintptr_t vertsPtr,
    uintptr_t materialsPtr,
    uintptr_t activeVertsPtr, int nbActiveVerts,
    uintptr_t origMasksPtr,
    uintptr_t blurredMasksPtr,
    uintptr_t localToScreenPtr,
    float height,
    float ax, float ay, float bx, float by,
    bool symmetry,
    float ptPlaneX, float ptPlaneY, float ptPlaneZ,
    float nPlaneX, float nPlaneY, float nPlaneZ,
    bool blurMaskedOnly,
    int totalNbVerts
);


