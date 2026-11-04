#include "mesh/NormalCalc.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#ifdef _OPENMP
#include <omp.h>
#endif

const uint32_t TRI_INDEX = 0xffffffff;

void updateFaceNormalsAndBoxes(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    const uint32_t* iFaces, int nbIFaces,
    float* faceNormals,
    float* faceBoxes,
    float* faceCenters
) {
    if (!verts || !faces || !faceNormals || !faceBoxes || !faceCenters || nbFaces <= 0 || nbVerts <= 0) return;
    bool full = (nbIFaces < 0 || (iFaces == nullptr && nbIFaces != 0));
    int loopCount = full ? nbFaces : nbIFaces;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        uint32_t ind = full ? i : iFaces[i];
        if (ind >= (uint32_t)nbFaces) continue;

        uint32_t idTri = ind * 3;
        uint32_t idFace = ind * 4;
        uint32_t idBox = ind * 6;

        uint32_t ind1 = faces[idFace] * 3;
        uint32_t ind2 = faces[idFace + 1] * 3;
        uint32_t ind3 = faces[idFace + 2] * 3;
        uint32_t ind4 = faces[idFace + 3];
        bool isQuad = (ind4 != TRI_INDEX);
        if (isQuad) ind4 *= 3;

        float v1x = verts[ind1];
        float v1y = verts[ind1 + 1];
        float v1z = verts[ind1 + 2];
        float v2x = verts[ind2];
        float v2y = verts[ind2 + 1];
        float v2z = verts[ind2 + 2];
        float v3x = verts[ind3];
        float v3y = verts[ind3 + 1];
        float v3z = verts[ind3 + 2];

        // compute normals
        float ax = v2x - v1x;
        float ay = v2y - v1y;
        float az = v2z - v1z;
        float bx = v3x - v1x;
        float by = v3y - v1y;
        float bz = v3z - v1z;
        float crx = ay * bz - az * by;
        float cry = az * bx - ax * bz;
        float crz = ax * by - ay * bx;

        // compute boxes
        float xmin = v1x < v2x ? (v1x < v3x ? v1x : v3x) : (v2x < v3x ? v2x : v3x);
        float xmax = v1x > v2x ? (v1x > v3x ? v1x : v3x) : (v2x > v3x ? v2x : v3x);
        float ymin = v1y < v2y ? (v1y < v3y ? v1y : v3y) : (v2y < v3y ? v2y : v3y);
        float ymax = v1y > v2y ? (v1y > v3y ? v1y : v3y) : (v2y > v3y ? v2y : v3y);
        float zmin = v1z < v2z ? (v1z < v3z ? v1z : v3z) : (v2z < v3z ? v2z : v3z);
        float zmax = v1z > v2z ? (v1z > v3z ? v1z : v3z) : (v2z > v3z ? v2z : v3z);

        if (isQuad) {
            float v4x = verts[ind4];
            float v4y = verts[ind4 + 1];
            float v4z = verts[ind4 + 2];
            if (v4x < xmin) xmin = v4x;
            if (v4x > xmax) xmax = v4x;
            if (v4y < ymin) ymin = v4y;
            if (v4y > ymax) ymax = v4y;
            if (v4z < zmin) zmin = v4z;
            if (v4z > zmax) zmax = v4z;
            
            float ax2 = v3x - v4x;
            float ay2 = v3y - v4y;
            float az2 = v3z - v4z;
            crx += ay2 * bz - az2 * by;
            cry += az2 * bx - ax2 * bz;
            crz += ax2 * by - ay2 * bx;
        }

        // normals
        faceNormals[idTri] = crx;
        faceNormals[idTri + 1] = cry;
        faceNormals[idTri + 2] = crz;
        // boxes
        faceBoxes[idBox] = xmin;
        faceBoxes[idBox + 1] = ymin;
        faceBoxes[idBox + 2] = zmin;
        faceBoxes[idBox + 3] = xmax;
        faceBoxes[idBox + 4] = ymax;
        faceBoxes[idBox + 5] = zmax;
        // compute centers
        faceCenters[idTri] = (xmin + xmax) * 0.5f;
        faceCenters[idTri + 1] = (ymin + ymax) * 0.5f;
        faceCenters[idTri + 2] = (zmin + zmax) * 0.5f;
    }
}

void updateFaceNormalsOnly(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    float* faceNormals
) {
    if (!verts || !faces || !faceNormals || nbFaces <= 0 || nbVerts <= 0) return;

    #pragma omp parallel for schedule(static) if(nbFaces > 500)
    for (int i = 0; i < nbFaces; ++i) {
        uint32_t idTri = i * 3;
        uint32_t idFace = i * 4;

        uint32_t ind1 = faces[idFace] * 3;
        uint32_t ind2 = faces[idFace + 1] * 3;
        uint32_t ind3 = faces[idFace + 2] * 3;
        uint32_t ind4 = faces[idFace + 3];
        bool isQuad = (ind4 != TRI_INDEX);

        float v1x = verts[ind1];
        float v1y = verts[ind1 + 1];
        float v1z = verts[ind1 + 2];
        float v2x = verts[ind2];
        float v2y = verts[ind2 + 1];
        float v2z = verts[ind2 + 2];
        float v3x = verts[ind3];
        float v3y = verts[ind3 + 1];
        float v3z = verts[ind3 + 2];

        float ax = v2x - v1x;
        float ay = v2y - v1y;
        float az = v2z - v1z;
        float bx = v3x - v1x;
        float by = v3y - v1y;
        float bz = v3z - v1z;
        float crx = ay * bz - az * by;
        float cry = az * bx - ax * bz;
        float crz = ax * by - ay * bx;

        if (isQuad) {
            uint32_t ind4_3 = ind4 * 3;
            float v4x = verts[ind4_3];
            float v4y = verts[ind4_3 + 1];
            float v4z = verts[ind4_3 + 2];

            float ax2 = v3x - v4x;
            float ay2 = v3y - v4y;
            float az2 = v3z - v4z;
            crx += ay2 * bz - az2 * by;
            cry += az2 * bx - ax2 * bz;
            crz += ax2 * by - ay2 * bx;
        }

        faceNormals[idTri]     = crx;
        faceNormals[idTri + 1] = cry;
        faceNormals[idTri + 2] = crz;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
static inline float fastInvSqrt(float lenSq) {
    if (lenSq <= 1e-12f) return 0.0f;
    __m128 s = _mm_set_ss(lenSq);
    __m128 r = _mm_rsqrt_ss(s);
    float res = _mm_cvtss_f32(r);
    return res * (1.5f - 0.5f * lenSq * res * res);
}
#else
static inline float fastInvSqrt(float lenSq) {
    return (lenSq > 1e-12f) ? (1.0f / std::sqrt(lenSq)) : 0.0f;
}
#endif

void updateVertexNormals(
    const uint32_t* iVerts, int nbIVerts, int totalNbVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    const float* faceNormals,
    float* outNormals
) {
    if (!vrfStartCount || !outNormals || totalNbVerts <= 0) return;
    bool full = (nbIVerts < 0 || (iVerts == nullptr && nbIVerts != 0));
    int loopCount = full ? totalNbVerts : nbIVerts;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        if (i + 8 < loopCount) {
            uint32_t nextInd = full ? (i + 8) : iVerts[i + 8];
            if (nextInd < (uint32_t)totalNbVerts) {
                __builtin_prefetch(&vrfStartCount[nextInd * 2], 0, 1);
                __builtin_prefetch(&outNormals[nextInd * 3], 1, 1);
            }
        }

        uint32_t ind = full ? i : iVerts[i];
        if (ind >= (uint32_t)totalNbVerts) continue;

        uint32_t start = vrfStartCount[ind * 2];
        uint32_t count = vrfStartCount[ind * 2 + 1];
        uint32_t ind3 = ind * 3;

        if (count == 0 || !vertRingFace || !faceNormals) {
            outNormals[ind3]     = 0.0f;
            outNormals[ind3 + 1] = 1.0f;
            outNormals[ind3 + 2] = 0.0f;
            continue;
        }

        // Fast-path: count == 4 (predominant case in regular quad sculpt meshes)
        if (count == 4) {
            uint32_t id0 = vertRingFace[start] * 3;
            uint32_t id1 = vertRingFace[start + 1] * 3;
            uint32_t id2 = vertRingFace[start + 2] * 3;
            uint32_t id3 = vertRingFace[start + 3] * 3;

            float nx = faceNormals[id0]     + faceNormals[id1]     + faceNormals[id2]     + faceNormals[id3];
            float ny = faceNormals[id0 + 1] + faceNormals[id1 + 1] + faceNormals[id2 + 1] + faceNormals[id3 + 1];
            float nz = faceNormals[id0 + 2] + faceNormals[id1 + 2] + faceNormals[id2 + 2] + faceNormals[id3 + 2];

            float lenSq = nx * nx + ny * ny + nz * nz;
            float invLen = fastInvSqrt(lenSq);
            if (invLen > 0.0f) {
                outNormals[ind3]     = nx * invLen;
                outNormals[ind3 + 1] = ny * invLen;
                outNormals[ind3 + 2] = nz * invLen;
            } else {
                outNormals[ind3]     = 0.0f;
                outNormals[ind3 + 1] = 1.0f;
                outNormals[ind3 + 2] = 0.0f;
            }
            continue;
        }

        // Fast-path: count <= 4 (count == 1, 2, 3)
        if (count <= 4) {
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t id = vertRingFace[start + j] * 3;
                nx += faceNormals[id];
                ny += faceNormals[id + 1];
                nz += faceNormals[id + 2];
            }
            float lenSq = nx * nx + ny * ny + nz * nz;
            float invLen = fastInvSqrt(lenSq);
            if (invLen > 0.0f) {
                outNormals[ind3]     = nx * invLen;
                outNormals[ind3 + 1] = ny * invLen;
                outNormals[ind3 + 2] = nz * invLen;
            } else {
                outNormals[ind3]     = 0.0f;
                outNormals[ind3 + 1] = 1.0f;
                outNormals[ind3 + 2] = 0.0f;
            }
            continue;
        }

        // Fast-path: extreme count (> 32, e.g. UV sphere pole)
        // Direct summation prevents O(N^2) stalls on thousands of incident faces
        if (count > 32) {
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t id = vertRingFace[start + j] * 3;
                nx += faceNormals[id];
                ny += faceNormals[id + 1];
                nz += faceNormals[id + 2];
            }
            float lenSq = nx * nx + ny * ny + nz * nz;
            float invLen = fastInvSqrt(lenSq);
            if (invLen > 0.0f) {
                outNormals[ind3]     = nx * invLen;
                outNormals[ind3 + 1] = ny * invLen;
                outNormals[ind3 + 2] = nz * invLen;
            } else {
                outNormals[ind3]     = 0.0f;
                outNormals[ind3 + 1] = 1.0f;
                outNormals[ind3 + 2] = 0.0f;
            }
            continue;
        }

        // Irregular vertices (5 <= count <= 32)
        // Fixed stack buffer to completely avoid dynamic vector allocations inside OpenMP threads
        float fnx_stack[32], fny_stack[32], fnz_stack[32];

        for (uint32_t j = 0; j < count; ++j) {
            uint32_t id = vertRingFace[start + j] * 3;
            float fx = faceNormals[id];
            float fy = faceNormals[id + 1];
            float fz = faceNormals[id + 2];
            float flenSq = fx * fx + fy * fy + fz * fz;
            float inv = fastInvSqrt(flenSq);
            if (inv > 0.0f) {
                fnx_stack[j] = fx * inv;
                fny_stack[j] = fy * inv;
                fnz_stack[j] = fz * inv;
            } else {
                fnx_stack[j] = 0.0f; fny_stack[j] = 1.0f; fnz_stack[j] = 0.0f;
            }
        }

        // Check for creases (dot <= 0.707) across incident faces
        bool hasCrease = false;
        for (uint32_t j = 0; j < count; ++j) {
            for (uint32_t k = j + 1; k < count; ++k) {
                float dot = fnx_stack[j] * fnx_stack[k] + fny_stack[j] * fny_stack[k] + fnz_stack[j] * fnz_stack[k];
                if (dot <= 0.707f) {
                    hasCrease = true;
                    break;
                }
            }
            if (hasCrease) break;
        }

        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        if (!hasCrease) {
            // Smooth patch: all faces belong to the same cluster
            for (uint32_t j = 0; j < count; ++j) {
                nx += fnx_stack[j];
                ny += fny_stack[j];
                nz += fnz_stack[j];
            }
        } else {
            // Cluster-weighted accumulation fallback for complex creases (e.g. after Boolean/Trim/CDT)
            for (uint32_t j = 0; j < count; ++j) {
                float clusterWeight = 0.0f;
                for (uint32_t k = 0; k < count; ++k) {
                    float dot = fnx_stack[j] * fnx_stack[k] + fny_stack[j] * fny_stack[k] + fnz_stack[j] * fnz_stack[k];
                    if (dot > 0.707f) {
                        clusterWeight += dot;
                    }
                }
                float w = (clusterWeight > 1e-4f) ? (1.0f / clusterWeight) : 1.0f;
                nx += fnx_stack[j] * w;
                ny += fny_stack[j] * w;
                nz += fnz_stack[j] * w;
            }
        }

        float lenSq = nx * nx + ny * ny + nz * nz;
        float invLen = fastInvSqrt(lenSq);
        if (invLen > 0.0f) {
            outNormals[ind3]     = nx * invLen;
            outNormals[ind3 + 1] = ny * invLen;
            outNormals[ind3 + 2] = nz * invLen;
        } else {
            outNormals[ind3]     = 0.0f;
            outNormals[ind3 + 1] = 1.0f;
            outNormals[ind3 + 2] = 0.0f;
        }
    }
}

void computeNormalsDirect(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    float* faceNormals,
    float* outNormals
) {
    if (!verts || !faces || !faceNormals || !outNormals || nbVerts <= 0 || nbFaces <= 0) return;

    int maxThreads = 1;
#ifdef _OPENMP
    maxThreads = omp_get_max_threads();
#endif
    // Cap thread count to 4 to balance parallelism with memory allocation overhead and cache footprint
    int nThreads = std::clamp(maxThreads, 1, 4);

    // Thread-local accumulation buffers (cleared to 0)
    std::vector<float> accumNormals(static_cast<size_t>(nThreads) * nbVerts * 3, 0.0f);

#pragma omp parallel num_threads(nThreads)
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        float* myNormals = &accumNormals[static_cast<size_t>(tid) * nbVerts * 3];

#pragma omp for schedule(static)
        for (int i = 0; i < nbFaces; ++i) {
            uint32_t idTri = i * 3;
            uint32_t idFace = i * 4;

            uint32_t iv0 = faces[idFace];
            uint32_t iv1 = faces[idFace + 1];
            uint32_t iv2 = faces[idFace + 2];
            uint32_t iv3 = faces[idFace + 3];
            bool isQuad = (iv3 != TRI_INDEX && iv3 < (uint32_t)nbVerts);

            if (iv0 >= (uint32_t)nbVerts || iv1 >= (uint32_t)nbVerts || iv2 >= (uint32_t)nbVerts) continue;

            uint32_t ind1 = iv0 * 3;
            uint32_t ind2 = iv1 * 3;
            uint32_t ind3 = iv2 * 3;

            float v1x = verts[ind1];
            float v1y = verts[ind1 + 1];
            float v1z = verts[ind1 + 2];
            float v2x = verts[ind2];
            float v2y = verts[ind2 + 1];
            float v2z = verts[ind2 + 2];
            float v3x = verts[ind3];
            float v3y = verts[ind3 + 1];
            float v3z = verts[ind3 + 2];

            float ax = v2x - v1x;
            float ay = v2y - v1y;
            float az = v2z - v1z;
            float bx = v3x - v1x;
            float by = v3y - v1y;
            float bz = v3z - v1z;
            float crx = ay * bz - az * by;
            float cry = az * bx - ax * bz;
            float crz = ax * by - ay * bx;

            if (isQuad) {
                uint32_t ind4 = iv3 * 3;
                float v4x = verts[ind4];
                float v4y = verts[ind4 + 1];
                float v4z = verts[ind4 + 2];

                float ax2 = v3x - v4x;
                float ay2 = v3y - v4y;
                float az2 = v3z - v4z;
                crx += ay2 * bz - az2 * by;
                cry += az2 * bx - ax2 * bz;
                crz += ax2 * by - ay2 * bx;
            }

            faceNormals[idTri]     = crx;
            faceNormals[idTri + 1] = cry;
            faceNormals[idTri + 2] = crz;

            myNormals[ind1]     += crx;
            myNormals[ind1 + 1] += cry;
            myNormals[ind1 + 2] += crz;

            myNormals[ind2]     += crx;
            myNormals[ind2 + 1] += cry;
            myNormals[ind2 + 2] += crz;

            myNormals[ind3]     += crx;
            myNormals[ind3 + 1] += cry;
            myNormals[ind3 + 2] += crz;

            if (isQuad) {
                uint32_t ind4 = iv3 * 3;
                myNormals[ind4]     += crx;
                myNormals[ind4 + 1] += cry;
                myNormals[ind4 + 2] += crz;
            }
        }
    }

#pragma omp parallel for schedule(static) if(nbVerts > 2000)
    for (int i = 0; i < nbVerts; ++i) {
        uint32_t i3 = i * 3;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        for (int t = 0; t < nThreads; ++t) {
            const float* tBuf = &accumNormals[static_cast<size_t>(t) * nbVerts * 3 + i3];
            nx += tBuf[0];
            ny += tBuf[1];
            nz += tBuf[2];
        }

        float lenSq = nx * nx + ny * ny + nz * nz;
        float invLen = (lenSq > 1e-12f) ? (1.0f / std::sqrt(lenSq)) : 0.0f;
        if (invLen > 0.0f) {
            outNormals[i3]     = nx * invLen;
            outNormals[i3 + 1] = ny * invLen;
            outNormals[i3 + 2] = nz * invLen;
        } else {
            outNormals[i3]     = 0.0f;
            outNormals[i3 + 1] = 1.0f;
            outNormals[i3 + 2] = 0.0f;
        }
    }
}

void diagnoseMeshNormals(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    const uint32_t* faceGroups,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    const float* faceNormals,
    const float* vertNormals
) {
    if (nbVerts <= 0 || !verts || !vrfStartCount || !vertRingFace || !faceNormals || !vertNormals) return;

    struct WorstVertInfo {
        uint32_t vid;
        float x, y, z;
        float nx, ny, nz;
        float minDotWithFace;
        float angleToFaceDeg;
        float creaseAngleDeg;
        int faceCount;
        std::vector<uint32_t> groups;
    };

    int numSharpCrease = 0;   // faces meeting with dot < 0.0 (>90 deg)
    int numAcuteCrease = 0;   // faces meeting with dot < -0.5 (>120 deg)
    int numExtremeCrease = 0; // faces meeting with dot < -0.8 (>143 deg)
    int numInverted = 0;      // vertex normal dot with incident face < 0.0 (pointing behind face!)
    int numSevere = 0;        // vertex normal dot with incident face < 0.3 (>72.5 deg)
    int numTilted = 0;        // vertex normal dot with incident face < 0.5 (>60 deg)
    int numSeamVerts = 0;     // touches 2 face groups
    int numCornerVerts = 0;   // touches 3+ face groups

    std::vector<WorstVertInfo> worstVerts;
    worstVerts.reserve(64);

    for (int i = 0; i < nbVerts; ++i) {
        uint32_t start = vrfStartCount[i * 2];
        uint32_t count = vrfStartCount[i * 2 + 1];
        if (count == 0) continue;

        uint32_t ind3 = i * 3;
        float vnx = vertNormals[ind3];
        float vny = vertNormals[ind3 + 1];
        float vnz = vertNormals[ind3 + 2];

        float minDot = 1.0f;
        float minCreaseDot = 1.0f;
        std::vector<uint32_t> uniqueGroups;

        // Collect incident face normals
        std::vector<float> fnx(count), fny(count), fnz(count);

        for (uint32_t j = 0; j < count; ++j) {
            uint32_t fIdx = vertRingFace[start + j];
            if (faceGroups && fIdx < (uint32_t)nbFaces) {
                uint32_t gid = faceGroups[fIdx];
                if (std::find(uniqueGroups.begin(), uniqueGroups.end(), gid) == uniqueGroups.end()) {
                    uniqueGroups.push_back(gid);
                }
            }

            uint32_t fId3 = fIdx * 3;
            float fx = faceNormals[fId3];
            float fy = faceNormals[fId3 + 1];
            float fz = faceNormals[fId3 + 2];
            float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-12f) {
                float inv = 1.0f / flen;
                fnx[j] = fx * inv;
                fny[j] = fy * inv;
                fnz[j] = fz * inv;
            } else {
                fnx[j] = 0.0f; fny[j] = 1.0f; fnz[j] = 0.0f;
            }

            float d = vnx * fnx[j] + vny * fny[j] + vnz * fnz[j];
            if (d < minDot) minDot = d;
        }

        // Pairwise face-to-face dot products around vertex
        for (uint32_t j = 0; j < count; ++j) {
            for (uint32_t k = j + 1; k < count; ++k) {
                float fd = fnx[j] * fnx[k] + fny[j] * fny[k] + fnz[j] * fnz[k];
                if (fd < minCreaseDot) minCreaseDot = fd;
            }
        }

        if (uniqueGroups.size() == 2) numSeamVerts++;
        else if (uniqueGroups.size() > 2) numCornerVerts++;

        if (minCreaseDot < 0.0f) numSharpCrease++;
        if (minCreaseDot < -0.5f) numAcuteCrease++;
        if (minCreaseDot < -0.8f) numExtremeCrease++;

        if (minDot < 0.0f) numInverted++;
        if (minDot < 0.3f) numSevere++;
        if (minDot < 0.5f) numTilted++;

        if (minDot < 0.3f || (uniqueGroups.size() >= 2 && minCreaseDot < 0.0f)) {
            float angleToFace = std::acos(std::clamp(minDot, -1.0f, 1.0f)) * 57.2957795f;
            float creaseAngle = std::acos(std::clamp(minCreaseDot, -1.0f, 1.0f)) * 57.2957795f;
            worstVerts.push_back({
                (uint32_t)i,
                verts[ind3], verts[ind3 + 1], verts[ind3 + 2],
                vnx, vny, vnz,
                minDot, angleToFace, creaseAngle,
                (int)count,
                uniqueGroups
            });
        }
    }

    sculpt_log("[NORMALS DIAGNOSTICS] Total: %d verts | Seams (2 groups): %d | Corners (3+ groups): %d\n",
               nbVerts, numSeamVerts, numCornerVerts);
    sculpt_log("[NORMALS DIAGNOSTICS] Crease Verts: >90 deg: %d, >120 deg: %d, >143 deg: %d\n",
               numSharpCrease, numAcuteCrease, numExtremeCrease);
    sculpt_log("[NORMALS DIAGNOSTICS] Normal-to-Face Alignment: Inverted (<0): %d | Severe (<0.3, >72 deg): %d | Tilted (<0.5, >60 deg): %d\n",
               numInverted, numSevere, numTilted);

    if (!worstVerts.empty()) {
        std::sort(worstVerts.begin(), worstVerts.end(), [](const WorstVertInfo& a, const WorstVertInfo& b) {
            return a.minDotWithFace < b.minDotWithFace;
        });

        int showCount = std::min((int)worstVerts.size(), 8);
        sculpt_log("[NORMALS DIAGNOSTICS] --- Worst %d Vertex Normals at Creases/Corners ---\n", showCount);
        for (int k = 0; k < showCount; ++k) {
            const auto& w = worstVerts[k];
            std::string gStr;
            for (size_t g = 0; g < w.groups.size(); ++g) {
                if (g > 0) gStr += ", ";
                gStr += std::to_string(w.groups[g]);
            }
            sculpt_log("  Vert #%u at (%.2f, %.2f, %.2f): Normal=[%.3f, %.3f, %.3f] | minDot=%.3f (%.1f deg to face) | Crease=%.1f deg | Faces=%d | Groups=[%s]\n",
                       w.vid, w.x, w.y, w.z, w.nx, w.ny, w.nz, w.minDotWithFace, w.angleToFaceDeg, w.creaseAngleDeg, w.faceCount, gStr.c_str());

            uint32_t start = vrfStartCount[w.vid * 2];
            for (uint32_t j = 0; j < (uint32_t)w.faceCount; ++j) {
                uint32_t fIdx = vertRingFace[start + j];
                uint32_t gid = (faceGroups && fIdx < (uint32_t)nbFaces) ? faceGroups[fIdx] : 0;
                float fx = faceNormals[fIdx * 3];
                float fy = faceNormals[fIdx * 3 + 1];
                float fz = faceNormals[fIdx * 3 + 2];
                float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (flen > 1e-12f) { fx /= flen; fy /= flen; fz /= flen; }
                float dFace = w.nx * fx + w.ny * fy + w.nz * fz;
                sculpt_log("     Face #%u (G%u): Normal=[%.3f, %.3f, %.3f], dot=%.3f\n",
                           fIdx, gid, fx, fy, fz, dFace);
            }
        }
        sculpt_log("[NORMALS DIAGNOSTICS] ----------------------------------------------------------------\n");
    }
}
