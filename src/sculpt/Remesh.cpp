#include "sculpt/Remesh.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <cstdio>

// Bounding box mapping and structures
struct VoxelGrid {
    int dims[3];
    float step;
    float minCoord[3];
    float maxCoord[3];
    std::vector<uint8_t> crossedEdges;
    std::vector<float> distanceField;
    std::unordered_map<int, uint32_t> colorField;      // sparse map of packed colors
    std::unordered_map<int, uint32_t> materialField;   // sparse map of packed materials
    float uniformColor[3];
    float uniformMaterial[3];
    bool hasColorField = false;
    bool hasMaterialField = false;
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
    double closest[4]
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
    closest[3] = (double)zone;

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
// Voxelization & Flood Fill
// ---------------------------------------------------------

static void voxelize(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    const float* colors,
    const float* materials,
    VoxelGrid& voxels
) {
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;
    int datalen = rx * ry * rz;

    printf("[C++ voxelize] Starting voxelization: input nbVerts=%d, nbTris=%d, grid rx=%d, ry=%d, rz=%d, datalen=%d\n",
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

    for (int iTri = 0; iTri < nbTris; ++iTri) {
        int idTri = iTri * 3;
        uint32_t iv1 = tris[idTri] * 3;
        uint32_t iv2 = tris[idTri + 1] * 3;
        uint32_t iv3 = tris[idTri + 2] * 3;

        double v1[3] = { verts[iv1], verts[iv1 + 1], verts[iv1 + 2] };
        double v2[3] = { verts[iv2], verts[iv2 + 1], verts[iv2 + 2] };
        double v3[3] = { verts[iv3], verts[iv3 + 1], verts[iv3 + 2] };

        float c1[3] = {0.0f, 0.0f, 0.0f};
        if (colors) {
            c1[0] = (colors[iv1] + colors[iv2] + colors[iv3]) * inv3;
            c1[1] = (colors[iv1 + 1] + colors[iv2 + 1] + colors[iv3 + 1]) * inv3;
            c1[2] = (colors[iv1 + 2] + colors[iv2 + 2] + colors[iv3 + 2]) * inv3;
        }

        float m1[3] = {0.0f, 0.0f, 0.0f};
        if (materials) {
            m1[0] = (materials[iv1] + materials[iv2] + materials[iv3]) * inv3;
            m1[1] = (materials[iv1 + 1] + materials[iv2 + 1] + materials[iv3 + 1]) * inv3;
            m1[2] = (materials[iv1 + 2] + materials[iv2 + 2] + materials[iv3 + 2]) * inv3;
        }

        // Bounding box
        double xmin = std::min({v1[0], v2[0], v3[0]});
        double xmax = std::max({v1[0], v2[0], v3[0]});
        double ymin = std::min({v1[1], v2[1], v3[1]});
        double ymax = std::max({v1[1], v2[1], v3[1]});
        double zmin = std::min({v1[2], v2[2], v3[2]});
        double zmax = std::max({v1[2], v2[2], v3[2]});

        double triEdge1[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
        double triEdge2[3] = { v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2] };

        double a00 = triEdge1[0] * triEdge1[0] + triEdge1[1] * triEdge1[1] + triEdge1[2] * triEdge1[2];
        double a01 = triEdge1[0] * triEdge2[0] + triEdge1[1] * triEdge2[1] + triEdge1[2] * triEdge2[2];
        double a11 = triEdge2[0] * triEdge2[0] + triEdge2[1] * triEdge2[1] + triEdge2[2] * triEdge2[2];

        int snapMinx = (int)std::floor((xmin - vminx) * invStep);
        int snapMiny = (int)std::floor((ymin - vminy) * invStep);
        int snapMinz = (int)std::floor((zmin - vminz) * invStep);

        int snapMaxx = (int)std::ceil((xmax - vminx) * invStep);
        int snapMaxy = (int)std::ceil((ymax - vminy) * invStep);
        int snapMaxz = (int)std::ceil((zmax - vminz) * invStep);

        // Clamp bounds to avoid out of bounds grid access
        snapMinx = std::max(0, snapMinx);
        snapMiny = std::max(0, snapMiny);
        snapMinz = std::max(0, snapMinz);
        snapMaxx = std::min(rx - 1, snapMaxx);
        snapMaxy = std::min(ry - 1, snapMaxy);
        snapMaxz = std::min(rz - 1, snapMaxz);

        for (int k = snapMinz; k <= snapMaxz; ++k) {
            for (int j = snapMiny; j <= snapMaxy; ++j) {
                for (int i = snapMinx; i <= snapMaxx; ++i) {
                    double x = vminx + i * step;
                    double y = vminy + j * step;
                    double z = vminz + k * step;
                    int n = i + j * rx + k * rxy;

                    double point[3] = { x, y, z };
                    double closest[4];
                    double newDist = distance2PointTriangleEdges(point, triEdge1, triEdge2, v1, a00, a01, a11, closest);
                    newDist = std::sqrt(newDist);

                    if (newDist < voxels.distanceField[n]) {
                        voxels.distanceField[n] = (float)newDist;
                        int n3 = n * 3;
                        if (voxels.hasColorField) {
                            uint8_t r = (uint8_t)(c1[0] * 255.0f + 0.5f);
                            uint8_t g = (uint8_t)(c1[1] * 255.0f + 0.5f);
                            uint8_t b = (uint8_t)(c1[2] * 255.0f + 0.5f);
                            voxels.colorField[n] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        }
                        if (voxels.hasMaterialField) {
                            uint8_t r = (uint8_t)(m1[0] * 255.0f + 0.5f);
                            uint8_t g = (uint8_t)(m1[1] * 255.0f + 0.5f);
                            uint8_t b = (uint8_t)(m1[2] * 255.0f + 0.5f);
                            voxels.materialField[n] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        }
                    }

                    if (newDist > step)
                        continue;

                    for (int it = 0; it < 3; ++it) {
                        double val = closest[it] - point[it];
                        if (val < 0.0 || val > step)
                            continue;

                        int bit = 1 << it;
                        if ((voxels.crossedEdges[n] & bit) != 0)
                            continue;

                        double dist = intersectionRayTriangleEdges(point, dirUnit[it], triEdge1, triEdge2, v1);
                        if (dist < 0.0 || dist > step)
                            continue;

                        voxels.crossedEdges[n] |= bit;
                    }
                }
            }
        }
    }

    int nonInfCount = 0;
    int crossedCount = 0;
    for (int i = 0; i < datalen; ++i) {
        if (voxels.distanceField[i] != std::numeric_limits<float>::infinity()) {
            nonInfCount++;
        }
        if (voxels.crossedEdges[i] != 0) {
            crossedCount++;
        }
    }
    printf("[C++ voxelize] Finished voxelization: non-inf distances count=%d, crossed edges count=%d\n", nonInfCount, crossedCount);
}

static void floodFill(VoxelGrid& voxels) {
    float step = voxels.step;
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;

    int datalen = rx * ry * rz;
    std::vector<bool> tagCell(datalen, false);
    std::vector<int32_t> stack;
    stack.reserve(datalen / 64);

    printf("[C++ floodFill] Starting flood fill: datalen=%d\n", datalen);

    stack.push_back(0);
    tagCell[0] = true;

    int dirs[6] = { -1, 1, -rx, rx, -rxy, rxy };
    int dirsEdge[6] = { 0, 0, 1, 1, 2, 2 };

    while (!stack.empty()) {
        int cell = stack.back();
        stack.pop_back();
        float cellDist = voxels.distanceField[cell];
        if (cellDist < step) {
            // border hit
            for (int i = 0; i < 6; ++i) {
                int off = dirs[i];
                int idNext = cell + off;
                if (idNext >= datalen || idNext < 0) continue;
                if (tagCell[idNext]) continue;
                if (voxels.distanceField[idNext] == std::numeric_limits<float>::infinity()) continue;
                int idx = off >= 0 ? cell : idNext;
                int bit = 1 << dirsEdge[i];
                if ((voxels.crossedEdges[idx] & bit) == 0) {
                    tagCell[idNext] = true;
                    stack.push_back(idNext);
                }
            }
        } else {
            // exterior
            for (int i = 0; i < 6; ++i) {
                int idNext = cell + dirs[i];
                if (idNext >= datalen || idNext < 0) continue;
                if (tagCell[idNext]) continue;
                tagCell[idNext] = true;
                stack.push_back(idNext);
            }
        }
    }

    int taggedCount = 0;
    for (int id = 0; id < datalen; ++id) {
        if (!tagCell[id]) {
            voxels.distanceField[id] = -voxels.distanceField[id];
        } else {
            taggedCount++;
        }
    }
    printf("[C++ floodFill] Finished: tagged (exterior) cells=%d, untagged (interior) cells=%d\n", taggedCount, datalen - taggedCount);
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
                auto it = voxels.colorField.find(id);
                if (it != voxels.colorField.end()) {
                    uint32_t val = it->second;
                    c1 += (float)((val >> 16) & 0xff) * p;
                    c2 += (float)((val >> 8) & 0xff) * p;
                    c3 += (float)(val & 0xff) * p;
                }
            }
            if (voxels.hasMaterialField) {
                auto it = voxels.materialField.find(id);
                if (it != voxels.materialField.end()) {
                    uint32_t val = it->second;
                    m1 += (float)((val >> 16) & 0xff) * p;
                    m2 += (float)((val >> 8) & 0xff) * p;
                    m3 += (float)(val & 0xff) * p;
                }
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

static RemeshResult marchingCubesReconstruct(VoxelGrid& voxels) {
    RemeshResult res;
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int rxy = rx * ry;

    std::unordered_map<std::string, uint32_t> mapVertices;
    int n = 0;
    float grid[8];
    uint32_t edges[12];

    float tmpV[3];
    float tmpC[3] = {0.0f, 0.0f, 0.0f};
    float tmpM[3] = {0.0f, 0.0f, 0.0f};

    for (int z = 0; z < rz - 1; ++z, n += rx) {
        for (int y = 0; y < ry - 1; ++y, ++n) {
            for (int x = 0; x < rx - 1; ++x, ++n) {
                int cubeIndex = readScalarValuesMarchingCubes(voxels, grid, n, tmpC, tmpM);
                uint32_t edgeMask = mcEdgeTable[cubeIndex];
                if (edgeMask == 0) continue;

                for (int k = 0; k < 12; ++k) {
                    if (!(edgeMask & (1 << k))) continue;

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

                    char hashBuf[128];
                    std::sprintf(hashBuf, "%.7g+%.7g+%.7g", tmpV[0], tmpV[1], tmpV[2]);
                    std::string hash(hashBuf);

                    auto it = mapVertices.find(hash);
                    if (it != mapVertices.end()) {
                        edges[k] = it->second;
                    } else {
                        uint32_t idVertex = res.vertices.size() / 3;
                        mapVertices[hash] = idVertex;
                        edges[k] = idVertex;
                        res.vertices.push_back(tmpV[0]);
                        res.vertices.push_back(tmpV[1]);
                        res.vertices.push_back(tmpV[2]);
                        res.colors.push_back(tmpC[0]);
                        res.colors.push_back(tmpC[1]);
                        res.colors.push_back(tmpC[2]);
                        res.materials.push_back(tmpM[0]);
                        res.materials.push_back(tmpM[1]);
                        res.materials.push_back(tmpM[2]);
                    }
                }

                // Add faces
                const int* f = mcTriTable[cubeIndex];
                for (int l = 0; f[l] != -1 && l < 16; l += 3) {
                    res.faces.push_back(edges[f[l]]);
                    res.faces.push_back(edges[f[l + 1]]);
                    res.faces.push_back(edges[f[l + 2]]);
                    res.faces.push_back(0xffffffff); // TRI_INDEX
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
    std::vector<float>& outCols, std::vector<float>& outMats
) {
    float c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
    float m1 = 0.0f, m2 = 0.0f, m3 = 0.0f;
    float invSum = 0.0f;

    uint8_t mask = 0;
    int g = 0;
    int rx = voxels.dims[0];
    int rxy = rx * voxels.dims[1];

    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                int id = n + i + j * rx + k * rxy;
                float p = voxels.distanceField[id];
                grid[g] = p;
                if (p < 0.0f) {
                    mask |= (1 << g);
                }
                g++;
                if (p != std::numeric_limits<float>::infinity()) {
                    p = std::min(1.0f / std::abs(p), 1e15f);
                    invSum += p;
                    if (voxels.hasColorField) {
                        auto it = voxels.colorField.find(id);
                        if (it != voxels.colorField.end()) {
                            uint32_t val = it->second;
                            c1 += (float)((val >> 16) & 0xff) * p;
                            c2 += (float)((val >> 8) & 0xff) * p;
                            c3 += (float)(val & 0xff) * p;
                        }
                    }
                    if (voxels.hasMaterialField) {
                        auto it = voxels.materialField.find(id);
                        if (it != voxels.materialField.end()) {
                            uint32_t val = it->second;
                            m1 += (float)((val >> 16) & 0xff) * p;
                            m2 += (float)((val >> 8) & 0xff) * p;
                            m3 += (float)(val & 0xff) * p;
                        }
                    }
                }
            }
        }
    }

    if (mask != 0 && mask != 0xff) {
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
    }

    return mask;
}

static void interpolateVerticesSurfaceNets(
    uint32_t edgeMask, const float grid[8], const int x[3],
    std::vector<float>& vertices, bool block
) {
    float vTemp[3] = {0.0f, 0.0f, 0.0f};
    int edgeCount = 0;

    for (int i = 0; i < 12; ++i) {
        if (!(edgeMask & (1 << i)))
            continue;
        ++edgeCount;
        if (block)
            continue;

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

    float s = 1.0f / (float)edgeCount;
    for (int l = 0; l < 3; ++l) {
        vTemp[l] = (float)x[l] + s * vTemp[l];
    }
    vertices.push_back(vTemp[0]);
    vertices.push_back(vTemp[1]);
    vertices.push_back(vTemp[2]);
}

static void createFaceSurfaceNets(
    uint32_t edgeMask, uint8_t mask, const std::vector<int32_t>& buffer,
    const int R[3], int m, const int x[3], std::vector<uint32_t>& faces
) {
    for (int i = 0; i < 3; ++i) {
        if (!(edgeMask & (1 << i)))
            continue;

        int iu = (i + 1) % 3;
        int iv = (i + 2) % 3;

        if (x[iu] == 0 || x[iv] == 0)
            continue;

        int du = R[iu];
        int dv = R[iv];

        if (mask & 1) {
            faces.push_back(buffer[m]);
            faces.push_back(buffer[m - du]);
            faces.push_back(buffer[m - du - dv]);
            faces.push_back(buffer[m - dv]);
        } else {
            faces.push_back(buffer[m]);
            faces.push_back(buffer[m - dv]);
            faces.push_back(buffer[m - du - dv]);
            faces.push_back(buffer[m - du]);
        }
    }
}

static RemeshResult surfaceNetsReconstruct(VoxelGrid& voxels, bool block) {
    RemeshResult res;
    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];

    int n = 0;
    int x[3];
    int R[3] = { 1, rx + 1, (rx + 1) * (ry + 1) };
    float grid[8];
    int nbBuf = 1;
    std::vector<int32_t> buffer((rx + 1) * (ry + 1) * 2, 0);

    for (x[2] = 0; x[2] < rz - 1; ++x[2], n += rx, nbBuf ^= 1, R[2] = -R[2]) {
        int m = 1 + (rx + 1) * (1 + nbBuf * (ry + 1));

        for (x[1] = 0; x[1] < ry - 1; ++x[1], ++n, m += 2) {
            for (x[0] = 0; x[0] < rx - 1; ++x[0], ++n, ++m) {
                uint8_t mask = readScalarValuesSurfaceNets(voxels, grid, n, res.colors, res.materials);
                if (mask == 0 || mask == 0xff)
                    continue;

                uint32_t edgeMask = snEdgeTable[mask];
                buffer[m] = res.vertices.size() / 3;
                interpolateVerticesSurfaceNets(edgeMask, grid, x, res.vertices, block);
                createFaceSurfaceNets(edgeMask, mask, buffer, R, m, x, res.faces);
            }
        }
    }
    return res;
}

// ---------------------------------------------------------
// Main Entry Point
// ---------------------------------------------------------

RemeshResult doRemesh(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    const float* colors,
    const float* materials,
    const float* box,
    float resolution,
    bool block,
    bool smooth,
    bool manifold,
    const float* uniformColor,
    const float* uniformMaterial,
    bool hasColors,
    bool hasMaterials
) {
    printf("[C++ doRemesh] Start. nbVerts=%d, nbTris=%d, resolution=%.2f, block=%d, smooth=%d, manifold=%d, hasColors=%d, hasMaterials=%d\n",
           nbVerts, nbTris, resolution, block, smooth, manifold, hasColors, hasMaterials);
    if (box) {
        printf("[C++ doRemesh] box: [%.4f, %.4f, %.4f, %.4f, %.4f, %.4f]\n", box[0], box[1], box[2], box[3], box[4], box[5]);
    } else {
        printf("[C++ doRemesh] WARNING: box is null!\n");
    }

    // 1. Compute voxel grid step and dimensions
    float boxWidth = box[3] - box[0];
    float boxHeight = box[4] - box[1];
    float boxDepth = box[5] - box[2];
    float step = std::max({boxWidth, boxHeight, boxDepth}) / resolution;

    float stepMin = step * 1.51f;
    float stepMax = step * 1.51f;

    VoxelGrid voxels;
    voxels.step = step;
    voxels.minCoord[0] = box[0] - stepMin;
    voxels.minCoord[1] = box[1] - stepMin;
    voxels.minCoord[2] = box[2] - stepMin;

    voxels.maxCoord[0] = box[3] + stepMax;
    voxels.maxCoord[1] = box[4] + stepMax;
    voxels.maxCoord[2] = box[5] + stepMax;

    voxels.dims[0] = (int)std::ceil((voxels.maxCoord[0] - voxels.minCoord[0]) / step);
    voxels.dims[1] = (int)std::ceil((voxels.maxCoord[1] - voxels.minCoord[1]) / step);
    voxels.dims[2] = (int)std::ceil((voxels.maxCoord[2] - voxels.minCoord[2]) / step);

    int rx = voxels.dims[0];
    int ry = voxels.dims[1];
    int rz = voxels.dims[2];
    int datalen = rx * ry * rz;

    printf("[C++ doRemesh] dims: [%d, %d, %d], step: %.6f, datalen: %d\n", rx, ry, rz, step, datalen);

    // 2. Allocate voxel fields
    voxels.crossedEdges.assign(datalen, 0);
    voxels.distanceField.assign(datalen, std::numeric_limits<float>::infinity());

    voxels.hasColorField = hasColors;
    if (!hasColors) {
        voxels.uniformColor[0] = uniformColor[0];
        voxels.uniformColor[1] = uniformColor[1];
        voxels.uniformColor[2] = uniformColor[2];
    }

    voxels.hasMaterialField = hasMaterials;
    if (!hasMaterials) {
        voxels.uniformMaterial[0] = uniformMaterial[0];
        voxels.uniformMaterial[1] = uniformMaterial[1];
        voxels.uniformMaterial[2] = uniformMaterial[2];
    }

    // 3. Voxelize
    voxelize(verts, nbVerts, tris, nbTris, colors, materials, voxels);

    // 4. Flood fill inside/outside
    floodFill(voxels);

    // 5. Reconstruct Surface
    RemeshResult r;
    if (manifold) {
        r = marchingCubesReconstruct(voxels);
    } else {
        r = surfaceNetsReconstruct(voxels, block);
    }

    for (size_t i = 0; i < r.vertices.size(); i += 3) {
        r.vertices[i]     = voxels.minCoord[0] + r.vertices[i] * step;
        r.vertices[i + 1] = voxels.minCoord[1] + r.vertices[i + 1] * step;
        r.vertices[i + 2] = voxels.minCoord[2] + r.vertices[i + 2] * step;
    }

    printf("[C++ doRemesh] Reconstructed. output verts: %d, faces: %d\n", (int)(r.vertices.size() / 3), (int)(r.faces.size() / 4));
    return r;
}
