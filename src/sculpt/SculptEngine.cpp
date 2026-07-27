#include "sculpt/SculptEngine.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <chrono>

static void radixSort(uint32_t* arr, uint32_t n) {
    if (n < 2) return;
    std::vector<uint32_t> temp(n);
    
    // Pass 1: bits 0-7
    uint32_t count0[256] = {0};
    for (uint32_t i = 0; i < n; ++i) {
        count0[arr[i] & 0xFF]++;
    }
    uint32_t pref0[256];
    pref0[0] = 0;
    for (int i = 1; i < 256; ++i) {
        pref0[i] = pref0[i - 1] + count0[i - 1];
    }
    for (uint32_t i = 0; i < n; ++i) {
        temp[pref0[arr[i] & 0xFF]++] = arr[i];
    }

    // Pass 2: bits 8-15
    uint32_t count1[256] = {0};
    for (uint32_t i = 0; i < n; ++i) {
        count1[(temp[i] >> 8) & 0xFF]++;
    }
    uint32_t pref1[256];
    pref1[0] = 0;
    for (int i = 1; i < 256; ++i) {
        pref1[i] = pref1[i - 1] + count1[i - 1];
    }
    for (uint32_t i = 0; i < n; ++i) {
        arr[pref1[(temp[i] >> 8) & 0xFF]++] = temp[i];
    }

    // Pass 3: bits 16-23
    uint32_t count2[256] = {0};
    for (uint32_t i = 0; i < n; ++i) {
        count2[(arr[i] >> 16) & 0xFF]++;
    }
    uint32_t pref2[256];
    pref2[0] = 0;
    for (int i = 1; i < 256; ++i) {
        pref2[i] = pref2[i - 1] + count2[i - 1];
    }
    for (uint32_t i = 0; i < n; ++i) {
        temp[pref2[(arr[i] >> 16) & 0xFF]++] = arr[i];
    }
    
    std::copy(temp.begin(), temp.end(), arr);
}

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
) {

    float comp = negative ? -1.0f : 1.0f;
    float p = (1.0f - focalShift) / 2.0f;
    const float pSq = p * p;
    const float radiusSq = radius * radius;

    const float* vProxy = (accumulate || lockPosition) ? verts : vertProxy;

    #pragma omp parallel for schedule(static) if(nbIVerts > 1000)
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float distToPlane = (vx - ax) * anx + (vy - ay) * any + (vz - az) * anz;
        if (distToPlane * comp > 0.0f) {
            continue;
        }

        float dx = vProxy[ind] - cx;
        float dy = vProxy[ind + 1] - cy;
        float dz = vProxy[ind + 2] - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        // Falloff
        float fallOff = 1.0f;
        if (distSq < pSq) {
            fallOff = 1.0f;
        } else if (p >= 1.0f) {
            fallOff = 0.0f;
        } else {
            float dist = std::sqrt(distSq);
            float d = (dist - p) / (1.0f - p);
            float d2 = d * d;
            fallOff = 3.0f * d2 * d2 - 4.0f * d2 * d + 1.0f;
        }

        // Alpha map value
        float alphaVal = 1.0f;
        if (hasAlpha && alphaTex && alphaLookAt) {
            float xn = alphaRatioY * (alphaLookAt[0] * vx + alphaLookAt[4] * vy + alphaLookAt[8] * vz + alphaLookAt[12]) / (alphaXSym ? -alphaSide : alphaSide);
            float yn = alphaRatioX * (alphaLookAt[1] * vx + alphaLookAt[5] * vy + alphaLookAt[9] * vz + alphaLookAt[13]) / alphaSide;

            float edgeDist = std::sqrt(xn * xn + yn * yn);
            if (edgeDist > 1.0f) {
                alphaVal = 0.0f;
            } else {
                const uint8_t* tex = alphaTex;
                int txn = std::max(0, std::min(alphaWidth - 1, static_cast<int>((0.5f - xn * 0.5f) * alphaWidth)));
                int tyn = std::max(0, std::min(alphaHeight - 1, static_cast<int>((0.5f - yn * 0.5f) * alphaHeight)));
                alphaVal = tex[txn + alphaWidth * tyn] / 255.0f;

                float fs = focalShiftFalloff ? focalShift : 0.0f;
                if (fs != 0.0f) {
                    if (fs > 0.0f) {
                        float softRadius = (1.0f - fs) / 2.0f;
                        if (edgeDist > softRadius) {
                            float edgeFade = 1.0f - (edgeDist - softRadius) / (1.0f - softRadius);
                            edgeFade = std::max(0.0f, std::min(1.0f, edgeFade));
                            float smooth = edgeFade * edgeFade * (3.0f - 2.0f * edgeFade);
                            alphaVal *= std::pow(smooth, 1.0f + fs * 5.0f);
                        }
                    } else {
                        alphaVal = std::pow(alphaVal, 1.0f / (1.0f - fs * 3.0f));
                    }
                }
            }
        }

        fallOff *= distToPlane * intensity * matVal * alphaVal;
        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] -= anx * fallOff;
        verts[ind + 1] -= any * fallOff;
        verts[ind + 2] -= anz * fallOff;
    }
    return nbIVerts;
}

static void laplacianSmooth(
    const float* __restrict verts,
    const uint32_t* __restrict vrvStartCount,
    const uint32_t* __restrict vertRingVert,
    const uint8_t* __restrict vertOnEdge,
    const uint32_t* __restrict iVerts, int nbIVerts,
    float* __restrict outSmoothVerts
) {
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int i3 = i * 3;

        // Prefetch next vertex info
        if (i + 1 < nbIVerts) {
            uint32_t nextId = iVerts[i + 1];
            __builtin_prefetch(&vrvStartCount[nextId * 2], 0, 1);
            __builtin_prefetch(&vertOnEdge[nextId], 0, 1);
        }

        uint32_t start = vrvStartCount[id * 2];
        uint32_t count = vrvStartCount[id * 2 + 1];

        if (count <= 2) {
            uint32_t idv = id * 3;
            outSmoothVerts[i3] = verts[idv];
            outSmoothVerts[i3 + 1] = verts[idv + 1];
            outSmoothVerts[i3 + 2] = verts[idv + 2];
            continue;
        }

        float avx = 0.0f;
        float avy = 0.0f;
        float avz = 0.0f;

        if (vertOnEdge[id] == 1) {
            int nbVertEdge = 0;
            for (uint32_t j = start; j < start + count; ++j) {
                if (j + 1 < start + count) {
                    __builtin_prefetch(&verts[vertRingVert[j + 1] * 3], 0, 1);
                    __builtin_prefetch(&vertOnEdge[vertRingVert[j + 1]], 0, 1);
                }
                uint32_t idv = vertRingVert[j];
                if (vertOnEdge[idv] == 1) {
                    uint32_t idv3 = idv * 3;
                    avx += verts[idv3];
                    avy += verts[idv3 + 1];
                    avz += verts[idv3 + 2];
                    ++nbVertEdge;
                }
            }

            if (nbVertEdge >= 2) {
                outSmoothVerts[i3] = avx / nbVertEdge;
                outSmoothVerts[i3 + 1] = avy / nbVertEdge;
                outSmoothVerts[i3 + 2] = avz / nbVertEdge;
                continue;
            }
            avx = avy = avz = 0.0f;
        }

        for (uint32_t j = start; j < start + count; ++j) {
            if (j + 1 < start + count) {
                __builtin_prefetch(&verts[vertRingVert[j + 1] * 3], 0, 1);
            }
            uint32_t idv3 = vertRingVert[j] * 3;
            avx += verts[idv3];
            avy += verts[idv3 + 1];
            avz += verts[idv3 + 2];
        }

        outSmoothVerts[i3] = avx / count;
        outSmoothVerts[i3 + 1] = avy / count;
        outSmoothVerts[i3 + 2] = avz / count;
    }
}

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
) {

    static std::vector<float> smoothVerts;
    if (smoothVerts.size() < static_cast<size_t>(nbIVerts * 3)) {
        smoothVerts.resize(nbIVerts * 3);
    }
    laplacianSmooth(verts, vrvStartCount, vertRingVert, vertOnEdge, iVerts, nbIVerts, smoothVerts.data());

    float p = (1.0f - focalShift) / 2.0f;
    const float pSq = p * p;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;

        float mIntensity = intensity * materials[ind + 2];
        if (mIntensity <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        if (radius > 0.0f) {
            float dx = vx - cx;
            float dy = vy - cy;
            float dz = vz - cz;
            float distSq = (dx * dx + dy * dy + dz * dz) / (radius * radius);
            if (distSq >= 1.0f) {
                continue;
            }
            if (distSq < pSq) {
                // fallOff = 1.0f, mIntensity is unchanged
            } else if (p >= 1.0f) {
                continue;
            } else {
                float dist = std::sqrt(distSq);
                float d = (dist - p) / (1.0f - p);
                float d2 = d * d;
                float fallOff = 3.0f * d2 * d2 - 4.0f * d2 * d + 1.0f;
                mIntensity *= fallOff;
            }
        }

        // Alpha map value
        float alphaVal = 1.0f;
        if (hasAlpha && alphaTex && alphaLookAt) {
            float xn = alphaRatioY * (alphaLookAt[0] * vx + alphaLookAt[4] * vy + alphaLookAt[8] * vz + alphaLookAt[12]) / (alphaXSym ? -alphaSide : alphaSide);
            float yn = alphaRatioX * (alphaLookAt[1] * vx + alphaLookAt[5] * vy + alphaLookAt[9] * vz + alphaLookAt[13]) / alphaSide;

            float edgeDist = std::sqrt(xn * xn + yn * yn);
            if (edgeDist > 1.0f) {
                alphaVal = 0.0f;
            } else {
                const uint8_t* tex = alphaTex;
                int txn = std::max(0, std::min(alphaWidth - 1, static_cast<int>((0.5f - xn * 0.5f) * alphaWidth)));
                int tyn = std::max(0, std::min(alphaHeight - 1, static_cast<int>((0.5f - yn * 0.5f) * alphaHeight)));
                alphaVal = tex[txn + alphaWidth * tyn] / 255.0f;

                float fs = focalShiftFalloff ? focalShift : 0.0f;
                if (fs != 0.0f) {
                    if (fs > 0.0f) {
                        float softRadius = (1.0f - fs) / 2.0f;
                        if (edgeDist > softRadius) {
                            float edgeFade = 1.0f - (edgeDist - softRadius) / (1.0f - softRadius);
                            edgeFade = std::max(0.0f, std::min(1.0f, edgeFade));
                            float smooth = edgeFade * edgeFade * (3.0f - 2.0f * edgeFade);
                            alphaVal *= std::pow(smooth, 1.0f + fs * 5.0f);
                        }
                    } else {
                        alphaVal = std::pow(alphaVal, 1.0f / (1.0f - fs * 3.0f));
                    }
                }
            }
        }

        mIntensity *= alphaVal;

        if (mIntensity <= 0.0f) {
            continue;
        }

        int i3 = i * 3;
        float smx = smoothVerts[i3];
        float smy = smoothVerts[i3 + 1];
        float smz = smoothVerts[i3 + 2];

        float diffX = smx - vx;
        float diffY = smy - vy;
        float diffZ = smz - vz;
        if (diffX * diffX + diffY * diffY + diffZ * diffZ < 1e-12f) {
            continue;
        }

        if (tangent) {
            float nx = normals[ind];
            float ny = normals[ind + 1];
            float nz = normals[ind + 2];
            float len = nx * nx + ny * ny + nz * nz;
            if (len == 0.0f) {
                continue;
            }
            len = 1.0f / std::sqrt(len);
            nx *= len;
            ny *= len;
            nz *= len;

            float dot = nx * diffX + ny * diffY + nz * diffZ;
            verts[ind] = vx + (smx - nx * dot - vx) * mIntensity;
            verts[ind + 1] = vy + (smy - ny * dot - vy) * mIntensity;
            verts[ind + 2] = vz + (smz - nz * dot - vz) * mIntensity;
        } else {
            float intComp = 1.0f - mIntensity;
            verts[ind] = vx * intComp + smx * mIntensity;
            verts[ind + 1] = vy * intComp + smy * mIntensity;
            verts[ind + 2] = vz * intComp + smz * mIntensity;
        }

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

uint32_t getFacesFromVerticesFast(
    const uint32_t* iVerts, uint32_t nbIVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    uint32_t* outIFaces,
    uint32_t* tagFlags,
    uint32_t* tagEpoch,
    uint32_t nbFaces
) {

    if (!tagEpoch || !tagFlags || !iVerts || !vrfStartCount || !vertRingFace || !outIFaces) {
        return 0;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    uint32_t epoch = *tagEpoch + 1;
    if (epoch == 0) {
        std::fill(tagFlags, tagFlags + nbFaces, 0);
        epoch = 1;
    }
    *tagEpoch = epoch;

    uint32_t acc = 0;
    for (uint32_t i = 0; i < nbIVerts; ++i) {
        if (i + 4 < nbIVerts) {
            uint32_t nextId = iVerts[i + 4];
            __builtin_prefetch(&vrfStartCount[nextId * 2], 0, 2);
        }

        uint32_t idVert = iVerts[i];
        uint32_t start = vrfStartCount[idVert * 2];
        uint32_t count = vrfStartCount[idVert * 2 + 1];
        for (uint32_t j = start; j < start + count; ++j) {
            if (j + 4 < start + count) {
                __builtin_prefetch(&tagFlags[vertRingFace[j + 4]], 1, 2);
            }
            uint32_t iFace = vertRingFace[j];
            if (tagFlags[iFace] != epoch) {
                tagFlags[iFace] = epoch;
                outIFaces[acc++] = iFace;
            }
        }
    }

    auto t2 = std::chrono::high_resolution_clock::now();
    double loop_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
    if (loop_ms > 0.5) {
        printf("[C++ getFacesFromVerticesFast] %u verts | loop took %.2fms\n", nbIVerts, loop_ms);
    }

    return acc;
}

bool computeAreaNormalAndCenter(
    const float* verts,
    const float* normals,
    const float* materials,
    const uint32_t* iVerts, int nbIVerts,
    float* outResults
) {
    auto t0 = std::chrono::high_resolution_clock::now();

    if (nbIVerts == 0 || !verts || !normals || !materials || !iVerts || !outResults) {
        if (outResults) {
            outResults[6] = 0.0f; // invalid
        }
        return false;
    }

    float anx = 0.0f;
    float any = 0.0f;
    float anz = 0.0f;

    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float acc = 0.0f;

    #pragma omp parallel for reduction(+:anx,any,anz,ax,ay,az,acc) if(nbIVerts > 512)
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;
        float f = materials[ind + 2];
        
        anx += normals[ind] * f;
        any += normals[ind + 1] * f;
        anz += normals[ind + 2] * f;

        acc += f;
        ax += verts[ind] * f;
        ay += verts[ind + 1] * f;
        az += verts[ind + 2] * f;
    }

    float len = std::sqrt(anx * anx + any * any + anz * anz);
    if (len == 0.0f || acc == 0.0f) {
        outResults[6] = 0.0f; // invalid
        return false;
    }

    len = 1.0f / len;
    outResults[0] = anx * len;
    outResults[1] = any * len;
    outResults[2] = anz * len;

    outResults[3] = ax / acc;
    outResults[4] = ay / acc;
    outResults[5] = az / acc;

    outResults[6] = 1.0f; // valid

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms > 0.2) {
        printf("[C++ computeAreaNormalAndCenter] %d verts took: %.2fms\n", nbIVerts, ms);
    }
    return true;
}

inline float getFallOff(float dist, float focalShift, bool useAccuCurve, const float* accuCurveLut) {
    if (useAccuCurve && accuCurveLut) {
        if (dist >= 1.0f) return 0.0f;
        float floatIndex = dist * 255.0f;
        if (floatIndex > 254.0f) floatIndex = 254.0f;
        int index = static_cast<int>(floatIndex);
        float fract = floatIndex - index;
        return accuCurveLut[index] * (1.0f - fract) + accuCurveLut[index + 1] * fract;
    }
    float p = (1.0f - focalShift) / 2.0f;
    if (dist < p) return 1.0f;
    if (p >= 1.0f) return 0.0f;
    float d = (dist - p) / (1.0f - p);
    float fallOff = d * d;
    return 3.0f * fallOff * fallOff - 4.0f * fallOff * d + 1.0f;
}

inline float getAlphaVal(
    float vx, float vy, float vz,
    bool hasAlpha, const uint8_t* tex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    float focalShift, bool focalShiftFalloff
) {
    if (!hasAlpha || !tex || !alphaLookAt) return 1.0f;
    float xn = alphaRatioY * (alphaLookAt[0] * vx + alphaLookAt[4] * vy + alphaLookAt[8] * vz + alphaLookAt[12]) / (alphaXSym ? -alphaSide : alphaSide);
    float yn = alphaRatioX * (alphaLookAt[1] * vx + alphaLookAt[5] * vy + alphaLookAt[9] * vz + alphaLookAt[13]) / alphaSide;

    float edgeDist = std::sqrt(xn * xn + yn * yn);
    if (edgeDist > 1.0f) {
        return 0.0f;
    }
    int txn = std::max(0, std::min(alphaWidth - 1, static_cast<int>((0.5f - xn * 0.5f) * alphaWidth)));
    int tyn = std::max(0, std::min(alphaHeight - 1, static_cast<int>((0.5f - yn * 0.5f) * alphaHeight)));
    float alphaVal = tex[txn + alphaWidth * tyn] / 255.0f;

    float fs = focalShiftFalloff ? focalShift : 0.0f;
    if (fs != 0.0f) {
        if (fs > 0.0f) {
            float softRadius = (1.0f - fs) / 2.0f;
            if (edgeDist > softRadius) {
                float edgeFade = 1.0f - (edgeDist - softRadius) / (1.0f - softRadius);
                edgeFade = std::max(0.0f, std::min(1.0f, edgeFade));
                float smooth = edgeFade * edgeFade * (3.0f - 2.0f * edgeFade);
                alphaVal *= std::pow(smooth, 1.0f + fs * 5.0f);
            }
        } else {
            alphaVal = std::pow(alphaVal, 1.0f / (1.0f - fs * 3.0f));
        }
    }
    return alphaVal;
}

inline float getCreaseFallOff(float dist, float focalShift, bool useAccuCurve, const float* accuCurveLut) {
    if (useAccuCurve && accuCurveLut) {
        if (dist >= 1.0f) return 0.0f;
        float floatIndex = dist * 255.0f;
        if (floatIndex > 254.0f) floatIndex = 254.0f;
        int index = static_cast<int>(floatIndex);
        float fract = floatIndex - index;
        return accuCurveLut[index] * (1.0f - fract) + accuCurveLut[index + 1] * fract;
    }
    if (dist >= 1.0f) return 0.0f;
    float base = 1.0f - dist;
    float exponent = std::pow(2.0f, focalShift * 2.0f) * 2.0f;
    return base * (1.0f - std::pow(dist, exponent));
}

inline float getMoveFallOff(float dist, float focalShift, bool useAccuCurve, const float* accuCurveLut) {
    if (useAccuCurve && accuCurveLut) {
        if (dist >= 1.0f) return 0.0f;
        float floatIndex = dist * 255.0f;
        if (floatIndex > 254.0f) floatIndex = 254.0f;
        int index = static_cast<int>(floatIndex);
        float fract = floatIndex - index;
        return accuCurveLut[index] * (1.0f - fract) + accuCurveLut[index + 1] * fract;
    }
    if (dist >= 1.0f) return 0.0f;
    float base = 1.0f - dist * dist;
    float exponent = std::pow(2.0f, (focalShift - 0.6f) * 2.0f) * 3.0f;
    return std::pow(base, exponent);
}

inline float getElasticFallOff(float dist, float focalShift, bool useAccuCurve, const float* accuCurveLut, float elasticity) {
    float baseFalloff = getFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
    return std::pow(baseFalloff, elasticity);
}

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
) {

    float deformIntensity = intensity * radius * 0.1f;
    if (negative) {
        deformIntensity = -deformIntensity;
    }
    const float radiusSq = radius * radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
            __builtin_prefetch(&vertProxy[nextId * 3], 0, 1);
            __builtin_prefetch(&normals[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float dx = vertProxy[ind] - cx;
        float dy = vertProxy[ind + 1] - cy;
        float dz = vertProxy[ind + 2] - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
        fallOff = deformIntensity * fallOff;

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float nx = normals[ind];
        float ny = normals[ind + 1];
        float nz = normals[ind + 2];

        float normalLen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (normalLen == 0.0f) {
            continue;
        }

        fallOff /= normalLen;
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] += nx * fallOff;
        verts[ind + 1] += ny * fallOff;
        verts[ind + 2] += nz * fallOff;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {

    float deformIntensity = intensity * 0.05f;
    if (negative) {
        deformIntensity = -deformIntensity;
    }
    const float radiusSq = radius * radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = cx - vx;
        float dy = cy - vy;
        float dz = cz - vz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= deformIntensity * matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] = vx + dx * fallOff;
        verts[ind + 1] = vy + dy * fallOff;
        verts[ind + 2] = vz + dz * fallOff;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {

    float deformIntensity = intensity * 0.07f;
    float brushFactor = deformIntensity * radius;
    if (negative) {
        brushFactor = -brushFactor;
    }
    const float radiusSq = radius * radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
            __builtin_prefetch(&vertProxy[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = cx - vertProxy[ind];
        float dy = cy - vertProxy[ind + 1];
        float dz = cz - vertProxy[ind + 2];
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getCreaseFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        float brushModifier = std::pow(fallOff, 5.0f) * brushFactor;
        float actualFallOff = fallOff * deformIntensity;

        verts[ind] = vx + dx * actualFallOff + anx * brushModifier;
        verts[ind + 1] = vy + dy * actualFallOff + any * brushModifier;
        verts[ind + 2] = vz + dz * actualFallOff + anz * brushModifier;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {

    float deformIntensity = intensity * 0.08f;
    float brushFactor = deformIntensity * radius;
    if (negative) {
        brushFactor = -brushFactor;
    }
    const float radiusSq = radius * radius;

    float k = 1.0f; // 90 degree angle

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
            __builtin_prefetch(&vertProxy[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = cx - vertProxy[ind];
        float dy = cy - vertProxy[ind + 1];
        float dz = cz - vertProxy[ind + 2];
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = 1.0f - k * dist;
        if (fallOff < 0.0f) fallOff = 0.0f;
        if (useAccuCurve && accuCurveLut) {
            float floatIndex = dist * 255.0f;
            if (floatIndex > 254.0f) floatIndex = 254.0f;
            int index = static_cast<int>(floatIndex);
            float fract = floatIndex - index;
            fallOff = accuCurveLut[index] * (1.0f - fract) + accuCurveLut[index + 1] * fract;
        }
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        // Linear V-groove displacement with sharp center pinch
        float brushModifier = fallOff * brushFactor;
        float pinchFactor = std::pow(fallOff, 3.0f) * deformIntensity * 2.0f;

        verts[ind] = vx + dx * pinchFactor + anx * brushModifier;
        verts[ind + 1] = vy + dy * pinchFactor + any * brushModifier;
        verts[ind + 2] = vz + dz * pinchFactor + anz * brushModifier;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {
    const float radiusSq = radius * radius;
    #pragma omp parallel for schedule(static) if(nbIVerts > 1000)
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = vertProxy[ind];
        float vy = vertProxy[ind + 1];
        float vz = vertProxy[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getMoveFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] = vx + dirx * fallOff;
        verts[ind + 1] = vy + diry * fallOff;
        verts[ind + 2] = vz + dirz * fallOff;
    }
    return nbIVerts;
}

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
) {
    const float radiusSq = radius * radius;
    #pragma omp parallel for schedule(static) if(nbIVerts > 1000)
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getFallOff(dist, focalShift, useAccuCurve, accuCurveLut);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] = vx + dirx * fallOff;
        verts[ind + 1] = vy + diry * fallOff;
        verts[ind + 2] = vz + dirz * fallOff;
    }
    return nbIVerts;
}

int strokeElastic(
    float* verts,
    const float* vertProxy,
    const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius, float elasticity,
    float focalShift, bool focalShiftFalloff,
    // Alpha params
    bool hasAlpha, const uint8_t* alphaTex, int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
) {

    const float radiusSq = radius * radius;
    const float eps = radius;
    const float eps2 = radiusSq;

    float nu = 0.40f + 0.099f * (elasticity - 0.1f) / 2.9f;
    float a = 1.0f;
    float b = 3.0f - 4.0f * nu;
    float S = (1.5f * a - b) / eps;

    #pragma omp parallel for schedule(static) if(nbIVerts > 1000)
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = vertProxy[ind];
        float vy = vertProxy[ind + 1];
        float vz = vertProxy[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;

        float r2 = dx * dx + dy * dy + dz * dz;
        float r = std::sqrt(r2);
        float r_eps = std::sqrt(r2 + eps2);
        float r_eps3 = r_eps * r_eps * r_eps;

        float dot_r_f = dx * dirx + dy * diry + dz * dirz;

        float k1 = (a - b) / r_eps + (a * eps2) / (2.0f * r_eps3);
        float k2 = b / r_eps3;

        float ux = k1 * dirx + k2 * dot_r_f * dx;
        float uy = k1 * diry + k2 * dot_r_f * dy;
        float uz = k1 * dirz + k2 * dot_r_f * dz;

        float dist = r / radius;
        float fallOff = getElasticFallOff(dist, focalShift, useAccuCurve, accuCurveLut, elasticity);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        verts[ind] = vx + (ux / S) * fallOff;
        verts[ind + 1] = vy + (uy / S) * fallOff;
        verts[ind + 2] = vz + (uz / S) * fallOff;
    }
    return nbIVerts;
}

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
) {

    float softness = 2.0f * (1.0f - hardness);
    float maskIntensity = negative ? intensity : -intensity;
    const float radiusSq = radius * radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 0, 1);
            __builtin_prefetch(&materials[nextId * 3], 1, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = std::pow(1.0f - dist, softness);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= maskIntensity * alphaVal;

        float maskVal = materials[ind + 2] + fallOff;
        if (maskVal < 0.0f) maskVal = 0.0f;
        if (maskVal > 1.0f) maskVal = 1.0f;

        materials[ind + 2] = maskVal;
        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {

    float softness = 2.0f * (1.0f - hardness);
    const float radiusSq = radius * radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 0, 1);
            __builtin_prefetch(&colors[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 1, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = std::pow(1.0f - dist, softness);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= intensity * materials[ind + 2] * alphaVal;
        float fallOffCompl = 1.0f - fallOff;

        if (writeAlbedo) {
            colors[ind] = colors[ind] * fallOffCompl + cr * fallOff;
            colors[ind + 1] = colors[ind + 1] * fallOffCompl + cg * fallOff;
            colors[ind + 2] = colors[ind + 2] * fallOffCompl + cb * fallOff;
        }

        if (writeRoughness) {
            materials[ind] = materials[ind] * fallOffCompl + roughness * fallOff;
        }

        if (writeMetalness) {
            materials[ind + 1] = materials[ind + 1] * fallOffCompl + metallic * fallOff;
        }

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

int strokePaintAll(
    float* colors,
    float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cr, float cg, float cb,
    float roughness, float metallic,
    bool writeAlbedo, bool writeRoughness, bool writeMetalness
) {

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&colors[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 1, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float fallOff = materials[ind + 2];
        float fallOffCompl = 1.0f - fallOff;

        if (writeAlbedo) {
            colors[ind] = colors[ind] * fallOffCompl + cr * fallOff;
            colors[ind + 1] = colors[ind + 1] * fallOffCompl + cg * fallOff;
            colors[ind + 2] = colors[ind + 2] * fallOffCompl + cb * fallOff;
        }

        if (writeRoughness) {
            materials[ind] = materials[ind] * fallOffCompl + roughness * fallOff;
        }

        if (writeMetalness) {
            materials[ind + 1] = materials[ind + 1] * fallOffCompl + metallic * fallOff;
        }

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

inline void rotateVectorQuat(float& vx, float& vy, float& vz, float qx, float qy, float qz, float qw) {
    float ux = qy * vz - qz * vy;
    float uy = qz * vx - qx * vz;
    float uz = qx * vy - qy * vx;

    ux += qw * vx;
    uy += qw * vy;
    uz += qw * vz;

    vx += 2.0f * (qy * uz - qz * uy);
    vy += 2.0f * (qz * ux - qx * uz);
    vz += 2.0f * (qx * uy - qy * ux);
}

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
) {

    float deltaScale = intensity * 0.01f;
    const float radiusSq = radius * radius;
    if (radius <= 0.0f) return 0;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float fallOff = getFallOff(dist, focalShift, false, nullptr);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= deltaScale * matVal * alphaVal;

        verts[ind] = vx + dx * fallOff;
        verts[ind + 1] = vy + dy * fallOff;
        verts[ind + 2] = vz + dz * fallOff;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {

    const float radiusSq = radius * radius;
    if (radius <= 0.0f) return 0;
    const float invRadius = 1.0f / radius;

    int writeIdx = 0;
    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 1, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float matVal = materials[ind + 2];
        if (matVal <= 0.0f) {
            continue;
        }

        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float dx = vx - cx;
        float dy = vy - cy;
        float dz = vz - cz;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq >= radiusSq) {
            continue;
        }

        float dist = std::sqrt(distSq) * invRadius;
        float fallOff = getFallOff(dist, focalShift, false, nullptr);
        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= angle * matVal * alphaVal;

        if (fallOff == 0.0f) {
            continue;
        }

        float halfAngle = fallOff * 0.5f;
        float sinHalf = std::sin(halfAngle);
        float cosHalf = std::cos(halfAngle);
        float qx = nx * sinHalf;
        float qy = ny * sinHalf;
        float qz = nz * sinHalf;
        float qw = cosHalf;

        float rx = dx;
        float ry = dy;
        float rz = dz;
        rotateVectorQuat(rx, ry, rz, qx, qy, qz, qw);

        verts[ind] = cx + rx;
        verts[ind + 1] = cy + ry;
        verts[ind + 2] = cz + rz;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

int blurMask(
    const uint32_t* iVerts, int nbIVerts,
    const uint32_t* vrvStartCount,
    const uint32_t* vertRingVert,
    const uint8_t* vertOnEdge,
    int iterations,
    float* tempMasks,
    int stride,
    int offset
) {

    if (nbIVerts <= 0 || iterations <= 0 || !iVerts || !vrvStartCount || !vertRingVert || !tempMasks) return 0;

    std::vector<float> smoothMasks(nbIVerts);

    float origMin = 1.0f;
    for (int i = 0; i < nbIVerts; ++i) {
        uint32_t id = iVerts[i];
        float val = tempMasks[id * stride + offset];
        if (val < origMin) origMin = val;
    }

    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < nbIVerts; ++i) {
            uint32_t id = iVerts[i];
            uint32_t start = vrvStartCount[id * 2];
            uint32_t count = vrvStartCount[id * 2 + 1];

            if (count <= 2) {
                smoothMasks[i] = tempMasks[id * stride + offset];
                continue;
            }

            float sumMask = 0.0f;
            if (vertOnEdge && vertOnEdge[id] == 1) {
                int nbVertEdge = 0;
                for (uint32_t j = start; j < start + count; ++j) {
                    uint32_t idv = vertRingVert[j];
                    if (vertOnEdge[idv] == 1) {
                        sumMask += tempMasks[idv * stride + offset];
                        ++nbVertEdge;
                    }
                }

                if (nbVertEdge >= 2) {
                    smoothMasks[i] = sumMask / nbVertEdge;
                    continue;
                }
            }

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t idv = vertRingVert[j];
                sumMask += tempMasks[idv * stride + offset];
            }
            smoothMasks[i] = sumMask / count;
        }

        float iterMin = 1.0f;
        for (int i = 0; i < nbIVerts; ++i) {
            if (smoothMasks[i] < iterMin) iterMin = smoothMasks[i];
        }

        float scale = (iterMin < 1.0f && iterMin > origMin) ? ((1.0f - origMin) / (1.0f - iterMin)) : 1.0f;

        for (int i = 0; i < nbIVerts; ++i) {
            uint32_t id = iVerts[i];
            float val = smoothMasks[i];
            if (scale > 1.0f && val < 1.0f) {
                val = origMin + (val - iterMin) * scale;
                if (val < 0.0f) val = 0.0f;
                if (val > 1.0f) val = 1.0f;
            }
            tempMasks[id * stride + offset] = val;
        }
    }
    return nbIVerts;
}

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
) {
    const float* m = localToScreen;

    float m0 = m[0], m4 = m[4], m8 = m[8], m12 = m[12];
    float m1 = m[1], m5 = m[5], m9 = m[9], m13 = m[13];
    float m3 = m[3], m7 = m[7], m11 = m[11], m15 = m[15];

    float vx_line = bx - ax;
    float vy_line = by - ay;
    float len2 = vx_line * vx_line + vy_line * vy_line;
    if (len2 < 1e-4f) len2 = 1.0f;

    int loopCount = blurMaskedOnly ? nbActiveVerts : totalNbVerts;

    for (int k = 0; k < loopCount; ++k) {
        uint32_t i = blurMaskedOnly ? activeVerts[k] : k;
        int ind = i * 3;
        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float w = m3 * vx + m7 * vy + m11 * vz + m15;
        if (w == 0.0f) w = 1.0f;
        float sx = (m0 * vx + m4 * vy + m8 * vz + m12) / w;
        float sy = height - (m1 * vx + m5 * vy + m9 * vz + m13) / w;

        float t = ((sx - ax) * vx_line + (sy - ay) * vy_line) / len2;

        if (symmetry) {
            float dx = vx - ptPlaneX;
            float dy = vy - ptPlaneY;
            float dz = vz - ptPlaneZ;
            float dot = dx * nPlaneX + dy * nPlaneY + dz * nPlaneZ;
            float svx = vx - 2.0f * nPlaneX * dot;
            float svy = vy - 2.0f * nPlaneY * dot;
            float svz = vz - 2.0f * nPlaneZ * dot;

            float sw = m3 * svx + m7 * svy + m11 * svz + m15;
            if (sw == 0.0f) sw = 1.0f;
            float ssx = (m0 * svx + m4 * svy + m8 * svz + m12) / sw;
            float ssy = height - (m1 * svx + m5 * svy + m9 * svz + m13) / sw;
            float t_sym = ((ssx - ax) * vx_line + (ssy - ay) * vy_line) / len2;
            t = std::min(t, t_sym);
        }

        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;

        if (blurMaskedOnly) {
            float origVal = origMasks[i];
            float blurredVal = blurredMasks[i];
            float blendVal = (1.0f - t) * blurredVal + t * origVal;
            materials[ind + 2] = (1.0f - t) * 1.0f + t * blendVal;
        } else {
            materials[ind + 2] = t;
        }
    }
    return loopCount;
}

inline float getStampProfile(float fallOff) {
    if (fallOff <= 0.0f) return 0.0f;
    if (fallOff >= 1.0f) return 1.0f;

    float y = 0.0f;
    if (fallOff <= 0.60295695f) {
        float t = fallOff / 0.60295695f;
        float t2 = t * t;
        float t3 = t2 * t;
        y = 0.33650525f * t3 - 0.33650525f * t2 + 0.2664517f * t;
    } else {
        float t = (fallOff - 0.60295695f) / 0.39704305f;
        float t2 = t * t;
        float t3 = t2 * t;
        y = -0.33649525f * t3 + 0.6730105f * t2 + 0.39704305f * t + 0.2664517f;
    }

    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;

    return y * y;
}

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
) {
    const float radiusSq = radius * radius;
    float deformIntensity = (intensity / 3.0f) * 0.08f;
    float brushFactor = deformIntensity * radius;
    if (negative) {
        brushFactor = -brushFactor;
    }
    int writeIdx = 0;

    for (int i = 0; i < nbIVerts; ++i) {
        if (i + 8 < nbIVerts) {
            uint32_t nextId = iVerts[i + 8];
            __builtin_prefetch(&verts[nextId * 3], 0, 1);
            __builtin_prefetch(&materials[nextId * 3], 0, 1);
        }

        uint32_t id = iVerts[i];
        int ind = id * 3;

        float dx = cx - vertProxy[ind];
        float dy = cy - vertProxy[ind + 1];
        float dz = cz - vertProxy[ind + 2];
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) {
            continue;
        }

        float dist = std::sqrt(distSq);
        float vx = verts[ind];
        float vy = verts[ind + 1];
        float vz = verts[ind + 2];

        float fallOff = 0.0f;
        if (useAccuCurve && accuCurveLut) {
            if (dist < 1.0f) {
                float floatIndex = dist * 255.0f;
                if (floatIndex > 254.0f) floatIndex = 254.0f;
                int index = static_cast<int>(floatIndex);
                float fract = floatIndex - index;
                fallOff = accuCurveLut[index] * (1.0f - fract) + accuCurveLut[index + 1] * fract;
            }
        } else {
            if (dist < 1.0f) {
                float fs = focalShiftFalloff ? focalShift : 0.0f;
                float power = std::pow(2.0f, fs * 2.0f);
                fallOff = std::pow(1.0f - dist, power);
            }
        }

        float alphaVal = getAlphaVal(
            vx, vy, vz,
            hasAlpha, alphaTex, alphaWidth, alphaHeight,
            alphaRatioX, alphaRatioY, alphaSide,
            alphaLookAt, alphaXSym,
            focalShift, focalShiftFalloff
        );
        fallOff *= materials[ind + 2] * alphaVal;

        float stamp = getStampProfile(fallOff);
        float brushModifier = stamp * brushFactor;

        verts[ind] = vx + anx * brushModifier;
        verts[ind + 1] = vy + any * brushModifier;
        verts[ind + 2] = vz + anz * brushModifier;

        iVerts[writeIdx++] = id;
    }
    return writeIdx;
}

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
) {
    float comp = negative ? -1.0f : 1.0f;
    int writeIdx = 0;

    #pragma omp parallel if(nbIVerts > 500)
    {
        std::vector<uint32_t> localVerts;
        #pragma omp for schedule(static)
        for (int i = 0; i < nbIVerts; ++i) {
            uint32_t id = iVerts[i];
            int ind = id * 3;

            float vx = verts[ind];
            float vy = verts[ind + 1];
            float vz = verts[ind + 2];

            float distToPlane = (vx - ax) * anx + (vy - ay) * any + (vz - az) * anz;
            if (distToPlane * comp > 0.0f) {
                continue;
            }

            float xn = alphaRatioY * (alphaLookAt[0] * vx + alphaLookAt[4] * vy + alphaLookAt[8] * vz + alphaLookAt[12]) / (alphaXSym ? -alphaSide : alphaSide);
            float yn = alphaRatioX * (alphaLookAt[1] * vx + alphaLookAt[5] * vy + alphaLookAt[9] * vz + alphaLookAt[13]) / alphaSide;

            float dist = std::max(std::abs(xn), std::abs(yn));
            if (dist >= 1.0f) {
                continue;
            }

            float fallOff = getFallOff(dist, focalShiftFalloff ? focalShift : 0.0f, false, nullptr);
            fallOff *= distToPlane * intensity * materials[ind + 2];

            if (fallOff == 0.0f) continue;

            verts[ind] -= anx * fallOff;
            verts[ind + 1] -= any * fallOff;
            verts[ind + 2] -= anz * fallOff;

            localVerts.push_back(id);
        }

        #pragma omp critical
        {
            for (uint32_t id : localVerts) {
                iVerts[writeIdx++] = id;
            }
        }
    }
    return writeIdx;
}

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
) {
    if (radius <= 0.0f) return 0;

    float angleRad = stampAngle * (3.1415926535f / 180.0f);
    float cosA = std::cos(angleRad);
    float sinA = std::sin(angleRad);
    int writeIdx = 0;

    #pragma omp parallel if(nbIVerts > 500)
    {
        std::vector<uint32_t> localVerts;
        #pragma omp for schedule(static)
        for (int i = 0; i < nbIVerts; ++i) {
            uint32_t id = iVerts[i];
            int ind = id * 3;

            float matVal = materials[ind + 2];
            if (matVal <= 0.0f) {
                continue;
            }

            float vx = verts[ind];
            float vy = verts[ind + 1];
            float vz = verts[ind + 2];

            float distToPlane = 0.0f;
            if (clay) {
                distToPlane = (vx - ax) * anx + (vy - ay) * any + (vz - az) * anz;
                float comp = negative ? -1.0f : 1.0f;
                if (distToPlane * comp > 0.0f) {
                    continue;
                }
            }

            // Project onto brush local coordinates (xn_proj, yn_proj)
            float xn_proj = (alphaLookAt[0] * vx + alphaLookAt[4] * vy + alphaLookAt[8] * vz + alphaLookAt[12]) / (alphaXSym ? -radius : radius);
            float yn_proj = (alphaLookAt[1] * vx + alphaLookAt[5] * vy + alphaLookAt[9] * vz + alphaLookAt[13]) / radius;

            // Pre-rotate coordinates by stampAngle
            float xn = xn_proj * cosA - yn_proj * sinA;
            float yn = xn_proj * sinA + yn_proj * cosA;

            // Calculate distance based on stampType
            float dist = 0.0f;
            bool inside = true;

            if (stampType == 0) { // Circle
                dist = std::sqrt(xn * xn + yn * yn);
                if (dist > 1.0f) inside = false;
            }
            else if (stampType == 1) { // Regular Polygon
                float r = std::sqrt(xn * xn + yn * yn);
                if (r > 1.0f) {
                    inside = false;
                } else {
                    int sides = std::max(3, stampSides);
                    float theta = std::atan2(yn, xn);
                    float alpha = 2.0f * 3.1415926535f / sides;
                    float folded = theta - alpha * std::round(theta / alpha);
                    
                    float rEdge = std::cos(alpha / 2.0f) / std::cos(folded);
                    dist = r / rEdge;
                    if (dist > 1.0f) inside = false;
                }
            }
            else if (stampType == 2) { // Star Shape
                float r = std::sqrt(xn * xn + yn * yn);
                if (r > 1.0f) {
                    inside = false;
                } else {
                    int sides = std::max(2, stampSides);
                    float theta = std::atan2(yn, xn);
                    float beta = 3.1415926535f / sides;
                    float folded = std::fmod(theta + 2.0f * 3.1415926535f, 2.0f * beta);
                    if (folded > beta) {
                        folded = 2.0f * beta - folded;
                    }
                    
                    float rIn = std::max(0.01f, std::min(0.99f, stampInnerRatio));
                    float num = rIn * std::sin(beta);
                    float den = std::sin(folded) * (1.0f - rIn * std::cos(beta)) + std::cos(folded) * rIn * std::sin(beta);
                    float rBoundary = num / std::max(1e-6f, den);
                    
                    dist = r / rBoundary;
                    if (dist > 1.0f) inside = false;
                }
            }
            else if (stampType == 3) { // Ring (Torus)
                float r = std::sqrt(xn * xn + yn * yn);
                float rIn = std::max(0.01f, std::min(0.99f, stampInnerRatio));
                if (r > 1.0f || r < rIn) {
                    inside = false;
                } else {
                    float rMid = (1.0f + rIn) * 0.5f;
                    float halfWidth = (1.0f - rIn) * 0.5f;
                    dist = std::abs(r - rMid) / halfWidth;
                }
            }
            else if (stampType == 4) { // Rectangle
                float rY = std::max(0.01f, std::min(1.0f, stampInnerRatio));
                if (std::abs(xn) > 1.0f || std::abs(yn) > rY) {
                    inside = false;
                } else {
                    dist = std::max(std::abs(xn), std::abs(yn) / rY);
                }
            }
            else {
                dist = std::sqrt(xn * xn + yn * yn);
                if (dist > 1.0f) inside = false;
            }

            if (!inside) {
                continue;
            }

            float fallOff = getFallOff(dist, focalShiftFalloff ? focalShift : 0.0f, false, nullptr);
            
            // Parametric edge blur
            if (stampBlur > 0.001f) {
                float blur = std::min(1.0f, stampBlur);
                if (dist > 1.0f - blur) {
                    float t = (1.0f - dist) / blur;
                    fallOff *= t * t * (3.0f - 2.0f * t); // smoothstep
                }
            }

            float deformVal = 0.0f;
            if (clay) {
                deformVal = -distToPlane * intensity * fallOff * matVal;
            } else {
                deformVal = (negative ? -intensity : intensity) * radius * 0.1f * fallOff * matVal;
            }

            if (deformVal == 0.0f) continue;

            verts[ind] += anx * deformVal;
            verts[ind + 1] += any * deformVal;
            verts[ind + 2] += anz * deformVal;

            localVerts.push_back(id);
        }

        #pragma omp critical
        {
            for (uint32_t id : localVerts) {
                iVerts[writeIdx++] = id;
            }
        }
    }
    return writeIdx;
}



