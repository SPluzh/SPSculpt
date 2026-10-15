#include "mesh/NormalCalc.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

const uint32_t TRI_INDEX = 0xffffffff;

void updateFaceNormalsAndBoxes(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    const uint32_t* iFaces, int nbIFaces,
    float* faceNormals,
    float* faceBoxes,
    float* faceCenters
) {
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

void updateVertexNormals(
    const uint32_t* iVerts, int nbIVerts, int totalNbVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    const float* faceNormals,
    float* outNormals
) {
    bool full = (nbIVerts < 0 || (iVerts == nullptr && nbIVerts != 0));
    int loopCount = full ? totalNbVerts : nbIVerts;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        uint32_t ind = full ? i : iVerts[i];
        if (ind >= (uint32_t)totalNbVerts) continue;

        uint32_t start = vrfStartCount[ind * 2];
        uint32_t count = vrfStartCount[ind * 2 + 1];
        if (count == 0) continue;

        if (count == 1) {
            uint32_t id = vertRingFace[start] * 3;
            float fx = faceNormals[id];
            float fy = faceNormals[id + 1];
            float fz = faceNormals[id + 2];
            float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
            float inv = (flen > 1e-12f) ? (1.0f / flen) : 0.0f;
            outNormals[ind * 3]     = fx * inv;
            outNormals[ind * 3 + 1] = fy * inv;
            outNormals[ind * 3 + 2] = fz * inv;
            continue;
        }

        // For dense vertex fans (k > 24, e.g. cap junctions), cluster-weighted O(k²)
        // is expensive and unnecessary. Direct averaging is O(k) with zero heap allocation.
        if (count > 24) {
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t id = vertRingFace[start + j] * 3;
                float fx = faceNormals[id];
                float fy = faceNormals[id + 1];
                float fz = faceNormals[id + 2];
                float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (flen > 1e-12f) {
                    float inv = 1.0f / flen;
                    nx += fx * inv;
                    ny += fy * inv;
                    nz += fz * inv;
                }
            }
            uint32_t ind3 = ind * 3;
            float lenSq = nx * nx + ny * ny + nz * nz;
            if (lenSq > 1e-12f) {
                float invLen = 1.0f / std::sqrt(lenSq);
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

        // Fetch and normalize each incident face normal on stack (zero heap allocation)
        float fnx_stack[24], fny_stack[24], fnz_stack[24];
        float* pFnx = fnx_stack;
        float* pFny = fny_stack;
        float* pFnz = fnz_stack;

        for (uint32_t j = 0; j < count; ++j) {
            uint32_t id = vertRingFace[start + j] * 3;
            float fx = faceNormals[id];
            float fy = faceNormals[id + 1];
            float fz = faceNormals[id + 2];
            float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-12f) {
                float inv = 1.0f / flen;
                pFnx[j] = fx * inv;
                pFny[j] = fy * inv;
                pFnz[j] = fz * inv;
            } else {
                pFnx[j] = 0.0f; pFny[j] = 1.0f; pFnz[j] = 0.0f;
            }
        }

        // Cluster-weighted accumulation:
        // When multiple coplanar faces meet 1 quad across a crease, naive summation gives
        // excessive weight to the planar cluster. Inverse cluster density scaling balances 1:1 weight.
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
        float avgFx = 0.0f, avgFy = 0.0f, avgFz = 0.0f;

        for (uint32_t j = 0; j < count; ++j) {
            avgFx += pFnx[j];
            avgFy += pFny[j];
            avgFz += pFnz[j];

            float clusterWeight = 0.0f;
            for (uint32_t k = 0; k < count; ++k) {
                float dot = pFnx[j] * pFnx[k] + pFny[j] * pFny[k] + pFnz[j] * pFnz[k];
                if (dot > 0.707f) {
                    clusterWeight += dot;
                }
            }
            float w = (clusterWeight > 1e-4f) ? (1.0f / clusterWeight) : 1.0f;
            nx += pFnx[j] * w;
            ny += pFny[j] * w;
            nz += pFnz[j] * w;
        }

        // R2: Verify normal-to-face orientation. If inverted (dot < 0 with average face normal), flip it.
        if (nx * avgFx + ny * avgFy + nz * avgFz < 0.0f) {
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }

        uint32_t ind3 = ind * 3;
        float lenSq = nx * nx + ny * ny + nz * nz;
        if (lenSq > 1e-12f) {
            float invLen = 1.0f / std::sqrt(lenSq);
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
