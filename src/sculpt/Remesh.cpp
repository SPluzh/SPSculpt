#include "sculpt/Remesh.h"
#include "sculpt/KeepMasked.h"
#include "common/Constants.h"
#include "common/MemoryUtils.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>
#include <thread>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <glm/glm.hpp>
#include "common/Logger.h"

// Bounding box mapping and structures
struct HermiteData {
    float nx, ny, nz;
    float t;
};

struct VoxelGrid {
    int dims[3];
    float step;
    float minCoord[3];
    float maxCoord[3];
    std::vector<uint8_t> crossedEdges;
    std::vector<float> distanceField;
    std::vector<uint32_t> colorField;      // packed colors
    std::vector<uint32_t> materialField;   // packed materials
    std::vector<uint32_t> groupField;      // polygroup IDs
    std::vector<std::unordered_map<uint32_t, HermiteData>> hermiteEdges;
    float uniformColor[3];
    float uniformMaterial[3];
    bool hasColorField = false;
    bool hasMaterialField = false;
    bool hasGroupField = false;
};

// ---------------------------------------------------------
// Geometry Utilities
// ---------------------------------------------------------


static double distance2PointTriangleEdges(
    const double point[3],
    const double edge1[3],
    const double edge2[3],
    const double v1[3],
    double a00, double a01, double a11,
    double closest[5]
) {
    double diff[3] = { v1[0] - point[0], v1[1] - point[1], v1[2] - point[2] };
    double b0 = diff[0] * edge1[0] + diff[1] * edge1[1] + diff[2] * edge1[2];
    double b1 = diff[0] * edge2[0] + diff[1] * edge2[1] + diff[2] * edge2[2];
    double c = diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2];
    double det = std::abs(a00 * a11 - a01 * a01);
    double s = a01 * b1 - a11 * b0;
    double t = a01 * b0 - a00 * b1;
    double sqrDistance;
    int zone = 4;

    if (s + t <= det) {
        if (s < 0.0) {
            if (t < 0.0) { // region 4
                zone = 4;
                if (b0 < 0.0) {
                    t = 0.0;
                    if (-b0 >= a00) {
                        s = 1.0;
                        sqrDistance = a00 + 2.0 * b0 + c;
                    } else {
                        s = -b0 / a00;
                        sqrDistance = b0 * s + c;
                    }
                } else {
                    s = 0.0;
                    if (b1 >= 0.0) {
                        t = 0.0;
                        sqrDistance = c;
                    } else if (-b1 >= a11) {
                        t = 1.0;
                        sqrDistance = a11 + 2.0 * b1 + c;
                    } else {
                        t = -b1 / a11;
                        sqrDistance = b1 * t + c;
                    }
                }
            } else { // region 3
                zone = 3;
                s = 0.0;
                if (b1 >= 0.0) {
                    t = 0.0;
                    sqrDistance = c;
                } else if (-b1 >= a11) {
                    t = 1.0;
                    sqrDistance = a11 + 2.0 * b1 + c;
                } else {
                    t = -b1 / a11;
                    sqrDistance = b1 * t + c;
                }
            }
        } else if (t < 0.0) { // region 5
            zone = 5;
            t = 0.0;
            if (b0 >= 0.0) {
                s = 0.0;
                sqrDistance = c;
            } else if (-b0 >= a00) {
                s = 1.0;
                sqrDistance = a00 + 2.0 * b0 + c;
            } else {
                s = -b0 / a00;
                sqrDistance = b0 * s + c;
            }
        } else { // region 0
            zone = 0;
            double invDet = 1.0 / det;
            s *= invDet;
            t *= invDet;
            sqrDistance = s * (a00 * s + a01 * t + 2.0 * b0) + t * (a01 * s + a11 * t + 2.0 * b1) + c;
        }
    } else {
        double tmp0, tmp1, numer, denom;
        if (s < 0.0) { // region 2
            zone = 2;
            tmp0 = a01 + b0;
            tmp1 = a11 + b1;
            if (tmp1 > tmp0) {
                numer = tmp1 - tmp0;
                denom = a00 - 2.0 * a01 + a11;
                if (numer >= denom) {
                    s = 1.0;
                    t = 0.0;
                    sqrDistance = a00 + 2.0 * b0 + c;
                } else {
                    s = numer / denom;
                    t = 1.0 - s;
                    sqrDistance = s * (a00 * s + a01 * t + 2.0 * b0) + t * (a01 * s + a11 * t + 2.0 * b1) + c;
                }
            } else {
                s = 0.0;
                if (tmp1 <= 0.0) {
                    t = 1.0;
                    sqrDistance = a11 + 2.0 * b1 + c;
                } else if (b1 >= 0.0) {
                    t = 0.0;
                    sqrDistance = c;
                } else {
                    t = -b1 / a11;
                    sqrDistance = b1 * t + c;
                }
            }
        } else if (t < 0.0) { // region 6
            zone = 6;
            tmp0 = a01 + b1;
            tmp1 = a00 + b0;
            if (tmp1 > tmp0) {
                numer = tmp1 - tmp0;
                denom = a00 - 2.0 * a01 + a11;
                if (numer >= denom) {
                    t = 1.0;
                    s = 0.0;
                    sqrDistance = a11 + 2.0 * b1 + c;
                } else {
                    t = numer / denom;
                    s = 1.0 - t;
                    sqrDistance = s * (a00 * s + a01 * t + 2.0 * b0) + t * (a01 * s + a11 * t + 2.0 * b1) + c;
                }
            } else {
                t = 0.0;
                if (tmp1 <= 0.0) {
                    s = 1.0;
                    sqrDistance = a00 + 2.0 * b0 + c;
                } else if (b0 >= 0.0) {
                    s = 0.0;
                    sqrDistance = c;
                } else {
                    s = -b0 / a00;
                    sqrDistance = b0 * s + c;
                }
            }
        } else { // region 1
            zone = 1;
            numer = a11 + b1 - a01 - b0;
            if (numer <= 0.0) {
                s = 0.0;
                t = 1.0;
                sqrDistance = a11 + 2.0 * b1 + c;
            } else {
                denom = a00 - 2.0 * a01 + a11;
                if (numer >= denom) {
                    s = 1.0;
                    t = 0.0;
                    sqrDistance = a00 + 2.0 * b0 + c;
                } else {
                    s = numer / denom;
                    t = 1.0 - s;
                    sqrDistance = s * (a00 * s + a01 * t + 2.0 * b0) + t * (a01 * s + a11 * t + 2.0 * b1) + c;
                }
            }
        }
    }

    if (sqrDistance < 0.0) sqrDistance = 0.0;

    closest[0] = v1[0] + s * edge1[0] + t * edge2[0];
    closest[1] = v1[1] + s * edge1[1] + t * edge2[1];
    closest[2] = v1[2] + s * edge1[2] + t * edge2[2];
    closest[3] = s;
    closest[4] = t;

    return sqrDistance;
}

static double intersectionRayTriangleEdges(
    const double orig[3],
    const double dir[3],
    const double edge1[3],
    const double edge2[3],
    const double v1[3]
) {
    const double EPSILON = 1E-15;
    const double ONE_PLUS_EPSILON = 1.0 + EPSILON;
    const double ZERO_MINUS_EPSILON = 0.0 - EPSILON;

    double pvec[3];
    // cross(dir, edge2)
    pvec[0] = dir[1] * edge2[2] - dir[2] * edge2[1];
    pvec[1] = dir[2] * edge2[0] - dir[0] * edge2[2];
    pvec[2] = dir[0] * edge2[1] - dir[1] * edge2[0];

    double det = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];
    if (det > -EPSILON && det < EPSILON)
        return -1.0;

    double invDet = 1.0 / det;
    double tvec[3] = { orig[0] - v1[0], orig[1] - v1[1], orig[2] - v1[2] };
    double u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * invDet;
    if (u < ZERO_MINUS_EPSILON || u > ONE_PLUS_EPSILON)
        return -1.0;

    double qvec[3];
    // cross(tvec, edge1)
    qvec[0] = tvec[1] * edge1[2] - tvec[2] * edge1[1];
    qvec[1] = tvec[2] * edge1[0] - tvec[0] * edge1[2];
    qvec[2] = tvec[0] * edge1[1] - tvec[1] * edge1[0];

    double v = (dir[0] * qvec[0] + dir[1] * qvec[1] + dir[2] * qvec[2]) * invDet;
    if (v < ZERO_MINUS_EPSILON || u + v > ONE_PLUS_EPSILON)
        return -1.0;

    double t = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * invDet;
    if (t < ZERO_MINUS_EPSILON)
        return -1.0;

    return t;
}

// ---------------------------------------------------------
// Spatial Acceleration Structure for PolyGroup Projection
// ---------------------------------------------------------

struct IndexedTriangle {
    uint32_t triIndex;
    uint32_t groupID;
    double v1[3];
    double edge1[3];
    double edge2[3];
    float normal[3];
    double a00, a01, a11;
    float bboxMin[3];
    float bboxMax[3];
};

class TriangleSpatialIndex {
private:
    float gridMin[3];
    float cellSize;
    int dims[3];
    std::vector<std::vector<uint32_t>> cells;
    std::vector<IndexedTriangle> m_triangles;

public:
    void build(const float* verts, int nbVerts, const uint32_t* tris, int nbTris, const uint32_t* faceGroups, float gridStep) {
        if (nbTris <= 0 || !verts || !tris) return;

        m_triangles.resize(nbTris);
        float minB[3] = { 1e30f, 1e30f, 1e30f };
        float maxB[3] = { -1e30f, -1e30f, -1e30f };

        for (int i = 0; i < nbTris; ++i) {
            uint32_t iv1 = tris[i * 3 + 0] * 3;
            uint32_t iv2 = tris[i * 3 + 1] * 3;
            uint32_t iv3 = tris[i * 3 + 2] * 3;

            IndexedTriangle& tri = m_triangles[i];
            tri.triIndex = (uint32_t)i;
            tri.groupID = (faceGroups ? faceGroups[i] : 0);

            tri.v1[0] = (double)verts[iv1];
            tri.v1[1] = (double)verts[iv1 + 1];
            tri.v1[2] = (double)verts[iv1 + 2];

            double v2_0 = (double)verts[iv2];
            double v2_1 = (double)verts[iv2 + 1];
            double v2_2 = (double)verts[iv2 + 2];

            double v3_0 = (double)verts[iv3];
            double v3_1 = (double)verts[iv3 + 1];
            double v3_2 = (double)verts[iv3 + 2];

            tri.edge1[0] = v2_0 - tri.v1[0];
            tri.edge1[1] = v2_1 - tri.v1[1];
            tri.edge1[2] = v2_2 - tri.v1[2];

            tri.edge2[0] = v3_0 - tri.v1[0];
            tri.edge2[1] = v3_1 - tri.v1[1];
            tri.edge2[2] = v3_2 - tri.v1[2];

            float nx = (float)(tri.edge1[1] * tri.edge2[2] - tri.edge1[2] * tri.edge2[1]);
            float ny = (float)(tri.edge1[2] * tri.edge2[0] - tri.edge1[0] * tri.edge2[2]);
            float nz = (float)(tri.edge1[0] * tri.edge2[1] - tri.edge1[1] * tri.edge2[0]);
            float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nlen > 1e-9f) {
                tri.normal[0] = nx / nlen;
                tri.normal[1] = ny / nlen;
                tri.normal[2] = nz / nlen;
            } else {
                tri.normal[0] = 0.0f;
                tri.normal[1] = 0.0f;
                tri.normal[2] = 1.0f;
            }

            tri.a00 = tri.edge1[0] * tri.edge1[0] + tri.edge1[1] * tri.edge1[1] + tri.edge1[2] * tri.edge1[2];
            tri.a01 = tri.edge1[0] * tri.edge2[0] + tri.edge1[1] * tri.edge2[1] + tri.edge1[2] * tri.edge2[2];
            tri.a11 = tri.edge2[0] * tri.edge2[0] + tri.edge2[1] * tri.edge2[1] + tri.edge2[2] * tri.edge2[2];

            float v1f[3] = { (float)tri.v1[0], (float)tri.v1[1], (float)tri.v1[2] };
            float v2f[3] = { (float)v2_0, (float)v2_1, (float)v2_2 };
            float v3f[3] = { (float)v3_0, (float)v3_1, (float)v3_2 };

            tri.bboxMin[0] = std::min({ v1f[0], v2f[0], v3f[0] });
            tri.bboxMin[1] = std::min({ v1f[1], v2f[1], v3f[1] });
            tri.bboxMin[2] = std::min({ v1f[2], v2f[2], v3f[2] });

            tri.bboxMax[0] = std::max({ v1f[0], v2f[0], v3f[0] });
            tri.bboxMax[1] = std::max({ v1f[1], v2f[1], v3f[1] });
            tri.bboxMax[2] = std::max({ v1f[2], v2f[2], v3f[2] });

            minB[0] = std::min(minB[0], tri.bboxMin[0]);
            minB[1] = std::min(minB[1], tri.bboxMin[1]);
            minB[2] = std::min(minB[2], tri.bboxMin[2]);

            maxB[0] = std::max(maxB[0], tri.bboxMax[0]);
            maxB[1] = std::max(maxB[1], tri.bboxMax[1]);
            maxB[2] = std::max(maxB[2], tri.bboxMax[2]);
        }

        cellSize = std::max(gridStep * 3.0f, 1e-4f);
        float invCellSize = 1.0f / cellSize;

        gridMin[0] = minB[0] - cellSize;
        gridMin[1] = minB[1] - cellSize;
        gridMin[2] = minB[2] - cellSize;

        dims[0] = (int)std::ceil((maxB[0] + cellSize - gridMin[0]) * invCellSize);
        dims[1] = (int)std::ceil((maxB[1] + cellSize - gridMin[1]) * invCellSize);
        dims[2] = (int)std::ceil((maxB[2] + cellSize - gridMin[2]) * invCellSize);

        dims[0] = std::max(1, dims[0]);
        dims[1] = std::max(1, dims[1]);
        dims[2] = std::max(1, dims[2]);

        int totalCells = dims[0] * dims[1] * dims[2];
        cells.resize(totalCells);

        for (int i = 0; i < nbTris; ++i) {
            const IndexedTriangle& tri = m_triangles[i];
            int cxMin = std::max(0, (int)std::floor((tri.bboxMin[0] - gridMin[0]) * invCellSize));
            int cyMin = std::max(0, (int)std::floor((tri.bboxMin[1] - gridMin[1]) * invCellSize));
            int czMin = std::max(0, (int)std::floor((tri.bboxMin[2] - gridMin[2]) * invCellSize));

            int cxMax = std::min(dims[0] - 1, (int)std::floor((tri.bboxMax[0] - gridMin[0]) * invCellSize));
            int cyMax = std::min(dims[1] - 1, (int)std::floor((tri.bboxMax[1] - gridMin[1]) * invCellSize));
            int czMax = std::min(dims[2] - 1, (int)std::floor((tri.bboxMax[2] - gridMin[2]) * invCellSize));

            for (int z = czMin; z <= czMax; ++z) {
                for (int y = cyMin; y <= cyMax; ++y) {
                    for (int x = cxMin; x <= cxMax; ++x) {
                        int cellIdx = x + y * dims[0] + z * dims[0] * dims[1];
                        cells[cellIdx].push_back((uint32_t)i);
                    }
                }
            }
        }
    }

    struct ClosestTriangleResult {
        uint32_t triIndex = 0;
        uint32_t groupID = 0;
        float w0 = 1.0f / 3.0f;
        float w1 = 1.0f / 3.0f;
        float w2 = 1.0f / 3.0f;
    };

    ClosestTriangleResult findClosestTriangle(const float point[3], const float normal[3]) const {
        ClosestTriangleResult res;
        if (m_triangles.empty()) return res;

        float invCellSize = 1.0f / cellSize;
        int cx = (int)std::floor((point[0] - gridMin[0]) * invCellSize);
        int cy = (int)std::floor((point[1] - gridMin[1]) * invCellSize);
        int cz = (int)std::floor((point[2] - gridMin[2]) * invCellSize);

        double bestDistSq = 1e30;
        bool foundAligned = false;
        double bestS = 0.0, bestT = 0.0;
        uint32_t bestTri = 0;
        uint32_t bestGroup = 0;

        double pointD[3] = { (double)point[0], (double)point[1], (double)point[2] };

        // Fast-path: Check cell r = 0 first
        if (cx >= 0 && cx < dims[0] && cy >= 0 && cy < dims[1] && cz >= 0 && cz < dims[2]) {
            int cellIdx = cx + cy * dims[0] + cz * dims[0] * dims[1];
            for (uint32_t triIdx : cells[cellIdx]) {
                const IndexedTriangle& tri = m_triangles[triIdx];
                double closest[5];
                double distSq = distance2PointTriangleEdges(
                    pointD, tri.edge1, tri.edge2, tri.v1, tri.a00, tri.a01, tri.a11, closest
                );

                float dotN = normal[0] * tri.normal[0] +
                             normal[1] * tri.normal[1] +
                             normal[2] * tri.normal[2];

                if (dotN > 0.0f) {
                    if (!foundAligned || distSq < bestDistSq) {
                        bestDistSq = distSq;
                        bestGroup = tri.groupID;
                        bestTri = tri.triIndex;
                        bestS = closest[3];
                        bestT = closest[4];
                        foundAligned = true;
                    }
                } else if (!foundAligned && distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestGroup = tri.groupID;
                    bestTri = tri.triIndex;
                    bestS = closest[3];
                    bestT = closest[4];
                }
            }
            if (foundAligned && bestDistSq < (double)(0.5f * cellSize * 0.5f * cellSize)) {
                res.triIndex = bestTri;
                res.groupID = bestGroup;
                res.w1 = (float)std::clamp(bestS, 0.0, 1.0);
                res.w2 = (float)std::clamp(bestT, 0.0, 1.0);
                res.w0 = (float)std::clamp(1.0 - bestS - bestT, 0.0, 1.0);
                float sum = res.w0 + res.w1 + res.w2;
                if (sum > 1e-6f) { res.w0 /= sum; res.w1 /= sum; res.w2 /= sum; }
                return res;
            }
        }

        int startR = foundAligned ? 1 : 0;
        for (int r = startR; r <= 3; ++r) {
            int xMin = std::max(0, cx - r);
            int xMax = std::min(dims[0] - 1, cx + r);
            int yMin = std::max(0, cy - r);
            int yMax = std::min(dims[1] - 1, cy + r);
            int zMin = std::max(0, cz - r);
            int zMax = std::min(dims[2] - 1, cz + r);

            for (int z = zMin; z <= zMax; ++z) {
                for (int y = yMin; y <= yMax; ++y) {
                    for (int x = xMin; x <= xMax; ++x) {
                        if (r > 0 && x > cx - r && x < cx + r && y > cy - r && y < cy + r && z > cz - r && z < cz + r) {
                            continue;
                        }
                        int cellIdx = x + y * dims[0] + z * dims[0] * dims[1];
                        for (uint32_t triIdx : cells[cellIdx]) {
                            const IndexedTriangle& tri = m_triangles[triIdx];
                            double closest[5];
                            double distSq = distance2PointTriangleEdges(
                                pointD, tri.edge1, tri.edge2, tri.v1, tri.a00, tri.a01, tri.a11, closest
                            );

                            float dotN = normal[0] * tri.normal[0] +
                                         normal[1] * tri.normal[1] +
                                         normal[2] * tri.normal[2];

                            if (dotN > 0.0f) {
                                if (!foundAligned || distSq < bestDistSq) {
                                    bestDistSq = distSq;
                                    bestGroup = tri.groupID;
                                    bestTri = tri.triIndex;
                                    bestS = closest[3];
                                    bestT = closest[4];
                                    foundAligned = true;
                                }
                            } else if (!foundAligned && distSq < bestDistSq) {
                                bestDistSq = distSq;
                                bestGroup = tri.groupID;
                                bestTri = tri.triIndex;
                                bestS = closest[3];
                                bestT = closest[4];
                            }
                        }
                    }
                }
            }
            if (foundAligned && bestDistSq < ((double)r * cellSize) * ((double)r * cellSize)) {
                break;
            }
        }

        if (bestDistSq >= 1e20) {
            int maxDim = std::max({ dims[0], dims[1], dims[2] });
            for (int r = 4; r <= maxDim && bestDistSq >= 1e20; ++r) {
                int xMin = std::max(0, cx - r);
                int xMax = std::min(dims[0] - 1, cx + r);
                int yMin = std::max(0, cy - r);
                int yMax = std::min(dims[1] - 1, cy + r);
                int zMin = std::max(0, cz - r);
                int zMax = std::min(dims[2] - 1, cz + r);

                for (int z = zMin; z <= zMax; ++z) {
                    for (int y = yMin; y <= yMax; ++y) {
                        for (int x = xMin; x <= xMax; ++x) {
                            if (x > cx - r && x < cx + r && y > cy - r && y < cy + r && z > cz - r && z < cz + r) {
                                continue;
                            }
                            int cellIdx = x + y * dims[0] + z * dims[0] * dims[1];
                            for (uint32_t triIdx : cells[cellIdx]) {
                                const IndexedTriangle& tri = m_triangles[triIdx];
                                double closest[5];
                                double distSq = distance2PointTriangleEdges(
                                    pointD, tri.edge1, tri.edge2, tri.v1, tri.a00, tri.a01, tri.a11, closest
                                );
                                float dotN = normal[0] * tri.normal[0] +
                                             normal[1] * tri.normal[1] +
                                             normal[2] * tri.normal[2];
                                if (dotN > 0.0f) {
                                    if (!foundAligned || distSq < bestDistSq) {
                                        bestDistSq = distSq;
                                        bestGroup = tri.groupID;
                                        bestTri = tri.triIndex;
                                        bestS = closest[3];
                                        bestT = closest[4];
                                        foundAligned = true;
                                    }
                                } else if (!foundAligned && distSq < bestDistSq) {
                                    bestDistSq = distSq;
                                    bestGroup = tri.groupID;
                                    bestTri = tri.triIndex;
                                    bestS = closest[3];
                                    bestT = closest[4];
                                }
                            }
                        }
                    }
                }
            }
        }

        res.triIndex = bestTri;
        res.groupID = bestGroup;
        res.w1 = (float)std::clamp(bestS, 0.0, 1.0);
        res.w2 = (float)std::clamp(bestT, 0.0, 1.0);
        res.w0 = (float)std::clamp(1.0 - bestS - bestT, 0.0, 1.0);
        float sum = res.w0 + res.w1 + res.w2;
        if (sum > 1e-6f) { res.w0 /= sum; res.w1 /= sum; res.w2 /= sum; }
        return res;
    }

    uint32_t findClosestPolyGroup(const float point[3], const float quadNormal[3]) const {
        return findClosestTriangle(point, quadNormal).groupID;
    }
};

// ---------------------------------------------------------
// Voxelization & Flood Fill
// ---------------------------------------------------------

static void voxelize(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    const float* colors,
    const float* materials,
    const uint32_t* faceGroups,
    VoxelGrid& voxels,
    std::function<void(int stage, int progress)> onProgress = nullptr,
    bool keepSharpEdges = false
) {
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;
    int datalen = rx * ry * rz;

    sculpt_log("[C++ voxelize] Starting voxelization: input nbVerts=%d, nbTris=%d, grid rx=%d, ry=%d, rz=%d, datalen=%d\n",
               nbVerts, nbTris, rx, ry, rz, datalen);

    double step = voxels.step;
    double invStep = 1.0 / step;
    double vminx = voxels.minCoord[0];
    double vminy = voxels.minCoord[1];
    double vminz = voxels.minCoord[2];

    double dirUnit[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}
    };

    double inv3 = 1.0 / 3.0;

    // 1. Precompute Z slice bounds and triangle metadata, then build CSR slice buckets
    struct TriVoxelMeta {
        double v1[3];
        double triEdge1[3];
        double triEdge2[3];
        double a00, a01, a11;
        float normal[3];
        int snapMinx, snapMaxx;
        int snapMiny, snapMaxy;
        float c1[3];
        float m1[3];
    };
    std::vector<TriVoxelMeta> triMeta(nbTris);
    std::vector<int> triMinZ(nbTris);
    std::vector<int> triMaxZ(nbTris);
    std::vector<int> sliceCounts(rz, 0);

#pragma omp parallel
    {
        std::vector<int> localCounts(rz, 0);
#pragma omp for schedule(static)
        for (int iTri = 0; iTri < nbTris; ++iTri) {
            int idTri = iTri * 3;
            uint32_t iv1 = tris[idTri] * 3;
            uint32_t iv2 = tris[idTri + 1] * 3;
            uint32_t iv3 = tris[idTri + 2] * 3;

            double v1[3] = { verts[iv1], verts[iv1 + 1], verts[iv1 + 2] };
            double v2[3] = { verts[iv2], verts[iv2 + 1], verts[iv2 + 2] };
            double v3[3] = { verts[iv3], verts[iv3 + 1], verts[iv3 + 2] };

            double zmin = std::min({ v1[2], v2[2], v3[2] });
            double zmax = std::max({ v1[2], v2[2], v3[2] });

            int snapMinz = (int)std::floor((zmin - vminz) * invStep);
            int snapMaxz = (int)std::ceil((zmax - vminz) * invStep);

            int kMin = std::max(0, snapMinz);
            int kMax = std::min(rz - 1, snapMaxz);

            triMinZ[iTri] = kMin;
            triMaxZ[iTri] = kMax;

            if (kMin <= kMax) {
                for (int k = kMin; k <= kMax; ++k) {
                    localCounts[k]++;
                }

                double xmin = std::min({v1[0], v2[0], v3[0]});
                double xmax = std::max({v1[0], v2[0], v3[0]});
                double ymin = std::min({v1[1], v2[1], v3[1]});
                double ymax = std::max({v1[1], v2[1], v3[1]});

                TriVoxelMeta& tm = triMeta[iTri];
                tm.v1[0] = v1[0]; tm.v1[1] = v1[1]; tm.v1[2] = v1[2];

                tm.triEdge1[0] = v2[0] - v1[0];
                tm.triEdge1[1] = v2[1] - v1[1];
                tm.triEdge1[2] = v2[2] - v1[2];

                tm.triEdge2[0] = v3[0] - v1[0];
                tm.triEdge2[1] = v3[1] - v1[1];
                tm.triEdge2[2] = v3[2] - v1[2];

                tm.a00 = tm.triEdge1[0] * tm.triEdge1[0] + tm.triEdge1[1] * tm.triEdge1[1] + tm.triEdge1[2] * tm.triEdge1[2];
                tm.a01 = tm.triEdge1[0] * tm.triEdge2[0] + tm.triEdge1[1] * tm.triEdge2[1] + tm.triEdge1[2] * tm.triEdge2[2];
                tm.a11 = tm.triEdge2[0] * tm.triEdge2[0] + tm.triEdge2[1] * tm.triEdge2[1] + tm.triEdge2[2] * tm.triEdge2[2];

                double nrmX = tm.triEdge1[1] * tm.triEdge2[2] - tm.triEdge1[2] * tm.triEdge2[1];
                double nrmY = tm.triEdge1[2] * tm.triEdge2[0] - tm.triEdge1[0] * tm.triEdge2[2];
                double nrmZ = tm.triEdge1[0] * tm.triEdge2[1] - tm.triEdge1[1] * tm.triEdge2[0];
                double lenN = std::sqrt(nrmX * nrmX + nrmY * nrmY + nrmZ * nrmZ);
                if (lenN > 1e-12) {
                    double invL = 1.0 / lenN;
                    tm.normal[0] = (float)(nrmX * invL);
                    tm.normal[1] = (float)(nrmY * invL);
                    tm.normal[2] = (float)(nrmZ * invL);
                } else {
                    tm.normal[0] = 0.0f; tm.normal[1] = 0.0f; tm.normal[2] = 1.0f;
                }

                int snapMinx = (int)std::floor((xmin - vminx) * invStep);
                int snapMiny = (int)std::floor((ymin - vminy) * invStep);
                int snapMaxx = (int)std::ceil((xmax - vminx) * invStep);
                int snapMaxy = (int)std::ceil((ymax - vminy) * invStep);

                tm.snapMinx = std::max(0, snapMinx);
                tm.snapMiny = std::max(0, snapMiny);
                tm.snapMaxx = std::min(rx - 1, snapMaxx);
                tm.snapMaxy = std::min(ry - 1, snapMaxy);

                if (colors) {
                    tm.c1[0] = (colors[iv1] + colors[iv2] + colors[iv3]) * (float)inv3;
                    tm.c1[1] = (colors[iv1 + 1] + colors[iv2 + 1] + colors[iv3 + 1]) * (float)inv3;
                    tm.c1[2] = (colors[iv1 + 2] + colors[iv2 + 2] + colors[iv3 + 2]) * (float)inv3;
                } else {
                    tm.c1[0] = tm.c1[1] = tm.c1[2] = 0.0f;
                }

                if (materials) {
                    tm.m1[0] = (materials[iv1] + materials[iv2] + materials[iv3]) * (float)inv3;
                    tm.m1[1] = (materials[iv1 + 1] + materials[iv2 + 1] + materials[iv3 + 1]) * (float)inv3;
                    tm.m1[2] = (materials[iv1 + 2] + materials[iv2 + 2] + materials[iv3 + 2]) * (float)inv3;
                } else {
                    tm.m1[0] = tm.m1[1] = tm.m1[2] = 0.0f;
                }
            }
        }
#pragma omp critical
        {
            for (int k = 0; k < rz; ++k) {
                sliceCounts[k] += localCounts[k];
            }
        }
    }

    std::vector<int> sliceOffsets(rz + 1, 0);
    for (int k = 0; k < rz; ++k) {
        sliceOffsets[k + 1] = sliceOffsets[k] + sliceCounts[k];
    }

    int totalPairs = sliceOffsets[rz];
    std::vector<int> sliceTriangles(totalPairs);
    std::vector<int> currentOffsets = sliceOffsets;

    for (int iTri = 0; iTri < nbTris; ++iTri) {
        int kMin = triMinZ[iTri];
        int kMax = triMaxZ[iTri];
        if (kMin <= kMax) {
            for (int k = kMin; k <= kMax; ++k) {
                sliceTriangles[currentOffsets[k]++] = iTri;
            }
        }
    }

    // 2. Parallel Slice-by-Slice Voxelization
#pragma omp parallel for schedule(dynamic, 1)
    for (int k = 0; k < rz; ++k) {
        int startIdx = sliceOffsets[k];
        int endIdx = sliceOffsets[k + 1];
        if (startIdx >= endIdx) continue;

        double z = vminz + k * step;
        int kOffset = k * rxy;

        for (int idx = startIdx; idx < endIdx; ++idx) {
            int iTri = sliceTriangles[idx];
            const TriVoxelMeta& tm = triMeta[iTri];

            for (int j = tm.snapMiny; j <= tm.snapMaxy; ++j) {
                double y = vminy + j * step;
                int n = tm.snapMinx + j * rx + kOffset;
                for (int i = tm.snapMinx; i <= tm.snapMaxx; ++i, ++n) {
                    double x = vminx + i * step;

                    double point[3] = { x, y, z };
                    double closest[5];
                    double newDistSq = distance2PointTriangleEdges(point, tm.triEdge1, tm.triEdge2, tm.v1, tm.a00, tm.a01, tm.a11, closest);
                    float curDistSq = voxels.distanceField[n];

                    if ((float)newDistSq < curDistSq) {
                        voxels.distanceField[n] = (float)newDistSq;
                        if (voxels.hasColorField) {
                            uint8_t r = (uint8_t)(tm.c1[0] * 255.0f + 0.5f);
                            uint8_t g = (uint8_t)(tm.c1[1] * 255.0f + 0.5f);
                            uint8_t b = (uint8_t)(tm.c1[2] * 255.0f + 0.5f);
                            voxels.colorField[n] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        }
                        if (voxels.hasMaterialField) {
                            uint8_t r = (uint8_t)(tm.m1[0] * 255.0f + 0.5f);
                            uint8_t g = (uint8_t)(tm.m1[1] * 255.0f + 0.5f);
                            uint8_t b = (uint8_t)(tm.m1[2] * 255.0f + 0.5f);
                            voxels.materialField[n] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        }
                        if (voxels.hasGroupField && faceGroups) {
                            voxels.groupField[n] = faceGroups[iTri];
                        }
                    }

                    if (newDistSq > step * step)
                        continue;

                    for (int it = 0; it < 3; ++it) {
                        double val = closest[it] - point[it];
                        if (val < 0.0 || val > step)
                            continue;

                        int bit = 1 << it;
                        if ((voxels.crossedEdges[n] & bit) != 0) {
                            if (keepSharpEdges && !voxels.hermiteEdges.empty()) {
                                double dist = intersectionRayTriangleEdges(point, dirUnit[it], tm.triEdge1, tm.triEdge2, tm.v1);
                                if (dist >= 0.0 && dist <= step) {
                                    uint32_t edgeKey = (uint32_t)((j * rx + i) * 3 + it);
                                    auto itH = voxels.hermiteEdges[k].find(edgeKey);
                                    float newT = (float)(dist / step);
                                    if (itH != voxels.hermiteEdges[k].end() && newT < itH->second.t) {
                                        itH->second = { tm.normal[0], tm.normal[1], tm.normal[2], newT };
                                    }
                                }
                            }
                            continue;
                        }

                        double dist = intersectionRayTriangleEdges(point, dirUnit[it], tm.triEdge1, tm.triEdge2, tm.v1);
                        if (dist < 0.0 || dist > step)
                            continue;

                        voxels.crossedEdges[n] |= bit;

                        if (keepSharpEdges && !voxels.hermiteEdges.empty()) {
                            uint32_t edgeKey = (uint32_t)((j * rx + i) * 3 + it);
                            voxels.hermiteEdges[k][edgeKey] = {
                                tm.normal[0], tm.normal[1], tm.normal[2],
                                (float)(dist / step)
                            };
                        }
                    }
                }
            }
        }
    }

    // 3. Vectorized OpenMP pass: convert squared distances to linear Euclidean distances
    int nonInfCount = 0;
#pragma omp parallel for schedule(static) reduction(+:nonInfCount)
    for (int i = 0; i < datalen; ++i) {
        float dSq = voxels.distanceField[i];
        if (dSq != std::numeric_limits<float>::infinity()) {
            voxels.distanceField[i] = std::sqrt(dSq);
            nonInfCount++;
        }
    }
    sculpt_log("[C++ voxelize] Finished voxelization: non-inf distances count=%d\n", nonInfCount);
}

static constexpr uint8_t TAG_EXTERIOR = 0x80;

static void floodFill(VoxelGrid& voxels, std::function<void(int stage, int progress)> onProgress = nullptr) {
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;
    int datalen = rx * ry * rz;

    sculpt_log("[C++ floodFill] Starting fast 3D topological sweep flood fill (bit-packed in crossedEdges): datalen=%d (dims: %dx%dx%d)\n", datalen, rx, ry, rz);

    if (onProgress) onProgress(1, 0);

    // Initialize entire outer 3D boundary grid as exterior (TAG_EXTERIOR bit in crossedEdges)
    for (int z = 0; z < rz; ++z) {
        for (int y = 0; y < ry; ++y) {
            voxels.crossedEdges[0 + y * rx + z * rxy] |= TAG_EXTERIOR;
            voxels.crossedEdges[(rx - 1) + y * rx + z * rxy] |= TAG_EXTERIOR;
        }
    }
    for (int z = 0; z < rz; ++z) {
        for (int x = 0; x < rx; ++x) {
            voxels.crossedEdges[x + 0 * rx + z * rxy] |= TAG_EXTERIOR;
            voxels.crossedEdges[x + (ry - 1) * rx + z * rxy] |= TAG_EXTERIOR;
        }
    }
    for (int y = 0; y < ry; ++y) {
        for (int x = 0; x < rx; ++x) {
            voxels.crossedEdges[x + y * rx + 0 * rxy] |= TAG_EXTERIOR;
            voxels.crossedEdges[x + y * rx + (rz - 1) * rxy] |= TAG_EXTERIOR;
        }
    }

    // Pass 1: Forward Sweep (0 -> max)
    int n = 0;
    for (int z = 0; z < rz; ++z) {
        for (int y = 0; y < ry; ++y) {
            for (int x = 0; x < rx; ++x, ++n) {
                if (voxels.crossedEdges[n] & TAG_EXTERIOR) continue;
                if (x > 0 && (voxels.crossedEdges[n - 1] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - 1] & 1)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                } else if (y > 0 && (voxels.crossedEdges[n - rx] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - rx] & 2)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                } else if (z > 0 && (voxels.crossedEdges[n - rxy] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - rxy] & 4)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                }
            }
        }
    }

    if (onProgress) onProgress(1, 30);

    // Pass 2: Backward Sweep (max -> 0)
    n = datalen - 1;
    for (int z = rz - 1; z >= 0; --z) {
        for (int y = ry - 1; y >= 0; --y) {
            for (int x = rx - 1; x >= 0; --x, --n) {
                if (voxels.crossedEdges[n] & TAG_EXTERIOR) continue;
                if (x < rx - 1 && (voxels.crossedEdges[n + 1] & TAG_EXTERIOR) && !(voxels.crossedEdges[n] & 1)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                } else if (y < ry - 1 && (voxels.crossedEdges[n + rx] & TAG_EXTERIOR) && !(voxels.crossedEdges[n] & 2)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                } else if (z < rz - 1 && (voxels.crossedEdges[n + rxy] & TAG_EXTERIOR) && !(voxels.crossedEdges[n] & 4)) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                }
            }
        }
    }

    if (onProgress) onProgress(1, 60);

    // Pass 3: Forward Sweep (0 -> max) & collect frontier into queue
    std::vector<int32_t> queue;
    n = 0;
    for (int z = 0; z < rz; ++z) {
        for (int y = 0; y < ry; ++y) {
            for (int x = 0; x < rx; ++x, ++n) {
                if (voxels.crossedEdges[n] & TAG_EXTERIOR) continue;
                bool newlyTagged = false;
                if (x > 0 && (voxels.crossedEdges[n - 1] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - 1] & 1)) {
                    newlyTagged = true;
                } else if (y > 0 && (voxels.crossedEdges[n - rx] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - rx] & 2)) {
                    newlyTagged = true;
                } else if (z > 0 && (voxels.crossedEdges[n - rxy] & TAG_EXTERIOR) && !(voxels.crossedEdges[n - rxy] & 4)) {
                    newlyTagged = true;
                }

                if (newlyTagged) {
                    voxels.crossedEdges[n] |= TAG_EXTERIOR;
                    queue.push_back(n);
                }
            }
        }
    }

    // Final Queue Cleanup for any deep 3D labyrinths
    int dirs[6] = { -1, 1, -rx, rx, -rxy, rxy };
    int dirsEdge[6] = { 0, 0, 1, 1, 2, 2 };

    size_t head = 0;
    while (head < queue.size()) {
        int cell = queue[head++];
        for (int i = 0; i < 6; ++i) {
            int off = dirs[i];
            int idNext = cell + off;
            if (idNext < 0 || idNext >= datalen) continue;
            if (voxels.crossedEdges[idNext] & TAG_EXTERIOR) continue;

            int idx = (off >= 0) ? cell : idNext;
            int bit = 1 << dirsEdge[i];
            if ((voxels.crossedEdges[idx] & bit) == 0) {
                voxels.crossedEdges[idNext] |= TAG_EXTERIOR;
                queue.push_back(idNext);
            }
        }
    }

    // Branchless OpenMP sign flip: negate interior cells
    int taggedCount = 0;
#pragma omp parallel for schedule(static) reduction(+:taggedCount)
    for (int id = 0; id < datalen; ++id) {
        float d = voxels.distanceField[id];
        bool tagged = (voxels.crossedEdges[id] & TAG_EXTERIOR) != 0;
        voxels.distanceField[id] = tagged ? d : -d;
        taggedCount += (int)tagged;
    }

    sculpt_log("[C++ floodFill] Finished: tagged (exterior) cells=%d, untagged (interior) cells=%d\n", taggedCount, datalen - taggedCount);

    // Phase 1 Optimization: crossedEdges is no longer needed after floodFill.
    // Deallocate crossedEdges immediately to reclaim ~1.01 GB RAM before surface reconstruction.
    std::vector<uint8_t>().swap(voxels.crossedEdges);

    if (onProgress) onProgress(1, 100);
}

// ---------------------------------------------------------
// Marching Cubes Constants & Tables
// ---------------------------------------------------------

static const uint32_t mcEdgeTable[256] = {
    0x0, 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c, 0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
    0x190, 0x99, 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c, 0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
    0x230, 0x339, 0x33, 0x13a, 0x636, 0x73f, 0x435, 0x53c, 0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
    0x3a0, 0x2a9, 0x1a3, 0xaa, 0x7a6, 0x6af, 0x5a5, 0x4ac, 0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
    0x460, 0x569, 0x663, 0x76a, 0x66, 0x16f, 0x265, 0x36c, 0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
    0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff, 0x3f5, 0x2fc, 0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
    0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55, 0x15c, 0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
    0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc, 0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
    0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc, 0xcc, 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
    0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c, 0x15c, 0x55, 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
    0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc, 0x2fc, 0x3f5, 0xff, 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
    0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c, 0x36c, 0x265, 0x16f, 0x66, 0x76a, 0x663, 0x569, 0x460,
    0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac, 0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa, 0x1a3, 0x2a9, 0x3a0,
    0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c, 0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33, 0x339, 0x230,
    0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c, 0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99, 0x190,
    0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c, 0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0
};

static const int mcTriTable[256][16] = {
    {-1}, {0, 8, 3, -1}, {0, 1, 9, -1}, {1, 8, 3, 9, 8, 1, -1}, {1, 2, 10, -1}, {0, 8, 3, 1, 2, 10, -1},
    {9, 2, 10, 0, 2, 9, -1}, {2, 8, 3, 2, 10, 8, 10, 9, 8, -1}, {3, 11, 2, -1}, {0, 11, 2, 8, 11, 0, -1},
    {1, 9, 0, 2, 3, 11, -1}, {1, 11, 2, 1, 9, 11, 9, 8, 11, -1}, {3, 10, 1, 11, 10, 3, -1}, {0, 10, 1, 0, 8, 10, 8, 11, 10, -1},
    {3, 9, 0, 3, 11, 9, 11, 10, 9, -1}, {9, 8, 10, 10, 8, 11, -1}, {4, 7, 8, -1}, {4, 3, 0, 7, 3, 4, -1},
    {0, 1, 9, 8, 4, 7, -1}, {4, 1, 9, 4, 7, 1, 7, 3, 1, -1}, {1, 2, 10, 8, 4, 7, -1}, {3, 4, 7, 3, 0, 4, 1, 2, 10, -1},
    {9, 2, 10, 9, 0, 2, 8, 4, 7, -1}, {2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1}, {8, 4, 7, 3, 11, 2, -1}, {11, 4, 7, 11, 2, 4, 2, 0, 4, -1},
    {9, 0, 1, 8, 4, 7, 2, 3, 11, -1}, {4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1}, {3, 10, 1, 3, 11, 10, 7, 8, 4, -1},
    {1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1}, {4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1}, {4, 7, 11, 4, 11, 9, 9, 11, 10, -1},
    {9, 5, 4, -1}, {9, 5, 4, 0, 8, 3, -1}, {0, 5, 4, 1, 5, 0, -1}, {8, 5, 4, 8, 3, 5, 3, 1, 5, -1},
    {1, 2, 10, 9, 5, 4, -1}, {3, 0, 8, 1, 2, 10, 4, 9, 5, -1}, {5, 2, 10, 5, 4, 2, 4, 0, 2, -1}, {2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1},
    {9, 5, 4, 2, 3, 11, -1}, {0, 11, 2, 0, 8, 11, 4, 9, 5, -1}, {0, 5, 4, 0, 1, 5, 2, 3, 11, -1}, {2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1},
    {10, 3, 11, 10, 1, 3, 9, 5, 4, -1}, {4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1}, {5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1}, {5, 4, 8, 5, 8, 10, 10, 8, 11, -1},
    {9, 7, 8, 5, 7, 9, -1}, {9, 3, 0, 9, 5, 3, 5, 7, 3, -1}, {0, 7, 8, 0, 1, 7, 1, 5, 7, -1}, {1, 5, 3, 3, 5, 7, -1},
    {9, 7, 8, 9, 5, 7, 10, 1, 2, -1}, {10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1}, {8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1}, {2, 10, 5, 2, 5, 3, 3, 5, 7, -1},
    {7, 9, 5, 7, 8, 9, 3, 11, 2, -1}, {9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1}, {2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1}, {11, 2, 1, 11, 1, 7, 7, 1, 5, -1},
    {9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1}, {5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
    {11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1}, {11, 10, 5, 7, 11, 5, -1}, {10, 6, 5, -1}, {0, 8, 3, 5, 10, 6, -1},
    {9, 0, 1, 5, 10, 6, -1}, {1, 8, 3, 1, 9, 8, 5, 10, 6, -1}, {1, 6, 5, 2, 6, 1, -1}, {1, 6, 5, 1, 2, 6, 3, 0, 8, -1},
    {9, 6, 5, 9, 0, 6, 0, 2, 6, -1}, {5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1}, {2, 3, 11, 10, 6, 5, -1}, {11, 0, 8, 11, 2, 0, 10, 6, 5, -1},
    {0, 1, 9, 2, 3, 11, 5, 10, 6, -1}, {5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1}, {6, 3, 11, 6, 5, 3, 5, 1, 3, -1},
    {0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1}, {3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1}, {6, 5, 9, 6, 9, 11, 11, 9, 8, -1},
    {5, 10, 6, 4, 7, 8, -1}, {4, 3, 0, 4, 7, 3, 6, 5, 10, -1}, {1, 9, 0, 5, 10, 6, 8, 4, 7, -1}, {10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1},
    {6, 1, 2, 6, 5, 1, 4, 7, 8, -1}, {1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1}, {8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1},
    {7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1}, {3, 11, 2, 7, 8, 4, 10, 6, 5, -1}, {5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1},
    {0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1}, {9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
    {8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1}, {5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
    {0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1}, {6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1},
    {10, 4, 9, 6, 4, 10, -1}, {4, 10, 6, 4, 9, 10, 0, 8, 3, -1}, {10, 0, 1, 10, 6, 0, 6, 4, 0, -1},
    {8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1}, {1, 4, 9, 1, 2, 4, 2, 6, 4, -1}, {3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1},
    {0, 2, 4, 4, 2, 6, -1}, {8, 3, 2, 8, 2, 4, 4, 2, 6, -1}, {10, 4, 9, 10, 6, 4, 11, 2, 3, -1},
    {0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1}, {3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1},
    {6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1}, {9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1},
    {8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1}, {3, 11, 6, 3, 6, 0, 0, 6, 4, -1}, {6, 4, 8, 11, 6, 8, -1},
    {7, 10, 6, 7, 8, 10, 8, 9, 10, -1}, {0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1}, {10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1},
    {10, 6, 7, 10, 7, 1, 1, 7, 3, -1}, {1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1}, {2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
    {7, 8, 0, 7, 0, 6, 6, 0, 2, -1}, {7, 3, 2, 6, 7, 2, -1}, {2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1},
    {2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1}, {1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
    {11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1}, {8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
    {0, 9, 1, 11, 6, 7, -1}, {7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1}, {7, 11, 6, -1}, {7, 6, 11, -1},
    {3, 0, 8, 11, 7, 6, -1}, {0, 1, 9, 11, 7, 6, -1}, {8, 1, 9, 8, 3, 1, 11, 7, 6, -1}, {10, 1, 2, 6, 11, 7, -1},
    {1, 2, 10, 3, 0, 8, 6, 11, 7, -1}, {2, 9, 0, 2, 10, 9, 6, 11, 7, -1}, {6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1},
    {7, 2, 3, 6, 2, 7, -1}, {7, 0, 8, 7, 6, 0, 6, 2, 0, -1}, {2, 7, 6, 2, 3, 7, 0, 1, 9, -1}, {1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1},
    {10, 7, 6, 10, 1, 7, 1, 3, 7, -1}, {10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1}, {0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1},
    {7, 6, 10, 7, 10, 8, 8, 10, 9, -1}, {6, 8, 4, 11, 8, 6, -1}, {3, 6, 11, 3, 0, 6, 0, 4, 6, -1}, {8, 6, 11, 8, 4, 6, 9, 0, 1, -1},
    {9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1}, {6, 8, 4, 6, 11, 8, 2, 10, 1, -1}, {1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1},
    {4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1}, {10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
    {8, 2, 3, 8, 4, 2, 4, 6, 2, -1}, {0, 4, 2, 4, 6, 2, -1}, {1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1},
    {1, 9, 4, 1, 4, 2, 2, 4, 6, -1}, {8, 1, 3, 8, 6, 1, 8, 4, 6, 6, 10, 1, -1}, {10, 1, 0, 10, 0, 6, 6, 0, 4, -1},
    {4, 6, 3, 4, 3, 8, 6, 10, 3, 0, 3, 9, 10, 9, 3, -1}, {10, 9, 4, 6, 10, 4, -1}, {4, 9, 5, 7, 6, 11, -1},
    {0, 8, 3, 4, 9, 5, 11, 7, 6, -1}, {5, 0, 1, 5, 4, 0, 7, 6, 11, -1}, {11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1},
    {9, 5, 4, 10, 1, 2, 7, 6, 11, -1}, {6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1}, {7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1},
    {3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1}, {7, 2, 3, 7, 6, 2, 5, 4, 9, -1}, {9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1},
    {3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1}, {6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
    {9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1}, {1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
    {4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1}, {7, 6, 10, 7, 10, 8, 5, 4, 10, 4, 8, 10, -1},
    {6, 9, 5, 6, 11, 9, 11, 8, 9, -1}, {3, 6, 11, 0, 6, 3, 0, 5, 6, 0, 9, 5, -1}, {0, 11, 8, 0, 5, 11, 0, 1, 5, 5, 6, 11, -1},
    {6, 11, 3, 6, 3, 5, 5, 3, 1, -1}, {1, 2, 10, 9, 5, 11, 9, 11, 8, 11, 5, 6, -1}, {0, 11, 3, 0, 6, 11, 0, 9, 6, 5, 6, 9, 1, 2, 10, -1},
    {11, 8, 5, 11, 5, 6, 8, 0, 5, 10, 5, 2, 0, 2, 5, -1}, {6, 11, 3, 6, 3, 5, 2, 10, 3, 10, 5, 3, -1},
    {5, 8, 9, 5, 2, 8, 5, 6, 2, 3, 8, 2, -1}, {9, 5, 6, 9, 6, 0, 0, 6, 2, -1}, {1, 5, 8, 1, 8, 0, 5, 6, 8, 3, 8, 2, 6, 2, 8, -1},
    {1, 5, 6, 2, 1, 6, -1}, {1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1}, {10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1},
    {0, 3, 8, 5, 6, 10, -1}, {10, 5, 6, -1}, {11, 5, 10, 7, 5, 11, -1}, {11, 5, 10, 11, 7, 5, 8, 3, 0, -1},
    {5, 11, 7, 5, 10, 11, 1, 9, 0, -1}, {10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1}, {11, 1, 2, 11, 7, 1, 7, 5, 1, -1},
    {0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1}, {9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1},
    {7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1}, {2, 5, 10, 2, 3, 5, 3, 7, 5, -1}, {8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1},
    {9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1}, {9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
    {1, 3, 5, 3, 7, 5, -1}, {0, 8, 7, 0, 7, 1, 1, 7, 5, -1}, {9, 0, 3, 9, 3, 5, 5, 3, 7, -1}, {9, 8, 7, 5, 9, 7, -1},
    {5, 8, 4, 5, 10, 8, 10, 11, 8, -1}, {5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1}, {0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1},
    {10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1}, {2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1},
    {0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1}, {0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
    {9, 4, 5, 2, 11, 3, -1}, {2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1}, {5, 10, 2, 5, 2, 4, 4, 2, 0, -1},
    {3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1}, {5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1},
    {8, 4, 5, 8, 5, 3, 3, 5, 1, -1}, {0, 4, 5, 1, 0, 5, -1}, {8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1},
    {9, 4, 5, -1}, {4, 11, 7, 4, 9, 11, 9, 10, 11, -1}, {0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1},
    {1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1}, {3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
    {4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1}, {9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
    {11, 7, 4, 11, 4, 2, 2, 4, 0, -1}, {11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1}, {2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1},
    {9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1}, {3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
    {1, 10, 2, 8, 7, 4, -1}, {4, 9, 1, 4, 1, 7, 7, 1, 3, -1}, {4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1},
    {4, 0, 3, 7, 4, 3, -1}, {4, 8, 7, -1}, {9, 10, 8, 10, 11, 8, -1}, {3, 0, 9, 3, 9, 11, 11, 9, 10, -1},
    {0, 1, 10, 0, 10, 8, 8, 10, 11, -1}, {3, 1, 10, 11, 3, 10, -1}, {1, 2, 11, 1, 11, 9, 9, 11, 8, -1},
    {3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1}, {0, 2, 11, 8, 0, 11, -1}, {3, 2, 11, -1},
    {2, 3, 8, 2, 8, 10, 10, 8, 9, -1}, {9, 10, 2, 0, 9, 2, -1}, {2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1},
    {1, 10, 2, -1}, {1, 3, 8, 9, 1, 8, -1}, {0, 9, 1, -1}, {0, 3, 8, -1}, {-1}
};

static const int mcCubeVerts[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
};

static const int mcEdgeIndex[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}
};

static uint8_t readScalarValuesMarchingCubes(
    VoxelGrid& voxels, float grid[8], int n,
    float outCols[3], float outMats[3]
) {
    float c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
    float m1 = 0.0f, m2 = 0.0f, m3 = 0.0f;
    float invSum = 0.0f;

    uint8_t mask = 0;
    int rx = voxels.dims[0];
    int rxy = rx * voxels.dims[1];

    for (int i = 0; i < 8; ++i) {
        const int* v = mcCubeVerts[i];
        int id = n + v[0] + v[1] * rx + v[2] * rxy;
        float p = voxels.distanceField[id];
        grid[i] = p;

        if (p > 0.0f) {
            mask |= (1 << i);
        }

        if (p != std::numeric_limits<float>::infinity()) {
            p = std::min(1.0f / std::abs(p), 1e15f);
            invSum += p;
            if (voxels.hasColorField) {
                uint32_t val = voxels.colorField[id];
                c1 += (float)((val >> 16) & 0xff) * p;
                c2 += (float)((val >> 8) & 0xff) * p;
                c3 += (float)(val & 0xff) * p;
            }
            if (voxels.hasMaterialField) {
                uint32_t val = voxels.materialField[id];
                m1 += (float)((val >> 16) & 0xff) * p;
                m2 += (float)((val >> 8) & 0xff) * p;
                m3 += (float)(val & 0xff) * p;
            }
        }
    }

    if (mcEdgeTable[mask] != 0) {
        if (invSum > 0.0f) invSum = 1.0f / invSum;
        if (voxels.hasColorField) {
            float inv255 = invSum / 255.0f;
            outCols[0] = c1 * inv255;
            outCols[1] = c2 * inv255;
            outCols[2] = c3 * inv255;
        } else {
            outCols[0] = voxels.uniformColor[0];
            outCols[1] = voxels.uniformColor[1];
            outCols[2] = voxels.uniformColor[2];
        }
        if (voxels.hasMaterialField) {
            float inv255 = invSum / 255.0f;
            outMats[0] = m1 * inv255;
            outMats[1] = m2 * inv255;
            outMats[2] = m3 * inv255;
        } else {
            outMats[0] = voxels.uniformMaterial[0];
            outMats[1] = voxels.uniformMaterial[1];
            outMats[2] = voxels.uniformMaterial[2];
        }
    }

    return mask;
}

static RemeshResult marchingCubesReconstruct(VoxelGrid& voxels, std::function<void(int stage, int progress)> onProgress = nullptr) {
    RemeshResult res;
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;

    // Phase 1 Optimization: 2-slice sliding window ring buffer for edge vertices.
    // Instead of 3 dense 3D arrays of size datalen (which consumes 12.15 GB on Res 1000),
    // Marching Cubes edge sharing only ever connects slice z and slice z + 1.
    // 2 slices per edge axis reduce memory to ~24 MB (99.8% reduction!).
    size_t sliceX = (size_t)(rx - 1) * ry;
    size_t sliceY = (size_t)rx * (ry - 1);
    size_t sliceZ = (size_t)rx * ry;

    std::vector<uint32_t> edgeVertX(2 * sliceX, 0xffffffff);
    std::vector<uint32_t> edgeVertY(2 * sliceY, 0xffffffff);
    std::vector<uint32_t> edgeVertZ(2 * sliceZ, 0xffffffff);
    res.profile.bytesReconstructAux = (2 * sliceX + 2 * sliceY + 2 * sliceZ) * sizeof(uint32_t);

    std::vector<size_t> dirtyX[2];
    std::vector<size_t> dirtyY[2];
    std::vector<size_t> dirtyZ[2];
    std::vector<uint32_t> vertGroups;

    int n = 0;
    float grid[8];
    uint32_t edges[12];

    float tmpV[3];
    float tmpC[3] = {0.0f, 0.0f, 0.0f};
    float tmpM[3] = {0.0f, 0.0f, 0.0f};

    float diagThreshold = 1.7321f * voxels.step;

    for (int z = 0; z < rz - 1; ++z, n += rx) {
        if (onProgress && (z % 10 == 0)) {
            int pct = (z * 100) / (rz - 1);
            onProgress(2, pct);
        }

        // Reset the bank that will be used for ez = z + 1 (last used for ez = z - 1 in slice z - 1)
        int bankNext = (z + 1) & 1;
        for (size_t idx : dirtyX[bankNext]) {
            edgeVertX[idx] = 0xffffffff;
        }
        dirtyX[bankNext].clear();

        for (size_t idx : dirtyY[bankNext]) {
            edgeVertY[idx] = 0xffffffff;
        }
        dirtyY[bankNext].clear();

        // Reset the bank that will be used for Z edges in this slice (last used in slice z - 2)
        int bankCur = z & 1;
        for (size_t idx : dirtyZ[bankCur]) {
            edgeVertZ[idx] = 0xffffffff;
        }
        dirtyZ[bankCur].clear();
        for (int y = 0; y < ry - 1; ++y, ++n) {
            for (int x = 0; x < rx - 1; ++x, ++n) {
                float d0 = voxels.distanceField[n];
                if (std::abs(d0) > diagThreshold) continue;

                int cubeIndex = readScalarValuesMarchingCubes(voxels, grid, n, tmpC, tmpM);
                uint32_t edgeMask = mcEdgeTable[cubeIndex];
                if (edgeMask == 0) continue;

                for (int k = 0; k < 12; ++k) {
                    if (!(edgeMask & (1 << k))) continue;

                    int ex = x;
                    int ey = y;
                    int ez = z;
                    int axis = 0; // 0=X, 1=Y, 2=Z

                    switch (k) {
                        case 0:  ex = x;     ey = y;     ez = z;     axis = 0; break;
                        case 1:  ex = x + 1; ey = y;     ez = z;     axis = 1; break;
                        case 2:  ex = x;     ey = y + 1; ez = z;     axis = 0; break;
                        case 3:  ex = x;     ey = y;     ez = z;     axis = 1; break;
                        case 4:  ex = x;     ey = y;     ez = z + 1; axis = 0; break;
                        case 5:  ex = x + 1; ey = y;     ez = z + 1; axis = 1; break;
                        case 6:  ex = x;     ey = y + 1; ez = z + 1; axis = 0; break;
                        case 7:  ex = x;     ey = y;     ez = z + 1; axis = 1; break;
                        case 8:  ex = x;     ey = y;     ez = z;     axis = 2; break;
                        case 9:  ex = x + 1; ey = y;     ez = z;     axis = 2; break;
                        case 10: ex = x + 1; ey = y + 1; ez = z;     axis = 2; break;
                        case 11: ex = x;     ey = y + 1; ez = z;     axis = 2; break;
                    }

                    uint32_t* slot = nullptr;
                    size_t slotIdx = 0;
                    if (axis == 0) {
                        slotIdx = (size_t)(ez & 1) * sliceX + (size_t)ex + (size_t)ey * (rx - 1);
                        slot = &edgeVertX[slotIdx];
                    } else if (axis == 1) {
                        slotIdx = (size_t)(ez & 1) * sliceY + (size_t)ex + (size_t)ey * rx;
                        slot = &edgeVertY[slotIdx];
                    } else {
                        slotIdx = (size_t)(ez & 1) * sliceZ + (size_t)ex + (size_t)ey * rx;
                        slot = &edgeVertZ[slotIdx];
                    }

                    if (*slot != 0xffffffff) {
                        edges[k] = *slot;
                    } else {
                        const int* e = mcEdgeIndex[k];
                        const int* p0 = mcCubeVerts[e[0]];
                        const int* p1 = mcCubeVerts[e[1]];
                        float a = grid[e[0]];
                        float b = grid[e[1]];
                        float d = a - b;
                        float t = 0.0f;
                        if (std::abs(d) > 1e-6f) {
                            t = a / d;
                        }

                        tmpV[0] = (float)x + (float)p0[0] + t * (float)(p1[0] - p0[0]);
                        tmpV[1] = (float)y + (float)p0[1] + t * (float)(p1[1] - p0[1]);
                        tmpV[2] = (float)z + (float)p0[2] + t * (float)(p1[2] - p0[2]);

                        uint32_t idVertex = res.vertices.size() / 3;
                        *slot = idVertex;
                        edges[k] = idVertex;
                        if (axis == 0) dirtyX[ez & 1].push_back(slotIdx);
                        else if (axis == 1) dirtyY[ez & 1].push_back(slotIdx);
                        else dirtyZ[ez & 1].push_back(slotIdx);
                        res.vertices.push_back(tmpV[0]);
                        res.vertices.push_back(tmpV[1]);
                        res.vertices.push_back(tmpV[2]);
                        res.colors.push_back(tmpC[0]);
                        res.colors.push_back(tmpC[1]);
                        res.colors.push_back(tmpC[2]);
                        res.materials.push_back(tmpM[0]);
                        res.materials.push_back(tmpM[1]);
                        res.materials.push_back(tmpM[2]);

                        if (voxels.hasGroupField) {
                            int node0 = (x + p0[0]) + (y + p0[1]) * rx + (z + p0[2]) * rxy;
                            int node1 = (x + p1[0]) + (y + p1[1]) * rx + (z + p1[2]) * rxy;
                            uint32_t grp = (std::abs(grid[e[0]]) <= std::abs(grid[e[1]])) ? voxels.groupField[node0] : voxels.groupField[node1];
                            vertGroups.push_back(grp);
                        }
                    }
                }

                // Add faces
                const int* f = mcTriTable[cubeIndex];
                for (int l = 0; f[l] != -1 && l < 16; l += 3) {
                    res.faces.push_back(edges[f[l]]);
                    res.faces.push_back(edges[f[l + 1]]);
                    res.faces.push_back(edges[f[l + 2]]);
                    res.faces.push_back(0xffffffff); // TRI_INDEX

                    if (voxels.hasGroupField && !vertGroups.empty()) {
                        uint32_t v0 = edges[f[l]];
                        res.faceGroups.push_back((size_t)v0 < vertGroups.size() ? vertGroups[v0] : 0);
                    }
                }
            }
        }
    }
    return res;
}

// ---------------------------------------------------------
// Surface Nets Constants & Tables
// ---------------------------------------------------------

static std::vector<uint32_t> computeCubeEdges() {
    std::vector<uint32_t> cubeEdges(24);
    int k = 0;
    for (int i = 0; i < 8; ++i) {
        for (int j = 1; j <= 4; j <<= 1) {
            int p = i ^ j;
            if (i <= p) {
                cubeEdges[k++] = i;
                cubeEdges[k++] = p;
            }
        }
    }
    return cubeEdges;
}

static std::vector<uint32_t> computeEdgeTable(const std::vector<uint32_t>& cubeEdges) {
    std::vector<uint32_t> edgeTable(256);
    for (int i = 0; i < 256; ++i) {
        uint32_t em = 0;
        for (int j = 0; j < 24; j += 2) {
            bool a = (i & (1 << cubeEdges[j])) != 0;
            bool b = (i & (1 << cubeEdges[j + 1])) != 0;
            if (a != b) {
                em |= (1 << (j >> 1));
            }
        }
        edgeTable[i] = em;
    }
    return edgeTable;
}

static const std::vector<uint32_t> snCubeEdges = computeCubeEdges();
static const std::vector<uint32_t> snEdgeTable = computeEdgeTable(snCubeEdges);

static uint8_t readScalarValuesSurfaceNets(
    VoxelGrid& voxels, float grid[8], int n,
    std::vector<float>& outCols, std::vector<float>& outMats,
    std::vector<uint32_t>& outGroups
) {
    uint8_t mask = 0;
    int rx = voxels.dims[0];
    int rxy = rx * voxels.dims[1];

    const int cornerOffsets[8] = {
        0, 1, rx, rx + 1,
        rxy, rxy + 1, rxy + rx, rxy + rx + 1
    };

    for (int g = 0; g < 8; ++g) {
        int id = n + cornerOffsets[g];
        float p = voxels.distanceField[id];
        grid[g] = p;
        if (p < 0.0f) {
            mask |= (1 << g);
        }
    }

    if (mask == 0 || mask == 0xff) {
        return mask;
    }

    float c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
    float m1 = 0.0f, m2 = 0.0f, m3 = 0.0f;
    float invSum = 0.0f;
    float minAbsDist = std::numeric_limits<float>::infinity();
    uint32_t closestGroup = 0;

    for (int g = 0; g < 8; ++g) {
        int id = n + cornerOffsets[g];
        float p = grid[g];
        if (p != std::numeric_limits<float>::infinity()) {
            float absP = std::abs(p);
            p = std::min(1.0f / absP, 1e15f);
            invSum += p;
            if (voxels.hasColorField) {
                uint32_t val = voxels.colorField[id];
                c1 += (float)((val >> 16) & 0xff) * p;
                c2 += (float)((val >> 8) & 0xff) * p;
                c3 += (float)(val & 0xff) * p;
            }
            if (voxels.hasMaterialField) {
                uint32_t val = voxels.materialField[id];
                m1 += (float)((val >> 16) & 0xff) * p;
                m2 += (float)((val >> 8) & 0xff) * p;
                m3 += (float)(val & 0xff) * p;
            }
            if (voxels.hasGroupField && absP < minAbsDist) {
                minAbsDist = absP;
                closestGroup = voxels.groupField[id];
            }
        }
    }

    if (invSum > 0.0f) invSum = 1.0f / invSum;
    if (voxels.hasColorField) {
        float inv255 = invSum / 255.0f;
        outCols.push_back(c1 * inv255);
        outCols.push_back(c2 * inv255);
        outCols.push_back(c3 * inv255);
    } else {
        outCols.push_back(voxels.uniformColor[0]);
        outCols.push_back(voxels.uniformColor[1]);
        outCols.push_back(voxels.uniformColor[2]);
    }
    if (voxels.hasMaterialField) {
        float inv255 = invSum / 255.0f;
        outMats.push_back(m1 * inv255);
        outMats.push_back(m2 * inv255);
        outMats.push_back(m3 * inv255);
    } else {
        outMats.push_back(voxels.uniformMaterial[0]);
        outMats.push_back(voxels.uniformMaterial[1]);
        outMats.push_back(voxels.uniformMaterial[2]);
    }
    if (voxels.hasGroupField) {
        outGroups.push_back(closestGroup);
    }

    return mask;
}

// 3x3 Real Symmetric Eigendecomposition via Jacobi Rotations
static void eigenSymmetric3x3(const float A[3][3], float eigenvalues[3], glm::vec3 eigenvectors[3]) {
    float V[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };
    float D[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            D[r][c] = A[r][c];
        }
    }

    for (int iter = 0; iter < 20; ++iter) {
        int p = 0, q = 1;
        float maxOff = std::abs(D[0][1]);
        if (std::abs(D[0][2]) > maxOff) { p = 0; q = 2; maxOff = std::abs(D[0][2]); }
        if (std::abs(D[1][2]) > maxOff) { p = 1; q = 2; maxOff = std::abs(D[1][2]); }

        if (maxOff < 1e-6f) break;

        float app = D[p][p];
        float aqq = D[q][q];
        float apq = D[p][q];

        float phi = 0.5f * std::atan2(2.0f * apq, aqq - app);
        float c = std::cos(phi);
        float s = std::sin(phi);

        for (int i = 0; i < 3; ++i) {
            if (i != p && i != q) {
                float aip = D[i][p];
                float aiq = D[i][q];
                D[i][p] = D[p][i] = c * aip - s * aiq;
                D[i][q] = D[q][i] = s * aip + c * aiq;
            }
        }
        D[p][p] = c * c * app - 2.0f * s * c * apq + s * s * aqq;
        D[q][q] = s * s * app + 2.0f * s * c * apq + c * c * aqq;
        D[p][q] = D[q][p] = 0.0f;

        for (int i = 0; i < 3; ++i) {
            float vip = V[i][p];
            float viq = V[i][q];
            V[i][p] = c * vip - s * viq;
            V[i][q] = s * vip + c * viq;
        }
    }

    eigenvalues[0] = D[0][0];
    eigenvalues[1] = D[1][1];
    eigenvalues[2] = D[2][2];

    for (int i = 0; i < 3; ++i) {
        eigenvectors[i] = glm::vec3(V[0][i], V[1][i], V[2][i]);
        float l = glm::length(eigenvectors[i]);
        if (l > 1e-6f) eigenvectors[i] /= l;
    }

    for (int i = 0; i < 2; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (eigenvalues[j] > eigenvalues[i]) {
                std::swap(eigenvalues[i], eigenvalues[j]);
                std::swap(eigenvectors[i], eigenvectors[j]);
            }
        }
    }
}

static bool interpolateVerticesSurfaceNets(
    const VoxelGrid& voxels,
    uint32_t edgeMask, const float grid[8], const int x[3],
    std::vector<float>& vertices, bool block,
    bool regularizeQuads, bool keepSharpEdges
) {
    if (block) {
        vertices.push_back((float)x[0] + 0.5f);
        vertices.push_back((float)x[1] + 0.5f);
        vertices.push_back((float)x[2] + 0.5f);
        return false;
    }

    if (!regularizeQuads && !keepSharpEdges) {
        float vTemp[3] = {0.0f, 0.0f, 0.0f};
        int edgeCount = 0;

        for (int i = 0; i < 12; ++i) {
            if (!(edgeMask & (1 << i)))
                continue;
            ++edgeCount;

            int e0 = snCubeEdges[i << 1];
            int e1 = snCubeEdges[(i << 1) + 1];
            float g0 = grid[e0];
            float t = g0 - grid[e1];
            if (std::abs(t) < 1e-7f)
                continue;
            t = g0 / t;

            for (int j = 0; j < 3; ++j) {
                int k = 1 << j;
                int a = e0 & k;
                if (a != (e1 & k))
                    vTemp[j] += a ? 1.0f - t : t;
                else
                    vTemp[j] += a ? 1.0f : 0.0f;
            }
        }

        if (edgeCount == 0) edgeCount = 1;
        float s = 1.0f / (float)edgeCount;
        for (int l = 0; l < 3; ++l) {
            vTemp[l] = (float)x[l] + s * vTemp[l];
        }
        vertices.push_back(vTemp[0]);
        vertices.push_back(vTemp[1]);
        vertices.push_back(vTemp[2]);
        return false;
    }

    // Regularized Quad Mode & Sharp Feature Preserving Mode: QEF Hermite Placement
    glm::vec3 points[12];
    glm::vec3 normals[12];
    glm::vec3 xMass(0.0f);
    int edgeCount = 0;

    for (int i = 0; i < 12; ++i) {
        if (!(edgeMask & (1 << i)))
            continue;

        int e0 = snCubeEdges[i << 1];
        int e1 = snCubeEdges[(i << 1) + 1];
        float g0 = grid[e0];
        float diff = g0 - grid[e1];
        if (std::abs(diff) < 1e-7f)
            continue;
        float t = g0 / diff;

        glm::vec3 pLocal(0.0f);
        for (int j = 0; j < 3; ++j) {
            int k = 1 << j;
            int a = e0 & k;
            if (a != (e1 & k))
                pLocal[j] = a ? (1.0f - t) : t;
            else
                pLocal[j] = a ? 1.0f : 0.0f;
        }

        // Exact trilinear gradient of cell at pLocal as default fallback
        float u = pLocal.x;
        float v = pLocal.y;
        float w = pLocal.z;
        float nx = (1.0f - v) * (1.0f - w) * (grid[1] - grid[0]) +
                   v * (1.0f - w) * (grid[3] - grid[2]) +
                   (1.0f - v) * w * (grid[5] - grid[4]) +
                   v * w * (grid[7] - grid[6]);
        float ny = (1.0f - u) * (1.0f - w) * (grid[2] - grid[0]) +
                   u * (1.0f - w) * (grid[3] - grid[1]) +
                   (1.0f - u) * w * (grid[6] - grid[4]) +
                   u * w * (grid[7] - grid[5]);
        float nz = (1.0f - u) * (1.0f - v) * (grid[4] - grid[0]) +
                   u * (1.0f - v) * (grid[5] - grid[1]) +
                   (1.0f - u) * v * (grid[6] - grid[2]) +
                   u * v * (grid[7] - grid[3]);

        glm::vec3 norm(nx, ny, nz);
        float nlen = glm::length(norm);
        if (nlen > 1e-6f) {
            norm /= nlen;
        } else {
            norm = glm::vec3(0.0f, 0.0f, 1.0f);
        }

        // If keepSharpEdges is on, lookup original triangle normal and intersection point
        if (keepSharpEdges && !voxels.hermiteEdges.empty()) {
            int dx0 = e0 & 1;
            int dy0 = (e0 >> 1) & 1;
            int dz0 = (e0 >> 2) & 1;
            int diffBits = e0 ^ e1;
            int axis = (diffBits == 1) ? 0 : ((diffBits == 2) ? 1 : 2);
            int gx = x[0] + dx0;
            int gy = x[1] + dy0;
            int gz = x[2] + dz0;

            if (gz >= 0 && gz < voxels.dims[2] && gy >= 0 && gy < voxels.dims[1] && gx >= 0 && gx < voxels.dims[0]) {
                uint32_t edgeKey = (uint32_t)((gy * voxels.dims[0] + gx) * 3 + axis);
                const auto& sliceMap = voxels.hermiteEdges[gz];
                auto itH = sliceMap.find(edgeKey);
                if (itH != sliceMap.end()) {
                    norm = glm::vec3(itH->second.nx, itH->second.ny, itH->second.nz);
                    pLocal = glm::vec3((float)dx0, (float)dy0, (float)dz0);
                    pLocal[axis] = itH->second.t;
                }
            }
        }

        points[edgeCount] = pLocal;
        normals[edgeCount] = norm;
        xMass += pLocal;
        ++edgeCount;
    }

    if (edgeCount == 0) {
        vertices.push_back((float)x[0] + 0.5f);
        vertices.push_back((float)x[1] + 0.5f);
        vertices.push_back((float)x[2] + 0.5f);
        return false;
    }

    // Sharp Feature Detection: cell with high normal divergence among intersected triangles
    float minDot = 1.0f;
    for (int i = 0; i < edgeCount; ++i) {
        for (int j = i + 1; j < edgeCount; ++j) {
            float d = glm::dot(normals[i], normals[j]);
            if (d < minDot) {
                minDot = d;
            }
        }
    }
    bool isSharp = (keepSharpEdges && edgeCount >= 2 && minDot <= 0.85f);

    xMass /= (float)edgeCount;

    // Minimize E(x) = sum (n_i . (x - p_i))^2 (+ alpha * ||x - xMass||^2)
    // Let y = x - xMass => (sum n_i n_i^T + alpha * I) y = sum n_i (n_i . (p_i - xMass))
    float A00 = 0.0f, A01 = 0.0f, A02 = 0.0f;
    float A11 = 0.0f, A12 = 0.0f, A22 = 0.0f;
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;

    for (int k = 0; k < edgeCount; ++k) {
        const glm::vec3& n = normals[k];
        glm::vec3 d = points[k] - xMass;
        float dot = glm::dot(n, d);

        A00 += n.x * n.x;
        A01 += n.x * n.y;
        A02 += n.x * n.z;
        A11 += n.y * n.y;
        A12 += n.y * n.z;
        A22 += n.z * n.z;

        b0 += n.x * dot;
        b1 += n.y * dot;
        b2 += n.z * dot;
    }

    glm::vec3 ySol(0.0f);

    if (isSharp) {
        // Feature Snapping & SVD QEF:
        // Spectral decomposition eliminates null-space drift along sharp edges (Rank 2)
        // and snaps corners (Rank 3) directly to the intersection point of the tangent planes.
        float A_mat[3][3] = {
            { A00, A01, A02 },
            { A01, A11, A12 },
            { A02, A12, A22 }
        };
        float eigenvalues[3];
        glm::vec3 eigenvectors[3];
        eigenSymmetric3x3(A_mat, eigenvalues, eigenvectors);

        glm::vec3 b(b0, b1, b2);
        float threshold = 0.10f * std::max(1e-4f, eigenvalues[0]);
        for (int i = 0; i < 3; ++i) {
            if (eigenvalues[i] >= threshold) {
                float coeff = glm::dot(eigenvectors[i], b) / (eigenvalues[i] + 1e-4f);
                ySol += coeff * eigenvectors[i];
            }
        }
    } else {
        const float alpha = 0.08f;
        float M00 = A00 + alpha;
        float M01 = A01;
        float M02 = A02;
        float M11 = A11 + alpha;
        float M12 = A12;
        float M22 = A22 + alpha;

        float C00 = M11 * M22 - M12 * M12;
        float C01 = M02 * M12 - M01 * M22;
        float C02 = M01 * M12 - M02 * M11;
        float C11 = M00 * M22 - M02 * M02;
        float C12 = M01 * M02 - M00 * M12;
        float C22 = M00 * M11 - M01 * M01;

        float det = M00 * C00 + M01 * C01 + M02 * C02;
        if (std::abs(det) > 1e-12f) {
            float invDet = 1.0f / det;
            ySol.x = invDet * (C00 * b0 + C01 * b1 + C02 * b2);
            ySol.y = invDet * (C01 * b0 + C11 * b1 + C12 * b2);
            ySol.z = invDet * (C02 * b0 + C12 * b1 + C22 * b2);
        }
    }

    glm::vec3 xSol = xMass + ySol;
    float clampMargin = isSharp ? 0.35f : 0.05f;
    xSol.x = std::clamp(xSol.x, -clampMargin, 1.0f + clampMargin);
    xSol.y = std::clamp(xSol.y, -clampMargin, 1.0f + clampMargin);
    xSol.z = std::clamp(xSol.z, -clampMargin, 1.0f + clampMargin);

    vertices.push_back((float)x[0] + xSol.x);
    vertices.push_back((float)x[1] + xSol.y);
    vertices.push_back((float)x[2] + xSol.z);

    return isSharp;
}

static RemeshResult surfaceNetsReconstruct(
    VoxelGrid& voxels, bool block, bool regularizeQuads,
    bool keepSharpEdges = false,
    std::function<void(int stage, int progress)> onProgress = nullptr
) {
    RemeshResult res;
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;
    int datalen = rx * ry * rz;

    int numThreads = (int)std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;

    // Phase 1 Optimization: Sliding Window 2-slice ring buffer for cellToVert.
    // Instead of allocating a 4.05 GB array for the entire 3D volume, quads generated at cell (x, y, z)
    // only ever reference vertices from slice z and slice z - 1.
    // 2 slices require only 2 * rxy * sizeof(int32_t) = ~8 MB (99.8% memory reduction!).
    std::vector<int32_t> sliceToVert(2 * rxy, -1);
    res.profile.bytesReconstructAux = 2 * (size_t)rxy * sizeof(int32_t);

    struct ThreadVertData {
        std::vector<float> vertices;
        std::vector<float> colors;
        std::vector<float> materials;
        std::vector<uint32_t> vertGroups;
        std::vector<int> vertCellIndices;
        std::vector<uint8_t> vertMasks;
        std::vector<uint8_t> isSharpVert;
    };
    std::vector<ThreadVertData> threadVerts(numThreads);

    int slicesPerThread = ((rz - 1) + numThreads - 1) / numThreads;
    float diagThreshold = 1.7321f * voxels.step;

    // Pass A: Parallel Vertex Generation with fast distance rejection
#pragma omp parallel num_threads(numThreads)
    {
        int t = omp_get_thread_num();
        int zStart = t * slicesPerThread;
        int zEnd = std::min(rz - 1, zStart + slicesPerThread);

        ThreadVertData& td = threadVerts[t];
        td.vertices.reserve(64000 * 3);
        td.colors.reserve(64000 * 3);
        td.materials.reserve(64000 * 3);
        if (voxels.hasGroupField) td.vertGroups.reserve(64000);
        td.vertCellIndices.reserve(64000);
        td.vertMasks.reserve(64000);
        if (keepSharpEdges) td.isSharpVert.reserve(64000);

        float grid[8];
        int x[3];

        for (x[2] = zStart; x[2] < zEnd; ++x[2]) {
            for (x[1] = 0; x[1] < ry - 1; ++x[1]) {
                int n = x[1] * rx + x[2] * rxy;
                for (x[0] = 0; x[0] < rx - 1; ++x[0], ++n) {
                    // FAST REJECTION: if center/corner0 distance exceeds bounding cube diagonal,
                    // the isosurface cannot intersect this cell.
                    float d0 = voxels.distanceField[n];
                    if (std::abs(d0) > diagThreshold) continue;

                    uint8_t mask = readScalarValuesSurfaceNets(voxels, grid, n, td.colors, td.materials, td.vertGroups);
                    if (mask == 0 || mask == 0xff) continue;

                    uint32_t edgeMask = snEdgeTable[mask];
                    bool isSharp = interpolateVerticesSurfaceNets(voxels, edgeMask, grid, x, td.vertices, block, regularizeQuads, keepSharpEdges);
                    td.vertCellIndices.push_back(n);
                    td.vertMasks.push_back(mask);
                    if (keepSharpEdges) {
                        td.isSharpVert.push_back(isSharp ? 1 : 0);
                    }
                }
            }
        }
    }

    // Merge Thread Vertices with parallel prefix sums
    std::vector<size_t> vertOffsets(numThreads + 1, 0);
    for (int t = 0; t < numThreads; ++t) {
        vertOffsets[t + 1] = vertOffsets[t] + threadVerts[t].vertCellIndices.size();
    }
    size_t totalVerts = vertOffsets[numThreads];

    res.vertices.resize(totalVerts * 3);
    res.colors.resize(totalVerts * 3);
    res.materials.resize(totalVerts * 3);
    if (keepSharpEdges) {
        res.sharpVertices.resize(totalVerts, 0);
    }
    std::vector<uint32_t> vertGroups;
    if (voxels.hasGroupField) vertGroups.resize(totalVerts);

    std::vector<int> allCellIndices(totalVerts);
    std::vector<uint8_t> allMasks(totalVerts);

#pragma omp parallel for schedule(static)
    for (int t = 0; t < numThreads; ++t) {
        const ThreadVertData& td = threadVerts[t];
        size_t baseVertIdx = vertOffsets[t];
        size_t count = td.vertCellIndices.size();

        if (count > 0) {
            std::memcpy(&res.vertices[baseVertIdx * 3], td.vertices.data(), count * 3 * sizeof(float));
            std::memcpy(&res.colors[baseVertIdx * 3], td.colors.data(), count * 3 * sizeof(float));
            std::memcpy(&res.materials[baseVertIdx * 3], td.materials.data(), count * 3 * sizeof(float));
            if (keepSharpEdges && !td.isSharpVert.empty()) {
                std::memcpy(&res.sharpVertices[baseVertIdx], td.isSharpVert.data(), count * sizeof(uint8_t));
            }
            if (voxels.hasGroupField && !td.vertGroups.empty()) {
                std::memcpy(&vertGroups[baseVertIdx], td.vertGroups.data(), count * sizeof(uint32_t));
            }
            std::memcpy(&allCellIndices[baseVertIdx], td.vertCellIndices.data(), count * sizeof(int));
            std::memcpy(&allMasks[baseVertIdx], td.vertMasks.data(), count * sizeof(uint8_t));
        }
    }

    if (keepSharpEdges) {
        size_t sharpCount = 0;
        for (uint8_t s : res.sharpVertices) sharpCount += s;
        sculpt_log("[Surface Nets] Sharp vertices identified: %zu / %zu (%.2f%%)\n",
                   sharpCount, totalVerts, totalVerts > 0 ? (float)sharpCount / totalVerts * 100.0f : 0.0f);
    }

    // Pass B: Sliding Window Face Generation across slices z
    // Precalculate vertex offset boundaries per slice z in O(totalVerts)
    std::vector<size_t> sliceVertStart(rz + 1, totalVerts);
    size_t curZ = 0;
    sliceVertStart[0] = 0;
    for (size_t v = 0; v < totalVerts; ++v) {
        size_t z = (size_t)(allCellIndices[v] / rxy);
        while (curZ < z) {
            curZ++;
            sliceVertStart[curZ] = v;
        }
    }
    for (size_t z = curZ + 1; z <= (size_t)rz; ++z) {
        sliceVertStart[z] = totalVerts;
    }

    const int dimOffsets[3] = { rx, rxy, 1 };
    const int dimOffsetsV[3] = { rxy, 1, rx };

    res.faces.reserve(totalVerts * 4);
    if (voxels.hasGroupField) res.faceGroups.reserve(totalVerts);

    for (int z = 0; z < rz; ++z) {
        // Clear bank that was used for slice z - 2 (only active vertices)
        if (z >= 2) {
            size_t vStartOld = sliceVertStart[z - 2];
            size_t vEndOld = sliceVertStart[z - 1];
            for (size_t v = vStartOld; v < vEndOld; ++v) {
                int nOld = allCellIndices[v];
                sliceToVert[((z - 2) & 1) * rxy + (nOld % rxy)] = -1;
            }
        }

        // Populate bank for slice z
        size_t vStartZ = sliceVertStart[z];
        size_t vEndZ = sliceVertStart[z + 1];
        for (size_t v = vStartZ; v < vEndZ; ++v) {
            int nCur = allCellIndices[v];
            sliceToVert[(z & 1) * rxy + (nCur % rxy)] = (int32_t)v;
        }

        // Generate quads for vertices in slice z
        for (size_t v = vStartZ; v < vEndZ; ++v) {
            int n = allCellIndices[v];
            uint8_t mask = allMasks[v];
            int32_t v0 = (int32_t)v;

            uint32_t edgeMask = snEdgeTable[mask];

            int rem = n - z * rxy;
            int y = rem / rx;
            int x = rem - y * rx;
            const int coords[3] = { x, y, z };

            for (int i = 0; i < 3; ++i) {
                if (!(edgeMask & (1 << i))) continue;

                int iu = (i + 1) % 3;
                int iv = (i + 2) % 3;

                if (coords[iu] == 0 || coords[iv] == 0) continue;

                int du = dimOffsets[i];
                int dv = dimOffsetsV[i];

                int n1 = (mask & 1) ? (n - du) : (n - dv);
                int n2 = n - du - dv;
                int n3 = (mask & 1) ? (n - dv) : (n - du);

                int z1 = n1 / rxy;
                int z2 = n2 / rxy;
                int z3 = n3 / rxy;

                int32_t v1 = sliceToVert[(z1 & 1) * rxy + (n1 % rxy)];
                int32_t v2 = sliceToVert[(z2 & 1) * rxy + (n2 % rxy)];
                int32_t v3 = sliceToVert[(z3 & 1) * rxy + (n3 % rxy)];

                if (v1 < 0 || v2 < 0 || v3 < 0) continue;

                res.faces.push_back(v0);
                res.faces.push_back(v1);
                res.faces.push_back(v2);
                res.faces.push_back(v3);

                if (voxels.hasGroupField && !vertGroups.empty()) {
                    uint32_t g0 = (size_t)v0 < vertGroups.size() ? vertGroups[v0] : 0;
                    uint32_t g1 = (size_t)v1 < vertGroups.size() ? vertGroups[v1] : 0;
                    uint32_t g2 = (size_t)v2 < vertGroups.size() ? vertGroups[v2] : 0;
                    uint32_t g3 = (size_t)v3 < vertGroups.size() ? vertGroups[v3] : 0;

                    uint32_t fg = g0;
                    if (g1 == g2 || g1 == g3) fg = g1;
                    else if (g2 == g3) fg = g2;
                    res.faceGroups.push_back(fg);
                }
            }
        }
    }

    return res;
}

// ---------------------------------------------------------
// Constrained Tangential Laplacian Relaxation & SDF Sampling
// ---------------------------------------------------------

static inline float sampleSDFScalar(const VoxelGrid& voxels, float gx, float gy, float gz) {
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;

    int x0 = std::clamp((int)std::floor(gx), 0, rx - 2);
    int y0 = std::clamp((int)std::floor(gy), 0, ry - 2);
    int z0 = std::clamp((int)std::floor(gz), 0, rz - 2);

    float u = std::clamp(gx - (float)x0, 0.0f, 1.0f);
    float v = std::clamp(gy - (float)y0, 0.0f, 1.0f);
    float w = std::clamp(gz - (float)z0, 0.0f, 1.0f);

    int n = x0 + y0 * rx + z0 * rxy;
    float d0 = voxels.distanceField[n];
    float d1 = voxels.distanceField[n + 1];
    float d2 = voxels.distanceField[n + rx];
    float d3 = voxels.distanceField[n + rx + 1];
    float d4 = voxels.distanceField[n + rxy];
    float d5 = voxels.distanceField[n + rxy + 1];
    float d6 = voxels.distanceField[n + rxy + rx];
    float d7 = voxels.distanceField[n + rxy + rx + 1];

    float infDist = 2.0f * voxels.step;
    auto clampDist = [infDist](float d) -> float {
        if (!std::isfinite(d)) {
            return (d < 0.0f) ? -infDist : infDist;
        }
        return d;
    };

    d0 = clampDist(d0);
    d1 = clampDist(d1);
    d2 = clampDist(d2);
    d3 = clampDist(d3);
    d4 = clampDist(d4);
    d5 = clampDist(d5);
    d6 = clampDist(d6);
    d7 = clampDist(d7);

    float c00 = d0 * (1.0f - u) + d1 * u;
    float c10 = d2 * (1.0f - u) + d3 * u;
    float c01 = d4 * (1.0f - u) + d5 * u;
    float c11 = d6 * (1.0f - u) + d7 * u;

    float c0 = c00 * (1.0f - v) + c10 * v;
    float c1 = c01 * (1.0f - v) + c11 * v;

    return c0 * (1.0f - w) + c1 * w;
}

static inline glm::vec3 sampleSDFGradient(const VoxelGrid& voxels, float gx, float gy, float gz) {
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;

    int x0 = std::clamp((int)std::floor(gx), 0, rx - 2);
    int y0 = std::clamp((int)std::floor(gy), 0, ry - 2);
    int z0 = std::clamp((int)std::floor(gz), 0, rz - 2);

    float u = std::clamp(gx - (float)x0, 0.0f, 1.0f);
    float v = std::clamp(gy - (float)y0, 0.0f, 1.0f);
    float w = std::clamp(gz - (float)z0, 0.0f, 1.0f);

    int n = x0 + y0 * rx + z0 * rxy;
    float d0 = voxels.distanceField[n];
    float d1 = voxels.distanceField[n + 1];
    float d2 = voxels.distanceField[n + rx];
    float d3 = voxels.distanceField[n + rx + 1];
    float d4 = voxels.distanceField[n + rxy];
    float d5 = voxels.distanceField[n + rxy + 1];
    float d6 = voxels.distanceField[n + rxy + rx];
    float d7 = voxels.distanceField[n + rxy + rx + 1];

    if (!std::isfinite(d0) || !std::isfinite(d1) || !std::isfinite(d2) || !std::isfinite(d3) ||
        !std::isfinite(d4) || !std::isfinite(d5) || !std::isfinite(d6) || !std::isfinite(d7)) {
        return glm::vec3(0.0f);
    }

    float nx = (1.0f - v) * (1.0f - w) * (d1 - d0) +
               v * (1.0f - w) * (d3 - d2) +
               (1.0f - v) * w * (d5 - d4) +
               v * w * (d7 - d6);
    float ny = (1.0f - u) * (1.0f - w) * (d2 - d0) +
               u * (1.0f - w) * (d3 - d1) +
               (1.0f - u) * w * (d6 - d4) +
               u * w * (d7 - d5);
    float nz = (1.0f - u) * (1.0f - v) * (d4 - d0) +
               u * (1.0f - v) * (d5 - d1) +
               (1.0f - u) * v * (d6 - d2) +
               u * v * (d7 - d3);

    glm::vec3 grad(nx, ny, nz);
    float len = glm::length(grad);
    if (len > 1e-6f) {
        return grad / len;
    }
    return glm::vec3(0.0f);
}

static void applyConstrainedTangentialRelaxation(
    RemeshResult& res,
    const VoxelGrid& voxels,
    int iterations,
    float lambda,
    bool alignSymmetry,
    bool keepSharpEdges = false
) {
    size_t nbV = res.vertices.size() / 3;
    size_t nbF = res.faces.size() / 4;
    if (nbV < 4 || nbF < 1 || iterations <= 0) return;

    // 1. Precompute vertex normals from mesh geometry as a reliable baseline
    std::vector<glm::vec3> vertNormals(nbV, glm::vec3(0.0f));
    for (size_t f = 0; f < nbF; ++f) {
        uint32_t i0 = res.faces[f * 4 + 0];
        uint32_t i1 = res.faces[f * 4 + 1];
        uint32_t i2 = res.faces[f * 4 + 2];
        uint32_t i3 = res.faces[f * 4 + 3];
        if (i0 >= nbV || i1 >= nbV || i2 >= nbV) continue;

        glm::vec3 p0(res.vertices[i0 * 3], res.vertices[i0 * 3 + 1], res.vertices[i0 * 3 + 2]);
        glm::vec3 p1(res.vertices[i1 * 3], res.vertices[i1 * 3 + 1], res.vertices[i1 * 3 + 2]);
        glm::vec3 p2(res.vertices[i2 * 3], res.vertices[i2 * 3 + 1], res.vertices[i2 * 3 + 2]);

        glm::vec3 fn(0.0f);
        if (i3 != 0xffffffff && i3 < (uint32_t)nbV) {
            glm::vec3 p3(res.vertices[i3 * 3], res.vertices[i3 * 3 + 1], res.vertices[i3 * 3 + 2]);
            fn = glm::cross(p2 - p0, p3 - p1);
            vertNormals[i3] += fn;
        } else {
            fn = glm::cross(p1 - p0, p2 - p0);
        }
        vertNormals[i0] += fn;
        vertNormals[i1] += fn;
        vertNormals[i2] += fn;
    }
#pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)nbV; ++i) {
        float l = glm::length(vertNormals[i]);
        if (l > 1e-6f) vertNormals[i] /= l;
        else vertNormals[i] = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    // 2. Crease Graph & Sharp Feature Preservation
    const int MAX_CREASE_DEG = 4;
    std::vector<uint8_t> isCreaseVert(nbV, 0);
    std::vector<uint32_t> creaseNeighbors(nbV * MAX_CREASE_DEG);
    std::vector<uint8_t> creaseNeighborCount(nbV, 0);
    std::vector<uint8_t> isNearCrease(nbV, 0);

    if (keepSharpEdges) {
        // Precompute face normals to evaluate dihedral angles across shared edges
        std::vector<glm::vec3> faceNormals(nbF, glm::vec3(0.0f));
        for (size_t f = 0; f < nbF; ++f) {
            uint32_t i0 = res.faces[f * 4 + 0];
            uint32_t i1 = res.faces[f * 4 + 1];
            uint32_t i2 = res.faces[f * 4 + 2];
            uint32_t i3 = res.faces[f * 4 + 3];
            if (i0 >= nbV || i1 >= nbV || i2 >= nbV) continue;

            glm::vec3 p0(res.vertices[i0 * 3], res.vertices[i0 * 3 + 1], res.vertices[i0 * 3 + 2]);
            glm::vec3 p1(res.vertices[i1 * 3], res.vertices[i1 * 3 + 1], res.vertices[i1 * 3 + 2]);
            glm::vec3 p2(res.vertices[i2 * 3], res.vertices[i2 * 3 + 1], res.vertices[i2 * 3 + 2]);

            glm::vec3 fn(0.0f);
            if (i3 != 0xffffffff && i3 < (uint32_t)nbV) {
                glm::vec3 p3(res.vertices[i3 * 3], res.vertices[i3 * 3 + 1], res.vertices[i3 * 3 + 2]);
                fn = glm::cross(p2 - p0, p3 - p1);
            } else {
                fn = glm::cross(p1 - p0, p2 - p0);
            }
            float l = glm::length(fn);
            if (l > 1e-6f) faceNormals[f] = fn / l;
            else faceNormals[f] = glm::vec3(0.0f, 0.0f, 1.0f);
        }

        std::unordered_map<uint64_t, uint32_t> edgeFaceMap;
        edgeFaceMap.reserve(nbF * 4);

        for (size_t f = 0; f < nbF; ++f) {
            uint32_t fv[4] = {
                res.faces[f * 4 + 0],
                res.faces[f * 4 + 1],
                res.faces[f * 4 + 2],
                res.faces[f * 4 + 3]
            };
            bool isQuad = (fv[3] != 0xffffffff && fv[3] < (uint32_t)nbV);
            int numEdges = isQuad ? 4 : 3;

            for (int k = 0; k < numEdges; ++k) {
                uint32_t u = fv[k];
                uint32_t w = fv[(k + 1) % numEdges];
                if (u >= (uint32_t)nbV || w >= (uint32_t)nbV || u == w) continue;

                uint64_t key = (uint64_t(std::min(u, w)) << 32) | uint64_t(std::max(u, w));
                auto it = edgeFaceMap.find(key);
                if (it == edgeFaceMap.end()) {
                    edgeFaceMap[key] = (uint32_t)f;
                } else {
                    uint32_t fOther = it->second;
                    float dot = glm::dot(faceNormals[f], faceNormals[fOther]);
                    bool sharpByTag = (!res.sharpVertices.empty() && u < res.sharpVertices.size() && w < res.sharpVertices.size() && res.sharpVertices[u] && res.sharpVertices[w]);
                    if (dot <= 0.85f || sharpByTag) {
                        isCreaseVert[u] = 1;
                        isCreaseVert[w] = 1;

                        uint8_t cu = creaseNeighborCount[u];
                        bool foundW = false;
                        for (uint8_t j = 0; j < cu; ++j) {
                            if (creaseNeighbors[u * MAX_CREASE_DEG + j] == w) {
                                foundW = true;
                                break;
                            }
                        }
                        if (!foundW && cu < MAX_CREASE_DEG) {
                            creaseNeighbors[u * MAX_CREASE_DEG + cu] = w;
                            creaseNeighborCount[u] = cu + 1;
                        }

                        uint8_t cw = creaseNeighborCount[w];
                        bool foundU = false;
                        for (uint8_t j = 0; j < cw; ++j) {
                            if (creaseNeighbors[w * MAX_CREASE_DEG + j] == u) {
                                foundU = true;
                                break;
                            }
                        }
                        if (!foundU && cw < MAX_CREASE_DEG) {
                            creaseNeighbors[w * MAX_CREASE_DEG + cw] = u;
                            creaseNeighborCount[w] = cw + 1;
                        }
                    }
                }
            }
        }

        if (!res.sharpVertices.empty()) {
            for (size_t i = 0; i < nbV && i < res.sharpVertices.size(); ++i) {
                if (res.sharpVertices[i]) isCreaseVert[i] = 1;
            }
        }
    }

    // 3. Build vertex adjacency graph (VRV) and quad diagonal pairs
    const int MAX_DEG = 8;
    std::vector<uint32_t> vertNeighbors(nbV * MAX_DEG);
    std::vector<uint8_t> vertNeighborCount(nbV, 0);

    struct QuadDiagRef {
        uint32_t opp;
        uint32_t otherA;
        uint32_t otherB;
    };
    std::vector<QuadDiagRef> vertDiags(nbV * MAX_DEG);
    std::vector<uint8_t> vertDiagCount(nbV, 0);

    for (size_t f = 0; f < nbF; ++f) {
        uint32_t fv[4] = {
            res.faces[f * 4 + 0],
            res.faces[f * 4 + 1],
            res.faces[f * 4 + 2],
            res.faces[f * 4 + 3]
        };
        bool isQuad = (fv[3] != 0xffffffff && fv[3] < (uint32_t)nbV);
        int numEdges = isQuad ? 4 : 3;

        for (int k = 0; k < numEdges; ++k) {
            uint32_t u = fv[k];
            uint32_t w = fv[(k + 1) % numEdges];
            if (u >= (uint32_t)nbV || w >= (uint32_t)nbV || u == w) continue;

            uint8_t cu = vertNeighborCount[u];
            bool foundU = false;
            for (uint8_t j = 0; j < cu; ++j) {
                if (vertNeighbors[u * MAX_DEG + j] == w) {
                    foundU = true;
                    break;
                }
            }
            if (!foundU && cu < MAX_DEG) {
                vertNeighbors[u * MAX_DEG + cu] = w;
                vertNeighborCount[u] = cu + 1;
            }

            uint8_t cw = vertNeighborCount[w];
            bool foundW = false;
            for (uint8_t j = 0; j < cw; ++j) {
                if (vertNeighbors[w * MAX_DEG + j] == u) {
                    foundW = true;
                    break;
                }
            }
            if (!foundW && cw < MAX_DEG) {
                vertNeighbors[w * MAX_DEG + cw] = u;
                vertNeighborCount[w] = cw + 1;
            }
        }

        if (isQuad && fv[0] < nbV && fv[1] < nbV && fv[2] < nbV && fv[3] < nbV) {
            auto addDiag = [&](uint32_t myV, uint32_t oppV, uint32_t oA, uint32_t oB) {
                uint8_t cd = vertDiagCount[myV];
                if (cd < MAX_DEG) {
                    vertDiags[myV * MAX_DEG + cd] = { oppV, oA, oB };
                    vertDiagCount[myV] = cd + 1;
                }
            };
            addDiag(fv[0], fv[2], fv[1], fv[3]);
            addDiag(fv[2], fv[0], fv[1], fv[3]);
            addDiag(fv[1], fv[3], fv[0], fv[2]);
            addDiag(fv[3], fv[1], fv[0], fv[2]);
        }
    }

    if (keepSharpEdges) {
        for (size_t v = 0; v < nbV; ++v) {
            if (isCreaseVert[v]) {
                isNearCrease[v] = 1;
                uint8_t count = vertNeighborCount[v];
                const uint32_t* nbrPtr = &vertNeighbors[v * MAX_DEG];
                for (uint8_t k = 0; k < count; ++k) {
                    uint32_t u = nbrPtr[k];
                    isNearCrease[u] = 1;
                    uint8_t countU = vertNeighborCount[u];
                    const uint32_t* nbrU = &vertNeighbors[u * MAX_DEG];
                    for (uint8_t j = 0; j < countU; ++j) {
                        isNearCrease[nbrU[j]] = 1;
                    }
                }
            }
        }
    }

    // 4. Identify symmetry-constrained vertices
    std::vector<uint8_t> symFixed(nbV, 0);
    if (alignSymmetry) {
#pragma omp parallel for schedule(static)
        for (int i = 0; i < (int)nbV; ++i) {
            float vx = res.vertices[i * 3 + 0];
            float vy = res.vertices[i * 3 + 1];
            float vz = res.vertices[i * 3 + 2];
            uint8_t mask = 0;
            if (std::abs(vx) < 1e-4f) mask |= 1;
            if (std::abs(vy) < 1e-4f) mask |= 2;
            if (std::abs(vz) < 1e-4f) mask |= 4;
            symFixed[i] = mask;
        }
    }

    // 5. Jacobi iterations with double buffering
    std::vector<float> currVerts = res.vertices;
    std::vector<float> nextVerts = res.vertices;

    for (int iter = 0; iter < iterations; ++iter) {
#pragma omp parallel for schedule(static)
        for (int v = 0; v < (int)nbV; ++v) {
            if (keepSharpEdges && isCreaseVert[v]) {
                uint8_t cCount = creaseNeighborCount[v];
                if (cCount == 2) {
                    // Regularize strictly along 1D crease tangent
                    uint32_t c0 = creaseNeighbors[v * MAX_CREASE_DEG + 0];
                    uint32_t c1 = creaseNeighbors[v * MAX_CREASE_DEG + 1];

                    glm::vec3 pv(currVerts[v * 3 + 0], currVerts[v * 3 + 1], currVerts[v * 3 + 2]);
                    glm::vec3 pc0(currVerts[c0 * 3 + 0], currVerts[c0 * 3 + 1], currVerts[c0 * 3 + 2]);
                    glm::vec3 pc1(currVerts[c1 * 3 + 0], currVerts[c1 * 3 + 1], currVerts[c1 * 3 + 2]);

                    glm::vec3 t = pc1 - pc0;
                    float tLen = glm::length(t);
                    if (tLen > 1e-6f) {
                        t /= tLen;
                        glm::vec3 pMid = 0.5f * (pc0 + pc1);
                        glm::vec3 delta = pMid - pv;
                        glm::vec3 deltaAlongCrease = glm::dot(delta, t) * t;

                        if (alignSymmetry) {
                            uint8_t sf = symFixed[v];
                            if (sf & 1) deltaAlongCrease.x = 0.0f;
                            if (sf & 2) deltaAlongCrease.y = 0.0f;
                            if (sf & 4) deltaAlongCrease.z = 0.0f;
                        }

                        glm::vec3 pNew = pv + lambda * deltaAlongCrease;

                        if (alignSymmetry) {
                            uint8_t sf = symFixed[v];
                            if (sf & 1) pNew.x = 0.0f;
                            if (sf & 2) pNew.y = 0.0f;
                            if (sf & 4) pNew.z = 0.0f;
                        }

                        nextVerts[v * 3 + 0] = pNew.x;
                        nextVerts[v * 3 + 1] = pNew.y;
                        nextVerts[v * 3 + 2] = pNew.z;
                        continue;
                    }
                }

                // Corners (creaseCount >= 3 or 1 or isolated): Freeze completely
                nextVerts[v * 3 + 0] = currVerts[v * 3 + 0];
                nextVerts[v * 3 + 1] = currVerts[v * 3 + 1];
                nextVerts[v * 3 + 2] = currVerts[v * 3 + 2];
                continue;
            }

            uint8_t count = vertNeighborCount[v];
            if (count == 0) continue;

            glm::vec3 pv(currVerts[v * 3 + 0], currVerts[v * 3 + 1], currVerts[v * 3 + 2]);

            // Edge Laplacian vector
            glm::vec3 pAvg(0.0f);
            const uint32_t* nbrPtr = &vertNeighbors[v * MAX_DEG];
            for (uint8_t k = 0; k < count; ++k) {
                uint32_t u = nbrPtr[k];
                pAvg += glm::vec3(currVerts[u * 3 + 0], currVerts[u * 3 + 1], currVerts[u * 3 + 2]);
            }
            pAvg /= (float)count;
            glm::vec3 delta = pAvg - pv;

            // Quad diagonal equalization force
            uint8_t diagCount = vertDiagCount[v];
            if (diagCount > 0) {
                glm::vec3 diagDelta(0.0f);
                const QuadDiagRef* dPtr = &vertDiags[v * MAX_DEG];
                for (uint8_t k = 0; k < diagCount; ++k) {
                    uint32_t opp = dPtr[k].opp;
                    uint32_t oA = dPtr[k].otherA;
                    uint32_t oB = dPtr[k].otherB;

                    glm::vec3 pOpp(currVerts[opp * 3 + 0], currVerts[opp * 3 + 1], currVerts[opp * 3 + 2]);
                    glm::vec3 pA(currVerts[oA * 3 + 0], currVerts[oA * 3 + 1], currVerts[oA * 3 + 2]);
                    glm::vec3 pB(currVerts[oB * 3 + 0], currVerts[oB * 3 + 1], currVerts[oB * 3 + 2]);

                    glm::vec3 myDiag = pv - pOpp;
                    glm::vec3 otherDiag = pB - pA;
                    float dMy = glm::length(myDiag);
                    float dOther = glm::length(otherDiag);

                    if (dMy > 1e-6f && dOther > 1e-6f) {
                        float targetLen = (dMy + dOther) * 0.5f;
                        float diff = (targetLen - dMy) * 0.5f;
                        diagDelta += diff * (myDiag / dMy);
                    }
                }
                diagDelta /= (float)diagCount;
                delta = delta * 0.6f + diagDelta * 0.4f;
            }

            // Normal projection:
            // For vertices adjacent to sharp creases, use vertNormals to prevent tilted SDF gradient from beveling the face!
            glm::vec3 norm = vertNormals[v];
            if (!keepSharpEdges || !isNearCrease[v]) {
                float gx = (pv.x - voxels.minCoord[0]) / voxels.step;
                float gy = (pv.y - voxels.minCoord[1]) / voxels.step;
                float gz = (pv.z - voxels.minCoord[2]) / voxels.step;
                glm::vec3 sdfNorm = sampleSDFGradient(voxels, gx, gy, gz);
                if (glm::length(sdfNorm) > 1e-6f) {
                    if (glm::dot(sdfNorm, vertNormals[v]) < 0.0f) {
                        sdfNorm = -sdfNorm;
                    }
                    norm = sdfNorm;
                }
            }

            // Project delta onto tangent plane
            glm::vec3 deltaTan = delta - glm::dot(delta, norm) * norm;

            if (alignSymmetry) {
                uint8_t sf = symFixed[v];
                if (sf & 1) deltaTan.x = 0.0f;
                if (sf & 2) deltaTan.y = 0.0f;
                if (sf & 4) deltaTan.z = 0.0f;
            }

            glm::vec3 pNew = pv + lambda * deltaTan;

            if (alignSymmetry) {
                uint8_t sf = symFixed[v];
                if (sf & 1) pNew.x = 0.0f;
                if (sf & 2) pNew.y = 0.0f;
                if (sf & 4) pNew.z = 0.0f;
            }

            nextVerts[v * 3 + 0] = pNew.x;
            nextVerts[v * 3 + 1] = pNew.y;
            nextVerts[v * 3 + 2] = pNew.z;
        }
        currVerts = nextVerts;
    }

    if (keepSharpEdges) {
        res.sharpVertices = std::move(isCreaseVert);
    }

    res.vertices = std::move(currVerts);
}

// ---------------------------------------------------------
// Multi-Chart 6-Direction PolyCube Reconstruction
// ---------------------------------------------------------

static bool tryPolyCubeReconstruct(
    const VoxelGrid& voxels,
    const float* box,
    bool alignSymmetry,
    RemeshResult& res,
    std::function<void(int stage, int progress)> onProgress = nullptr
) {
    if (!box) return false;

    // 1. Center and extents of bounding box
    glm::vec3 C(
        0.5f * (box[0] + box[3]),
        0.5f * (box[1] + box[4]),
        0.5f * (box[2] + box[5])
    );

    float gx = (C.x - voxels.minCoord[0]) / voxels.step;
    float gy = (C.y - voxels.minCoord[1]) / voxels.step;
    float gz = (C.z - voxels.minCoord[2]) / voxels.step;

    if (gx < 1.0f || gx >= (float)(voxels.dims[0] - 2) ||
        gy < 1.0f || gy >= (float)(voxels.dims[1] - 2) ||
        gz < 1.0f || gz >= (float)(voxels.dims[2] - 2)) {
        return false;
    }

    // Center MUST be deeply inside the object (not in a hole or hollow area)
    float dCenter = sampleSDFScalar(voxels, gx, gy, gz);
    if (dCenter >= -0.5f * voxels.step) {
        return false;
    }

    float rx = 0.5f * (box[3] - box[0]);
    float ry = 0.5f * (box[4] - box[1]);
    float rz = 0.5f * (box[5] - box[2]);
    float maxR = std::max(rx, std::max(ry, rz));
    if (maxR < voxels.step * 2.0f) return false;

    // Sphericity check: compare radius along axis vs diagonal
    // A sphere has R_diag / R_axis ~ 1.0, while a sharp cube has R_diag / R_axis ~ 1.732.
    // If the object has sharp box corners, Surface Nets already reconstructs it with 1.05x aspect ratio!
    auto traceTestRay = [&](const glm::vec3& dir) -> float {
        float t = voxels.step * 0.5f;
        float tMax = maxR * 2.5f;
        float dPrev = sampleSDFScalar(voxels, 
            (C.x + t * dir.x - voxels.minCoord[0]) / voxels.step,
            (C.y + t * dir.y - voxels.minCoord[1]) / voxels.step,
            (C.z + t * dir.z - voxels.minCoord[2]) / voxels.step
        );
        if (dPrev >= 0.0f) return -1.0f;
        while (t < tMax) {
            float advance = std::clamp(std::abs(dPrev) * 0.5f, voxels.step * 0.35f, voxels.step * 2.0f);
            t += advance;
            float gx = (C.x + t * dir.x - voxels.minCoord[0]) / voxels.step;
            float gy = (C.y + t * dir.y - voxels.minCoord[1]) / voxels.step;
            float gz = (C.z + t * dir.z - voxels.minCoord[2]) / voxels.step;
            if (gx < 0.0f || gx >= (float)(voxels.dims[0] - 1) ||
                gy < 0.0f || gy >= (float)(voxels.dims[1] - 1) ||
                gz < 0.0f || gz >= (float)(voxels.dims[2] - 1)) break;
            float dCur = sampleSDFScalar(voxels, gx, gy, gz);
            if (dCur >= 0.0f) return t;
            dPrev = dCur;
        }
        return -1.0f;
    };

    float rAxis = traceTestRay(glm::vec3(1.0f, 0.0f, 0.0f));
    float rDiag = traceTestRay(glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));
    if (rAxis > 0.0f && rDiag > 0.0f && (rDiag / rAxis > 1.25f)) {
        return false;
    }

    // 2. Target grid subdivision M per cube edge (M must be even for exact symmetry planes)
    int M = 2 * (int)std::round(0.88f * maxR / voxels.step);
    M = std::clamp(M, 8, 384);

    // 3. Build unique boundary vertex table and 6-chart quad faces
    int strideY = M + 1;
    int strideZ = (M + 1) * (M + 1);
    size_t keyCount = (size_t)(M + 1) * strideZ;
    std::vector<int32_t> cubeKeyToVertId(keyCount, -1);

    std::vector<glm::ivec3> vertCoords;
    vertCoords.reserve(6 * M * M + 2);

    auto getOrCreateVert = [&](int ix, int iy, int iz) -> int32_t {
        size_t key = (size_t)ix + (size_t)iy * strideY + (size_t)iz * strideZ;
        int32_t id = cubeKeyToVertId[key];
        if (id == -1) {
            id = (int32_t)vertCoords.size();
            vertCoords.push_back(glm::ivec3(ix, iy, iz));
            cubeKeyToVertId[key] = id;
        }
        return id;
    };

    std::vector<uint32_t> faces;
    faces.reserve(6 * M * M * 4);

    auto getFaceVert = [&](int f, int u, int v) -> int32_t {
        switch (f) {
            case 0: return getOrCreateVert(u, v, M);              // +Z
            case 1: return getOrCreateVert(M - u, v, 0);          // -Z
            case 2: return getOrCreateVert(M, u, v);              // +X
            case 3: return getOrCreateVert(0, M - u, v);          // -X
            case 4: return getOrCreateVert(v, M, u);              // +Y
            case 5: return getOrCreateVert(u, 0, v);              // -Y
            default: return 0;
        }
    };

    for (int f = 0; f < 6; ++f) {
        for (int v = 0; v < M; ++v) {
            for (int u = 0; u < M; ++u) {
                int32_t i0 = getFaceVert(f, u, v);
                int32_t i1 = getFaceVert(f, u + 1, v);
                int32_t i2 = getFaceVert(f, u + 1, v + 1);
                int32_t i3 = getFaceVert(f, u, v + 1);

                faces.push_back((uint32_t)i0);
                faces.push_back((uint32_t)i1);
                faces.push_back((uint32_t)i2);
                faces.push_back((uint32_t)i3);
            }
        }
    }

    size_t totalVerts = vertCoords.size();
    std::vector<float> vertices(totalVerts * 3);
    std::atomic<int> failCount(0);

    // 4. Raymarch each vertex along tangent-spherized direction from C to SDF = 0
    #pragma omp parallel for schedule(dynamic, 64)
    for (int i = 0; i < (int)totalVerts; ++i) {
        glm::ivec3 c = vertCoords[i];
        float xc = 2.0f * (float)c.x / (float)M - 1.0f;
        float yc = 2.0f * (float)c.y / (float)M - 1.0f;
        float zc = 2.0f * (float)c.z / (float)M - 1.0f;

        // Equal-area spherization mapping
        float xs = xc * std::sqrt(std::max(0.0f, 1.0f - yc * yc * 0.5f - zc * zc * 0.5f + yc * yc * zc * zc / 3.0f));
        float ys = yc * std::sqrt(std::max(0.0f, 1.0f - zc * zc * 0.5f - xc * xc * 0.5f + zc * zc * xc * xc / 3.0f));
        float zs = zc * std::sqrt(std::max(0.0f, 1.0f - xc * xc * 0.5f - yc * yc * 0.5f + xc * xc * yc * yc / 3.0f));

        glm::vec3 dir(xs, ys, zs);
        float dlen = glm::length(dir);
        if (dlen > 1e-6f) dir /= dlen;
        else dir = glm::vec3(0.0f, 0.0f, 1.0f);

        // March from C + tMin*dir to C + tMax*dir
        float tMin = voxels.step * 0.5f;
        float tMax = maxR * 2.5f;
        float tCur = tMin;
        float tFound = -1.0f;

        float g0x = (C.x + tMin * dir.x - voxels.minCoord[0]) / voxels.step;
        float g0y = (C.y + tMin * dir.y - voxels.minCoord[1]) / voxels.step;
        float g0z = (C.z + tMin * dir.z - voxels.minCoord[2]) / voxels.step;
        float dPrev = sampleSDFScalar(voxels, g0x, g0y, g0z);
        float tPrev = tMin;

        if (dPrev < 0.0f) {
            while (tCur < tMax) {
                float advance = std::clamp(std::abs(dPrev) * 0.5f, voxels.step * 0.25f, voxels.step * 1.5f);
                tCur += advance;

                float gx = (C.x + tCur * dir.x - voxels.minCoord[0]) / voxels.step;
                float gy = (C.y + tCur * dir.y - voxels.minCoord[1]) / voxels.step;
                float gz = (C.z + tCur * dir.z - voxels.minCoord[2]) / voxels.step;

                if (gx < 0.0f || gx >= (float)(voxels.dims[0] - 1) ||
                    gy < 0.0f || gy >= (float)(voxels.dims[1] - 1) ||
                    gz < 0.0f || gz >= (float)(voxels.dims[2] - 1)) {
                    break;
                }

                float dCur = sampleSDFScalar(voxels, gx, gy, gz);
                if (dCur >= 0.0f) {
                    float tA = tPrev;
                    float tB = tCur;
                    for (int it = 0; it < 14; ++it) {
                        float tMid = 0.5f * (tA + tB);
                        float mx = (C.x + tMid * dir.x - voxels.minCoord[0]) / voxels.step;
                        float my = (C.y + tMid * dir.y - voxels.minCoord[1]) / voxels.step;
                        float mz = (C.z + tMid * dir.z - voxels.minCoord[2]) / voxels.step;
                        float dMid = sampleSDFScalar(voxels, mx, my, mz);
                        if (dMid < 0.0f) {
                            tA = tMid;
                        } else {
                            tB = tMid;
                        }
                    }
                    tFound = 0.5f * (tA + tB);
                    break;
                }
                tPrev = tCur;
                dPrev = dCur;
            }
        }

        if (tFound <= 0.0f) {
            failCount.fetch_add(1, std::memory_order_relaxed);
            glm::vec3 fallback = C + maxR * dir;
            vertices[i * 3 + 0] = fallback.x;
            vertices[i * 3 + 1] = fallback.y;
            vertices[i * 3 + 2] = fallback.z;
        } else {
            glm::vec3 pos = C + tFound * dir;
            vertices[i * 3 + 0] = pos.x;
            vertices[i * 3 + 1] = pos.y;
            vertices[i * 3 + 2] = pos.z;
        }
    }

    if (failCount.load() > (int)(totalVerts * 0.005f)) {
        return false;
    }

    // 5. Radial Distance Smoothing (Eliminates discrete voxel facetting / dimple artifacts)
    const int MAX_DEG = 8;
    std::vector<uint32_t> nbrList(totalVerts * MAX_DEG);
    std::vector<uint8_t> nbrCounts(totalVerts, 0);

    size_t nbFaces = faces.size() / 4;
    for (size_t f = 0; f < nbFaces; ++f) {
        for (int k = 0; k < 4; ++k) {
            uint32_t u = faces[f * 4 + k];
            uint32_t w = faces[f * 4 + (k + 1) % 4];
            if (u >= totalVerts || w >= totalVerts || u == w) continue;

            uint8_t cu = nbrCounts[u];
            bool foundU = false;
            for (uint8_t j = 0; j < cu; ++j) {
                if (nbrList[u * MAX_DEG + j] == w) { foundU = true; break; }
            }
            if (!foundU && cu < MAX_DEG) {
                nbrList[u * MAX_DEG + cu] = w;
                nbrCounts[u] = cu + 1;
            }

            uint8_t cw = nbrCounts[w];
            bool foundW = false;
            for (uint8_t j = 0; j < cw; ++j) {
                if (nbrList[w * MAX_DEG + j] == u) { foundW = true; break; }
            }
            if (!foundW && cw < MAX_DEG) {
                nbrList[w * MAX_DEG + cw] = u;
                nbrCounts[w] = cw + 1;
            }
        }
    }

    std::vector<float> currRadii(totalVerts);
    double initialSumR = 0.0;
    for (size_t i = 0; i < totalVerts; ++i) {
        glm::vec3 pos(vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]);
        float r = glm::length(pos - C);
        currRadii[i] = r;
        initialSumR += r;
    }
    float initialMeanR = (totalVerts > 0) ? (float)(initialSumR / (double)totalVerts) : 0.0f;

    std::vector<float> nextRadii = currRadii;
    const int numSmoothIters = 6;
    for (int it = 0; it < numSmoothIters; ++it) {
#pragma omp parallel for schedule(static)
        for (int i = 0; i < (int)totalVerts; ++i) {
            uint8_t deg = nbrCounts[i];
            if (deg == 0) continue;
            float rAvg = 0.0f;
            const uint32_t* nbrPtr = &nbrList[i * MAX_DEG];
            for (uint8_t j = 0; j < deg; ++j) {
                rAvg += currRadii[nbrPtr[j]];
            }
            rAvg /= (float)deg;
            nextRadii[i] = 0.30f * currRadii[i] + 0.70f * rAvg;
        }
        currRadii = nextRadii;
    }

    // Preserve exact mean radius / volume after smoothing
    double smoothedSumR = 0.0;
    for (size_t i = 0; i < totalVerts; ++i) {
        smoothedSumR += currRadii[i];
    }
    float smoothedMeanR = (totalVerts > 0) ? (float)(smoothedSumR / (double)totalVerts) : 0.0f;
    if (smoothedMeanR > 1e-6f && initialMeanR > 1e-6f) {
        float rScale = initialMeanR / smoothedMeanR;
        for (size_t i = 0; i < totalVerts; ++i) {
            currRadii[i] *= rScale;
        }
    }

    // Apply smoothed radii along rays
#pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)totalVerts; ++i) {
        glm::ivec3 c = vertCoords[i];
        float xc = 2.0f * (float)c.x / (float)M - 1.0f;
        float yc = 2.0f * (float)c.y / (float)M - 1.0f;
        float zc = 2.0f * (float)c.z / (float)M - 1.0f;

        float xs = xc * std::sqrt(std::max(0.0f, 1.0f - yc * yc * 0.5f - zc * zc * 0.5f + yc * yc * zc * zc / 3.0f));
        float ys = yc * std::sqrt(std::max(0.0f, 1.0f - zc * zc * 0.5f - xc * xc * 0.5f + zc * zc * xc * xc / 3.0f));
        float zs = zc * std::sqrt(std::max(0.0f, 1.0f - xc * xc * 0.5f - yc * yc * 0.5f + xc * xc * yc * yc / 3.0f));
        glm::vec3 dir(xs, ys, zs);
        float dlen = glm::length(dir);
        if (dlen > 1e-6f) dir /= dlen;
        else dir = glm::vec3(0.0f, 0.0f, 1.0f);

        glm::vec3 pos = C + currRadii[i] * dir;

        if (alignSymmetry) {
            if (c.x == M / 2 && std::abs(C.x) < voxels.step * 0.5f) pos.x = 0.0f;
            if (c.y == M / 2 && std::abs(C.y) < voxels.step * 0.5f) pos.y = 0.0f;
            if (c.z == M / 2 && std::abs(C.z) < voxels.step * 0.5f) pos.z = 0.0f;
        }

        vertices[i * 3 + 0] = pos.x;
        vertices[i * 3 + 1] = pos.y;
        vertices[i * 3 + 2] = pos.z;
    }

    res.vertices = std::move(vertices);
    res.faces = std::move(faces);

    res.colors.resize(totalVerts * 3);
    res.materials.resize(totalVerts * 3);
    for (size_t i = 0; i < totalVerts; ++i) {
        res.colors[i * 3 + 0] = voxels.uniformColor[0];
        res.colors[i * 3 + 1] = voxels.uniformColor[1];
        res.colors[i * 3 + 2] = voxels.uniformColor[2];
        res.materials[i * 3 + 0] = voxels.uniformMaterial[0];
        res.materials[i * 3 + 1] = voxels.uniformMaterial[1];
        res.materials[i * 3 + 2] = voxels.uniformMaterial[2];
    }

    return true;
}

// ---------------------------------------------------------
// Main Entry Point
// ---------------------------------------------------------

RemeshResult doRemesh(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    const float* colors,
    const float* materials,
    const uint32_t* faceGroups,
    const float* box,
    float resolution,
    bool block,
    bool smooth,
    bool manifold,
    const float* uniformColor,
    const float* uniformMaterial,
    bool hasColors,
    bool hasMaterials,
    bool hasFaceGroups,
    bool alignSymmetry,
    std::function<void(int stage, int progress)> onProgress,
    float matrixScaleX,
    float matrixScaleY,
    float matrixScaleZ,
    bool regularizeQuads,
    bool keepSharpEdges,
    bool keepMasked
) {
    auto tRemeshStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[C++ doRemesh] Start. nbVerts=%d, nbTris=%d, resolution=%.2f, block=%d, smooth=%d, manifold=%d, hasColors=%d, hasMaterials=%d, hasFaceGroups=%d, alignSymmetry=%d, regularizeQuads=%d, keepSharpEdges=%d, keepMasked=%d, scaleXYZ=(%.4f, %.4f, %.4f)\n",
               nbVerts, nbTris, resolution, block, smooth, manifold, hasColors, hasMaterials, hasFaceGroups, alignSymmetry, regularizeQuads, keepSharpEdges, keepMasked, matrixScaleX, matrixScaleY, matrixScaleZ);

    const std::vector<uint8_t> keep = (keepMasked && hasMaterials && materials)
        ? sculpt::getKeepMaskedTriangles(materials, nbVerts, tris, nbTris)
        : std::vector<uint8_t>();
    const bool keeping = std::find(keep.begin(), keep.end(), 1) != keep.end();

    if (keeping && std::find(keep.begin(), keep.end(), 0) == keep.end()) {
        sculpt_log("[C++ doRemesh] Everything is masked, returning original mesh.\n");
        RemeshResult allKept;
        allKept.vertices.assign(verts, verts + nbVerts * 3);
        if (hasColors && colors) allKept.colors.assign(colors, colors + nbVerts * 3);
        if (hasMaterials && materials) allKept.materials.assign(materials, materials + nbVerts * 3);
        allKept.faces.reserve(nbTris * 4);
        allKept.faceGroups.reserve(nbTris);
        for (int t = 0; t < nbTris; ++t) {
            allKept.faces.push_back(tris[t * 3 + 0]);
            allKept.faces.push_back(tris[t * 3 + 1]);
            allKept.faces.push_back(tris[t * 3 + 2]);
            allKept.faces.push_back(TRI_INDEX);
            allKept.faceGroups.push_back(hasFaceGroups && faceGroups ? faceGroups[t] : 0u);
        }
        allKept.profile.msTotalRemesh = 0.0;
        return allKept;
    }

    if (box) {
        sculpt_log("[C++ doRemesh] box: [%.4f, %.4f, %.4f, %.4f, %.4f, %.4f]\n", box[0], box[1], box[2], box[3], box[4], box[5]);
    } else {
        sculpt_log("[C++ doRemesh] WARNING: box is null!\n");
    }

    // 1. Compute voxel grid step based on scene reference scale (100.0 world units)
    float refWorldScale = 100.0f;
    float mScaleX = (matrixScaleX > 1e-5f) ? matrixScaleX : 1.0f;
    float mScaleY = (matrixScaleY > 1e-5f) ? matrixScaleY : 1.0f;
    float mScaleZ = (matrixScaleZ > 1e-5f) ? matrixScaleZ : 1.0f;
    float mScale = std::cbrt(mScaleX * mScaleY * mScaleZ);
    float worldStep = refWorldScale / (resolution > 0.0f ? resolution : 1.0f);
    float step = worldStep / mScale;

    float boxWidth = box ? (box[3] - box[0]) : 1.0f;
    float boxHeight = box ? (box[4] - box[1]) : 1.0f;
    float boxDepth = box ? (box[5] - box[2]) : 1.0f;
    float maxBoxDim = std::max({boxWidth, boxHeight, boxDepth});
    if (step <= 0.0f) step = 0.001f;
    if (maxBoxDim / step > 1200.0f) {
        step = maxBoxDim / 1200.0f;
    }

    float stepMin = step * 1.51f;
    float stepMax = step * 1.51f;

    VoxelGrid voxels;
    voxels.step = step;

    if (alignSymmetry) {
        // Offset by 0.5 * step so dual cell centers land exactly on 0.0 (origin axes)
        int minIdxX = (int)std::floor((box[0] - stepMin) / step - 0.5f);
        int minIdxY = (int)std::floor((box[1] - stepMin) / step - 0.5f);
        int minIdxZ = (int)std::floor((box[2] - stepMin) / step - 0.5f);

        voxels.minCoord[0] = (minIdxX + 0.5f) * step;
        voxels.minCoord[1] = (minIdxY + 0.5f) * step;
        voxels.minCoord[2] = (minIdxZ + 0.5f) * step;

        int maxIdxX = (int)std::ceil((box[3] + stepMax) / step - 0.5f);
        int maxIdxY = (int)std::ceil((box[4] + stepMax) / step - 0.5f);
        int maxIdxZ = (int)std::ceil((box[5] + stepMax) / step - 0.5f);

        voxels.dims[0] = maxIdxX - minIdxX + 1;
        voxels.dims[1] = maxIdxY - minIdxY + 1;
        voxels.dims[2] = maxIdxZ - minIdxZ + 1;
    } else {
        voxels.minCoord[0] = box[0] - stepMin;
        voxels.minCoord[1] = box[1] - stepMin;
        voxels.minCoord[2] = box[2] - stepMin;

        voxels.maxCoord[0] = box[3] + stepMax;
        voxels.maxCoord[1] = box[4] + stepMax;
        voxels.maxCoord[2] = box[5] + stepMax;

        voxels.dims[0] = (int)std::ceil((voxels.maxCoord[0] - voxels.minCoord[0]) / step);
        voxels.dims[1] = (int)std::ceil((voxels.maxCoord[1] - voxels.minCoord[1]) / step);
        voxels.dims[2] = (int)std::ceil((voxels.maxCoord[2] - voxels.minCoord[2]) / step);
    }

    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int datalen = rx * ry * rz;

    sculpt_log("[C++ doRemesh] dims: [%d, %d, %d], step: %.6f, datalen: %d\n", rx, ry, rz, step, datalen);

#ifdef _OPENMP
    sculpt_log("[REMESH PROFILE] OpenMP active. Max threads: %d\n", omp_get_max_threads());
#else
    sculpt_log("[REMESH PROFILE] OpenMP: Disabled (compiled single-threaded)\n");
#endif

    double distMemMB = (double)datalen * sizeof(float) / (1024.0 * 1024.0);
    double crossMemMB = (double)datalen * sizeof(uint8_t) / (1024.0 * 1024.0);
    double colorMemMB = 0.0;
    double matMemMB = 0.0;
    double coreGridTotalMB = distMemMB + crossMemMB;
    sculpt_log("[REMESH PROFILE] Grid Memory: Dims=[%d, %d, %d] (%d voxels) | dist=%.2f MB, cross=%.2f MB, color=%.2f MB, mat=%.2f MB | Core Grid Total=%.2f MB\n",
              rx, ry, rz, datalen, distMemMB, crossMemMB, colorMemMB, matMemMB, coreGridTotalMB);

    // 2. Allocate voxel fields
    voxels.crossedEdges.assign(datalen, 0);
    voxels.distanceField.assign(datalen, std::numeric_limits<float>::infinity());
    if (keepSharpEdges) {
        voxels.hermiteEdges.resize(rz);
    }

    // Phase 1 Optimization: Lazy Attribute Transfer.
    // 3D volume grids for colors and materials are eliminated (saving 8.1 GB on Res 1000).
    // Colors and materials are transferred directly onto the reconstructed surface mesh
    // vertices via spatial KD-tree/barycentric projection post-reconstruction.
    voxels.hasColorField = false;
    if (uniformColor) {
        voxels.uniformColor[0] = uniformColor[0];
        voxels.uniformColor[1] = uniformColor[1];
        voxels.uniformColor[2] = uniformColor[2];
    } else {
        voxels.uniformColor[0] = 0.72f;
        voxels.uniformColor[1] = 0.52f;
        voxels.uniformColor[2] = 0.45f;
    }

    voxels.hasMaterialField = false;
    if (uniformMaterial) {
        voxels.uniformMaterial[0] = uniformMaterial[0];
        voxels.uniformMaterial[1] = uniformMaterial[1];
        voxels.uniformMaterial[2] = uniformMaterial[2];
    } else {
        voxels.uniformMaterial[0] = 0.5f;
        voxels.uniformMaterial[1] = 0.0f;
        voxels.uniformMaterial[2] = 1.0f;
    }

    voxels.hasGroupField = false; // PolyGroups are transferred via spatial projection post-reconstruction

    // 3. Voxelize
    auto tVoxelizeStart = std::chrono::high_resolution_clock::now();
    voxelize(verts, nbVerts, tris, nbTris, colors, materials, faceGroups, voxels, onProgress, keepSharpEdges);
    double msVoxelize = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tVoxelizeStart).count();
    sculpt_log("[REMESH PROFILE] Stage 1 - Voxelize: %.2f ms\n", msVoxelize);

    // 4. Flood fill inside/outside
    auto tFloodStart = std::chrono::high_resolution_clock::now();
    floodFill(voxels, onProgress);
    double msFlood = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tFloodStart).count();
    sculpt_log("[REMESH PROFILE] Stage 2 - Flood Fill: %.2f ms\n", msFlood);

    // 5. Reconstruct Surface
    auto tReconstructStart = std::chrono::high_resolution_clock::now();
    RemeshResult r;
    bool usedPolyCube = false;
    if (manifold) {
        r = marchingCubesReconstruct(voxels, onProgress);
    } else {
        if (regularizeQuads && !block && !keepSharpEdges) {
            usedPolyCube = tryPolyCubeReconstruct(voxels, box, alignSymmetry, r, onProgress);
        }
        if (!usedPolyCube) {
            r = surfaceNetsReconstruct(voxels, block, regularizeQuads, keepSharpEdges, onProgress);
        }
    }
    double msReconstruct = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tReconstructStart).count();
    sculpt_log("[REMESH PROFILE] Stage 3 - Surface Reconstruction (%s): %.2f ms (Output: %d verts, %d faces)\n",
              manifold ? "Marching Cubes" : (usedPolyCube ? "PolyCube 6-Chart" : "Surface Nets"), msReconstruct, (int)(r.vertices.size() / 3), (int)(r.faces.size() / 4));

    if (!usedPolyCube) {
        float snapTol = step * 0.45f;
        for (size_t i = 0; i < r.vertices.size(); i += 3) {
            float vx = voxels.minCoord[0] + r.vertices[i] * step;
            float vy = voxels.minCoord[1] + r.vertices[i + 1] * step;
            float vz = voxels.minCoord[2] + r.vertices[i + 2] * step;

            if (alignSymmetry) {
                if (std::abs(vx) < snapTol) vx = 0.0f;
                if (std::abs(vy) < snapTol) vy = 0.0f;
                if (std::abs(vz) < snapTol) vz = 0.0f;
            }

            r.vertices[i]     = vx;
            r.vertices[i + 1] = vy;
            r.vertices[i + 2] = vz;
        }

        if (regularizeQuads && !manifold && !block && r.vertices.size() >= 12) {
            applyConstrainedTangentialRelaxation(r, voxels, 3, 0.45f, alignSymmetry, keepSharpEdges);
        }
    } else {
        if (r.vertices.size() >= 12) {
            applyConstrainedTangentialRelaxation(r, voxels, 1, 0.15f, alignSymmetry, false);
        }
    }

    // 6. Spatial Mesh-to-Mesh Attribute & PolyGroup Transfer
    double msPolyGroupTotal = 0.0;
    double msBuild = 0.0;
    double msQuery = 0.0;
    double msCleanup = 0.0;
    bool needsAttributeTransfer = ((hasMaterials && materials) || (hasColors && colors));
    bool needsPolyGroupTransfer = (hasFaceGroups && faceGroups && !r.faces.empty());
    if ((needsAttributeTransfer || needsPolyGroupTransfer) && nbTris > 0 && !r.vertices.empty()) {
        auto tPolyGroupStart = std::chrono::high_resolution_clock::now();
        size_t numFaces = r.faces.size() / 4;
        size_t numVerts = r.vertices.size() / 3;

        auto tBuildStart = std::chrono::high_resolution_clock::now();
        TriangleSpatialIndex spatialIndex;
        spatialIndex.build(verts, nbVerts, tris, nbTris, faceGroups, step);
        msBuild = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tBuildStart).count();

        auto tQueryStart = std::chrono::high_resolution_clock::now();

        // A. Material & Color Attribute Transfer onto Reconstructed Vertices
        if (needsAttributeTransfer && numVerts > 0) {
            std::vector<float> vertNormals(numVerts * 3, 0.0f);
            for (size_t f = 0; f < numFaces; ++f) {
                uint32_t idx0 = r.faces[f * 4 + 0];
                uint32_t idx1 = r.faces[f * 4 + 1];
                uint32_t idx2 = r.faces[f * 4 + 2];
                uint32_t idx3 = r.faces[f * 4 + 3];
                if (idx0 >= numVerts || idx1 >= numVerts || idx2 >= numVerts) continue;
                const float* p0 = &r.vertices[idx0 * 3];
                const float* p1 = &r.vertices[idx1 * 3];
                const float* p2 = &r.vertices[idx2 * 3];
                float fn[3] = {
                    (p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]),
                    (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]),
                    (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])
                };
                for (int k = 0; k < 3; ++k) {
                    vertNormals[idx0 * 3 + k] += fn[k];
                    vertNormals[idx1 * 3 + k] += fn[k];
                    vertNormals[idx2 * 3 + k] += fn[k];
                    if (idx3 != 0xffffffff && idx3 < numVerts) {
                        vertNormals[idx3 * 3 + k] += fn[k];
                    }
                }
            }
#pragma omp parallel for schedule(static)
            for (int i = 0; i < (int)numVerts; ++i) {
                float* vn = &vertNormals[i * 3];
                float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
                if (len > 1e-9f) { vn[0] /= len; vn[1] /= len; vn[2] /= len; }
                else { vn[0] = 0.0f; vn[1] = 0.0f; vn[2] = 1.0f; }
            }

            if (hasMaterials && materials) {
                r.materials.resize(numVerts * 3);
            }
            if (hasColors && colors) {
                r.colors.resize(numVerts * 3);
            }

#pragma omp parallel for schedule(static)
            for (int i = 0; i < (int)numVerts; ++i) {
                const float* pt = &r.vertices[i * 3];
                const float* vn = &vertNormals[i * 3];
                auto triRes = spatialIndex.findClosestTriangle(pt, vn);
                uint32_t t = triRes.triIndex;
                uint32_t iv0 = tris[t * 3 + 0];
                uint32_t iv1 = tris[t * 3 + 1];
                uint32_t iv2 = tris[t * 3 + 2];

                if (hasMaterials && materials) {
                    for (int k = 0; k < 3; ++k) {
                        float val = triRes.w0 * materials[iv0 * 3 + k] +
                                    triRes.w1 * materials[iv1 * 3 + k] +
                                    triRes.w2 * materials[iv2 * 3 + k];
                        r.materials[i * 3 + k] = std::clamp(val, 0.0f, 1.0f);
                    }
                }
                if (hasColors && colors) {
                    for (int k = 0; k < 3; ++k) {
                        float val = triRes.w0 * colors[iv0 * 3 + k] +
                                    triRes.w1 * colors[iv1 * 3 + k] +
                                    triRes.w2 * colors[iv2 * 3 + k];
                        r.colors[i * 3 + k] = std::clamp(val, 0.0f, 1.0f);
                    }
                }
            }
        }

        // B. PolyGroup Transfer onto Reconstructed Faces
        if (needsPolyGroupTransfer) {
            r.faceGroups.resize(numFaces, 0);

#pragma omp parallel for schedule(static)
            for (int i = 0; i < (int)numFaces; ++i) {
                uint32_t idx0 = r.faces[i * 4 + 0];
                uint32_t idx1 = r.faces[i * 4 + 1];
                uint32_t idx2 = r.faces[i * 4 + 2];
                uint32_t idx3 = r.faces[i * 4 + 3];

                if (idx0 >= numVerts || idx1 >= numVerts || idx2 >= numVerts) {
                    continue;
                }

                bool isQuad = (idx3 != 0xffffffff && idx3 < numVerts);
                const float* p0 = &r.vertices[idx0 * 3];
                const float* p1 = &r.vertices[idx1 * 3];
                const float* p2 = &r.vertices[idx2 * 3];

                float centroid[3];
                float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                float e2[3];

                if (isQuad) {
                    const float* p3 = &r.vertices[idx3 * 3];
                    centroid[0] = (p0[0] + p1[0] + p2[0] + p3[0]) * 0.25f;
                    centroid[1] = (p0[1] + p1[1] + p2[1] + p3[1]) * 0.25f;
                    centroid[2] = (p0[2] + p1[2] + p2[2] + p3[2]) * 0.25f;
                    e2[0] = p3[0] - p0[0];
                    e2[1] = p3[1] - p0[1];
                    e2[2] = p3[2] - p0[2];
                } else {
                    centroid[0] = (p0[0] + p1[0] + p2[0]) * (1.0f / 3.0f);
                    centroid[1] = (p0[1] + p1[1] + p2[1]) * (1.0f / 3.0f);
                    centroid[2] = (p0[2] + p1[2] + p2[2]) * (1.0f / 3.0f);
                    e2[0] = p2[0] - p0[0];
                    e2[1] = p2[1] - p0[1];
                    e2[2] = p2[2] - p0[2];
                }

                float nx = e1[1] * e2[2] - e1[2] * e2[1];
                float ny = e1[2] * e2[0] - e1[0] * e2[2];
                float nz = e1[0] * e2[1] - e1[1] * e2[0];
                float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                float norm[3] = { 0.0f, 0.0f, 1.0f };
                if (nlen > 1e-9f) {
                    norm[0] = nx / nlen;
                    norm[1] = ny / nlen;
                    norm[2] = nz / nlen;
                }

                r.faceGroups[i] = spatialIndex.findClosestPolyGroup(centroid, norm);
            }

            // Post-processing cleanup: Filter isolated 1-face noise
            auto tCleanupStart = std::chrono::high_resolution_clock::now();
            if (numFaces > 0) {
                std::vector<uint32_t> faceCounts(numVerts, 0);
                for (size_t f = 0; f < numFaces; ++f) {
                    for (int k = 0; k < 4; ++k) {
                        uint32_t v = r.faces[f * 4 + k];
                        if (v != 0xffffffff && v < numVerts) {
                            faceCounts[v]++;
                        }
                    }
                }

                std::vector<uint32_t> faceOffsets(numVerts + 1, 0);
                for (size_t v = 0; v < numVerts; ++v) {
                    faceOffsets[v + 1] = faceOffsets[v] + faceCounts[v];
                }

                std::vector<uint32_t> vertFaceIndices(faceOffsets[numVerts]);
                std::vector<uint32_t> currentOffsets = faceOffsets;
                for (size_t f = 0; f < numFaces; ++f) {
                    for (int k = 0; k < 4; ++k) {
                        uint32_t v = r.faces[f * 4 + k];
                        if (v != 0xffffffff && v < numVerts) {
                            vertFaceIndices[currentOffsets[v]++] = (uint32_t)f;
                        }
                    }
                }

                std::vector<uint32_t> cleanedGroups = r.faceGroups;
#pragma omp parallel for schedule(static)
                for (int f = 0; f < (int)numFaces; ++f) {
                    uint32_t myGroup = r.faceGroups[f];
                    struct GroupEntry { uint32_t group; int count; };
                    GroupEntry neighbors[32];
                    int neighborEntryCount = 0;
                    int totalNeighborFaces = 0;

                    for (int k = 0; k < 4; ++k) {
                        uint32_t v = r.faces[f * 4 + k];
                        if (v == 0xffffffff || v >= numVerts) continue;
                        uint32_t startIdx = faceOffsets[v];
                        uint32_t endIdx = faceOffsets[v + 1];
                        for (uint32_t idx = startIdx; idx < endIdx; ++idx) {
                            uint32_t nFace = vertFaceIndices[idx];
                            if (nFace != f) {
                                uint32_t nGroup = r.faceGroups[nFace];
                                totalNeighborFaces++;
                                bool found = false;
                                for (int e = 0; e < neighborEntryCount; ++e) {
                                    if (neighbors[e].group == nGroup) {
                                        neighbors[e].count++;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found && neighborEntryCount < 32) {
                                    neighbors[neighborEntryCount++] = { nGroup, 1 };
                                }
                            }
                        }
                    }

                    if (totalNeighborFaces > 0) {
                        int myGroupCount = 0;
                        for (int e = 0; e < neighborEntryCount; ++e) {
                            if (neighbors[e].group == myGroup) {
                                myGroupCount = neighbors[e].count;
                                break;
                            }
                        }

                        if (myGroupCount == 0 || (float)myGroupCount / totalNeighborFaces < 0.15f) {
                            uint32_t bestNeighborGroup = myGroup;
                            int maxCount = 0;
                            for (int e = 0; e < neighborEntryCount; ++e) {
                                if (neighbors[e].count > maxCount) {
                                    maxCount = neighbors[e].count;
                                    bestNeighborGroup = neighbors[e].group;
                                }
                            }
                            if (maxCount > 0) {
                                cleanedGroups[f] = bestNeighborGroup;
                            }
                        }
                    }
                }
                r.faceGroups = std::move(cleanedGroups);
            }
            msCleanup = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tCleanupStart).count();
        }

        msQuery = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tQueryStart).count();
        msPolyGroupTotal = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tPolyGroupStart).count();

        sculpt_log("[REMESH PROFILE] Stage 4 - Attribute & PolyGroup Transfer: Total=%.2f ms (Index Build: %.2f ms, Query Search: %.2f ms, Cleanup: %.2f ms)\n",
                  msPolyGroupTotal, msBuild, msQuery, msCleanup);
    }

    double msKeep = 0.0;
    if (keeping) {
        auto tKeepStart = std::chrono::high_resolution_clock::now();
        std::string keepErr, keepNote;
        RemeshResult stitched;
        if (sculpt::stitchKeptTriangles(
                verts, nbVerts,
                tris, nbTris,
                colors, materials, faceGroups,
                keep, r, stitched,
                keepErr, &keepNote)) {
            stitched.profile = r.profile;
            r = std::move(stitched);
            sculpt_log("[C++ doRemesh] Keep masked successfully stitched%s%s\n",
                       keepNote.empty() ? "" : ": ", keepNote.c_str());
        } else {
            sculpt_log("[C++ doRemesh] WARNING: Keep masked stitching failed: %s\n", keepErr.c_str());
        }
        msKeep = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tKeepStart).count();
    }

    if (onProgress) onProgress(2, 100);

    double msTotalRemesh = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRemeshStart).count();
    sculpt_log("[REMESH PROFILE] SUMMARY - C++ doRemesh Total: %.2f ms (Voxelize: %.2f ms | FloodFill: %.2f ms | Reconstruct: %.2f ms | PolyGroup: %.2f ms | KeepMasked: %.2f ms)\n",
              msTotalRemesh, msVoxelize, msFlood, msReconstruct, msPolyGroupTotal, msKeep);
    sculpt_log("[C++ doRemesh] Reconstructed. output verts: %d, faces: %d\n", (int)(r.vertices.size() / 3), (int)(r.faces.size() / 4));

    r.profile.msVoxelize = msVoxelize;
    r.profile.msFloodFill = msFlood;
    r.profile.msReconstruct = msReconstruct;
    r.profile.msPolyGroup = msPolyGroupTotal;
    r.profile.msPolyGroupBuild = msBuild;
    r.profile.msPolyGroupQuery = msQuery;
    r.profile.msPolyGroupCleanup = msCleanup;
    r.profile.msKeepMasked = msKeep;
    r.profile.msTotalRemesh = msTotalRemesh;
    r.profile.gridDims[0] = rx;
    r.profile.gridDims[1] = ry;
    r.profile.gridDims[2] = rz;
    r.profile.gridDatalen = datalen;
    size_t bytesCross = (size_t)datalen * sizeof(uint8_t); // peak during voxelize/floodFill (freed prior to reconstruct)
    size_t bytesDist = voxels.distanceField.capacity() * sizeof(float);
    size_t bytesColor = 0; // lazy attribute transfer
    size_t bytesMat = 0;   // lazy attribute transfer
    size_t bytesFloodAux = 0; // tagCell eliminated via crossedEdges bit 7
    size_t bytesReconAux = r.profile.bytesReconstructAux;

    double coreGridMB = (double)(bytesCross + bytesDist + bytesColor + bytesMat) / (1024.0 * 1024.0);
    double auxAllocMB = (double)(bytesFloodAux + bytesReconAux) / (1024.0 * 1024.0);
    ProcessMemorySnapshot snapAfter = getProcessMemorySnapshot();
    double peakWorkingSetMB = (double)snapAfter.peakWorkingSetBytes / (1024.0 * 1024.0);

    r.profile.bytesCrossedEdges = bytesCross;
    r.profile.bytesDistanceField = bytesDist;
    r.profile.bytesColorField = bytesColor;
    r.profile.bytesMaterialField = bytesMat;
    r.profile.bytesFloodFillAux = bytesFloodAux;
    r.profile.bytesReconstructAux = bytesReconAux;
    r.profile.coreGridRAM_MB = coreGridMB;
    r.profile.auxAllocRAM_MB = auxAllocMB;
    r.profile.peakWorkingSetMB = peakWorkingSetMB;
    r.profile.gridMemoryMB = coreGridMB;
    r.profile.step = step;

    return r;
}

RemeshResult doSurfaceNetsFromSDF(
    int resolution, 
    const float* minCoords, 
    const float* maxCoords, 
    const float* distanceField,
    const float* uniformColor,
    const float* uniformMaterial,
    std::function<void(int stage, int progress)> onProgress
) {
    VoxelGrid voxels;
    voxels.dims[0] = resolution;
    voxels.dims[1] = resolution;
    voxels.dims[2] = resolution;
    voxels.step = (maxCoords[0] - minCoords[0]) / (resolution - 1);
    
    voxels.minCoord[0] = minCoords[0];
    voxels.minCoord[1] = minCoords[1];
    voxels.minCoord[2] = minCoords[2];
    voxels.maxCoord[0] = maxCoords[0];
    voxels.maxCoord[1] = maxCoords[1];
    voxels.maxCoord[2] = maxCoords[2];
    
    int datalen = resolution * resolution * resolution;
    voxels.distanceField.assign(distanceField, distanceField + datalen);
    
    voxels.hasColorField = false;
    voxels.hasMaterialField = false;
    voxels.uniformColor[0] = uniformColor[0];
    voxels.uniformColor[1] = uniformColor[1];
    voxels.uniformColor[2] = uniformColor[2];
    voxels.uniformMaterial[0] = uniformMaterial[0];
    voxels.uniformMaterial[1] = uniformMaterial[1];
    voxels.uniformMaterial[2] = uniformMaterial[2];
    
    RemeshResult r = surfaceNetsReconstruct(voxels, false, true, false, onProgress);
    
    for (size_t i = 0; i < r.vertices.size(); i += 3) {
        r.vertices[i]     = voxels.minCoord[0] + r.vertices[i] * voxels.step;
        r.vertices[i + 1] = voxels.minCoord[1] + r.vertices[i + 1] * voxels.step;
        r.vertices[i + 2] = voxels.minCoord[2] + r.vertices[i + 2] * voxels.step;
    }

    if (r.vertices.size() >= 12) {
        applyConstrainedTangentialRelaxation(r, voxels, 3, 0.45f, false, false);
    }
    
    return r;
}
