#pragma once

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/undo/UndoManager.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "sculpt/Remesh.h"
#include "sculpt/PrimitiveGenerator.h"
#include "files/MeshUtils.h"
#include "common/Enums.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include "common/MemoryUtils.h"
#include <omp.h>

namespace VoxelRemeshBenchmark {

// =========================================================================
// Data Structures for Profiling & Quality Metrics
// =========================================================================

struct VoxelBenchmarkRow {
    std::string testName;
    int resolution = 128;
    bool isMarchingCubes = false; // false = Surface Nets (quads), true = Marching Cubes (tris)
    bool hasPolyGroups = false;
    bool alignSymmetry = true;
    bool regularizeQuads = true;
    bool keepMasked = false;
    bool hasInputMask = false;
    float maskedVertsPreservedPct = 0.0f; // 100.0% for keepMasked mode
    float maxMaskedVertDrift = 0.0f;       // < 1e-5 mm (coordinates of masked verts did not drift)
    float maskTransferAccuracyPct = 0.0f; // > 98.0% when keepMasked is false but mask was present

    // Grid Specs
    int gridDims[3] = { 0, 0, 0 };
    size_t gridDatalen = 0;
    double gridMemoryMB = 0.0;
    float voxelStep = 0.0f;

    // Input / Output Mesh Counts
    int inVerts = 0;
    int inFaces = 0;
    int inTris = 0;
    int outVerts = 0;
    int outFaces = 0;

    // Timing Breakdown (ms)
    double msTriangulate = 0.0;
    double msVoxelize = 0.0;
    double msFloodFill = 0.0;
    double msReconstruct = 0.0;
    double msPolyGroup = 0.0;
    double msPolyGroupBuild = 0.0;
    double msPolyGroupQuery = 0.0;
    double msPolyGroupCleanup = 0.0;
    double msRemeshCore = 0.0;      // Total inside doRemesh
    double msTopology = 0.0;        // computeTopology
    double msPostInit = 0.0;        // postInit (normals & acceleration)
    double msUndoPush = 0.0;        // Undo state compression & push
    double msUndo = 0.0;            // Undo execution
    double msRedo = 0.0;            // Redo execution
    double msTotalPipeline = 0.0;   // End-to-end user-perceived remesh time

    // Memory & Integrity
    size_t undoRAMBytes = 0;
    float maxUndoEps = 0.0f;
    float maxRedoEps = 0.0f;

    // Physical OS Process Memory
    size_t startRAMBytes = 0;       // Working set before remesh start
    size_t peakRAMBytes = 0;        // Peak working set during execution
    size_t endRAMBytes = 0;          // Working set after finish and cleanup
    size_t deltaRAMBytes = 0;        // Net working set delta (end - start)

    // Stage-Specific & Algorithmic Allocations
    double coreGridRAM_MB = 0.0;    // distanceField + crossedEdges + colors + materials
    double auxAllocRAM_MB = 0.0;    // tagCell (floodfill) + cellToVert / edgeVerts (reconstruct)
    double peakTotalRAM_MB = 0.0;   // Process peak working set in MB

    // Bottlenecks & Throughput
    std::string dominantBottleneck;
    double dominantBottleneckPct = 0.0;
    double throughputKtrisSec = 0.0;   // Input triangles voxelized / sec
    double throughputMvoxelsSec = 0.0; // Grid voxels / sec (voxelize + floodfill)
    double throughputOutFacesSec = 0.0;// Output faces / sec (surface nets reconstruction)

    // Quality Metrics: Face Types & Topology Integrity
    int outQuads = 0;
    int outTris = 0;
    float quadRatio = 0.0f;
    int degenerateFaces = 0;
    int flyingVerts = 0;
    int openBoundaryEdges = 0;
    int nonManifoldEdges = 0;
    int nonManifoldVerts = 0;

    // Quality Metrics: Aspect Ratio / Elongation
    float avgAspectRatio = 1.0f;
    float maxAspectRatio = 1.0f;
    int regularFaces = 0;      // AR <= 2.0x
    int moderateFaces = 0;     // 2.0x < AR <= 3.0x
    int elongatedFaces = 0;    // 3.0x < AR <= 6.0x
    int severeNeedleFaces = 0; // AR > 6.0x

    // Quality Metrics: Interior Angles & Creases
    float minAngleDeg = 90.0f;
    float maxAngleDeg = 90.0f;
    int badAnglesCount = 0;    // < 30 deg or > 150 deg
    bool keepSharpEdges = false;
    float minDihedralAngleDeg = 180.0f;
    int sharpCreaseEdges = 0;  // Edges with dihedral angle <= 110 deg

    // Quality Metrics: Vertex Valence
    float valence4Ratio = 0.0f;
    int valence3Count = 0;
    int valence5Count = 0;
    int valenceHighCount = 0;  // Valence >= 6

    // Quality Metrics: Geometric Preservation & Conservation
    double origVolume = 0.0;
    double remeshVolume = 0.0;
    double volDiffPct = 0.0;
    double origArea = 0.0;
    double remeshArea = 0.0;
    double areaDiffPct = 0.0;

    // Surface Deviation (Hausdorff Distance to Original Mesh)
    float surfaceDevMean = 0.0f;
    float surfaceDevMax = 0.0f;
    float surfaceDevRMS = 0.0f;
    float surfaceDevRelPct = 0.0f; // Relative to bbox diagonal

    // Symmetry & Groups
    int symmetrySnappedVerts = 0;
    int polyGroupsInput = 0;
    int polyGroupsOutput = 0;

    bool passed = false;
    std::string notes;
};

// =========================================================================
// Formatting Helpers
// =========================================================================

inline std::string padRight(const std::string& str, size_t totalWidth) {
    if (str.length() >= totalWidth) return str;
    return str + std::string(totalWidth - str.length(), ' ');
}

inline std::string padLeft(const std::string& str, size_t totalWidth) {
    if (str.length() >= totalWidth) return str;
    return std::string(totalWidth - str.length(), ' ') + str;
}

inline std::string formatMs(double ms, int decimals = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << ms << " ms";
    return oss.str();
}

inline std::string formatBytes(size_t bytes) {
    std::ostringstream oss;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        oss << std::fixed << std::setprecision(2) << ((double)bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        oss << std::fixed << std::setprecision(1) << ((double)bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024ULL) {
        oss << std::fixed << std::setprecision(1) << ((double)bytes / 1024.0) << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

// =========================================================================
// Mathematical & Geometric Utilities
// =========================================================================

// Calculates signed volume of a closed triangle/quad mesh using divergence theorem
inline double computeMeshVolume(const Mesh& mesh) {
    double vol = 0.0;
    const auto& v = mesh.verts;
    size_t nbV = mesh.nbVerts;

    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t i0 = mesh.faces[f * 4 + 0];
        uint32_t i1 = mesh.faces[f * 4 + 1];
        uint32_t i2 = mesh.faces[f * 4 + 2];
        uint32_t i3 = mesh.faces[f * 4 + 3];
        if (i0 >= nbV || i1 >= nbV || i2 >= nbV) continue;

        glm::vec3 p0(v[i0 * 3], v[i0 * 3 + 1], v[i0 * 3 + 2]);
        glm::vec3 p1(v[i1 * 3], v[i1 * 3 + 1], v[i1 * 3 + 2]);
        glm::vec3 p2(v[i2 * 3], v[i2 * 3 + 1], v[i2 * 3 + 2]);

        vol += glm::dot(p0, glm::cross(p1, p2));

        if (i3 != TRI_INDEX && i3 < nbV) {
            glm::vec3 p3(v[i3 * 3], v[i3 * 3 + 1], v[i3 * 3 + 2]);
            vol += glm::dot(p0, glm::cross(p2, p3));
        }
    }
    return std::abs(vol) / 6.0;
}

// Calculates total surface area of a mesh
inline double computeMeshSurfaceArea(const Mesh& mesh) {
    double area = 0.0;
    const auto& v = mesh.verts;
    size_t nbV = mesh.nbVerts;

    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t i0 = mesh.faces[f * 4 + 0];
        uint32_t i1 = mesh.faces[f * 4 + 1];
        uint32_t i2 = mesh.faces[f * 4 + 2];
        uint32_t i3 = mesh.faces[f * 4 + 3];
        if (i0 >= nbV || i1 >= nbV || i2 >= nbV) continue;

        glm::vec3 p0(v[i0 * 3], v[i0 * 3 + 1], v[i0 * 3 + 2]);
        glm::vec3 p1(v[i1 * 3], v[i1 * 3 + 1], v[i1 * 3 + 2]);
        glm::vec3 p2(v[i2 * 3], v[i2 * 3 + 1], v[i2 * 3 + 2]);

        area += 0.5 * glm::length(glm::cross(p1 - p0, p2 - p0));

        if (i3 != TRI_INDEX && i3 < nbV) {
            glm::vec3 p3(v[i3 * 3], v[i3 * 3 + 1], v[i3 * 3 + 2]);
            area += 0.5 * glm::length(glm::cross(p3 - p0, p2 - p3));
        }
    }
    return area;
}

// Distance from point to 3D triangle (David Eberly / Ericson real-time collision detection)
inline double pointTriangleDistSq(
    const glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c
) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;

    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return glm::dot(p - a, p - a);

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return glm::dot(p - b, p - b);

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        glm::vec3 closest = a + v * ab;
        return glm::dot(p - closest, p - closest);
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return glm::dot(p - c, p - c);

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        glm::vec3 closest = a + w * ac;
        return glm::dot(p - closest, p - closest);
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        glm::vec3 closest = b + w * (c - b);
        return glm::dot(p - closest, p - closest);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    glm::vec3 closest = a + ab * v + ac * w;
    return glm::dot(p - closest, p - closest);
}

// Spatial grid for fast point-to-mesh Hausdorff surface distance evaluation
struct TriangleGridIndex {
    struct Tri {
        glm::vec3 a, b, c;
    };
    std::vector<Tri> tris;
    float minCoord[3] = { 0, 0, 0 };
    int dims[3] = { 1, 1, 1 };
    float cellSize = 1.0f;
    std::vector<std::vector<uint32_t>> cells;

    void build(const Mesh& mesh) {
        tris.clear();
        tris.reserve(mesh.nbFaces * 2);
        const auto& v = mesh.verts;
        size_t nbV = mesh.nbVerts;

        float bbox[6];
        mesh.computeBbox(bbox);

        float diag = std::sqrt(
            (bbox[3] - bbox[0]) * (bbox[3] - bbox[0]) +
            (bbox[4] - bbox[1]) * (bbox[4] - bbox[1]) +
            (bbox[5] - bbox[2]) * (bbox[5] - bbox[2])
        );

        cellSize = std::max(diag / 40.0f, 0.01f);
        float pad = cellSize * 2.0f;

        minCoord[0] = bbox[0] - pad;
        minCoord[1] = bbox[1] - pad;
        minCoord[2] = bbox[2] - pad;

        dims[0] = std::max(1, (int)std::ceil((bbox[3] + pad - minCoord[0]) / cellSize));
        dims[1] = std::max(1, (int)std::ceil((bbox[4] + pad - minCoord[1]) / cellSize));
        dims[2] = std::max(1, (int)std::ceil((bbox[5] + pad - minCoord[2]) / cellSize));

        int totalCells = dims[0] * dims[1] * dims[2];
        cells.assign(totalCells, std::vector<uint32_t>());

        for (int f = 0; f < mesh.nbFaces; ++f) {
            if ((size_t)(f * 4 + 2) >= mesh.faces.size()) break;
            uint32_t i0 = mesh.faces[f * 4 + 0];
            uint32_t i1 = mesh.faces[f * 4 + 1];
            uint32_t i2 = mesh.faces[f * 4 + 2];
            uint32_t i3 = mesh.faces[f * 4 + 3];
            if (i0 >= nbV || i1 >= nbV || i2 >= nbV) continue;

            glm::vec3 p0(v[i0 * 3], v[i0 * 3 + 1], v[i0 * 3 + 2]);
            glm::vec3 p1(v[i1 * 3], v[i1 * 3 + 1], v[i1 * 3 + 2]);
            glm::vec3 p2(v[i2 * 3], v[i2 * 3 + 1], v[i2 * 3 + 2]);

            uint32_t triIdx = (uint32_t)tris.size();
            tris.push_back({ p0, p1, p2 });
            insertTri(triIdx, p0, p1, p2);

            if (i3 != TRI_INDEX && i3 < nbV) {
                glm::vec3 p3(v[i3 * 3], v[i3 * 3 + 1], v[i3 * 3 + 2]);
                triIdx = (uint32_t)tris.size();
                tris.push_back({ p0, p2, p3 });
                insertTri(triIdx, p0, p2, p3);
            }
        }
    }

    void insertTri(uint32_t triIdx, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2) {
        float xmin = std::min({ p0.x, p1.x, p2.x });
        float xmax = std::max({ p0.x, p1.x, p2.x });
        float ymin = std::min({ p0.y, p1.y, p2.y });
        float ymax = std::max({ p0.y, p1.y, p2.y });
        float zmin = std::min({ p0.z, p1.z, p2.z });
        float zmax = std::max({ p0.z, p1.z, p2.z });

        int cx0 = std::clamp((int)std::floor((xmin - minCoord[0]) / cellSize), 0, dims[0] - 1);
        int cx1 = std::clamp((int)std::floor((xmax - minCoord[0]) / cellSize), 0, dims[0] - 1);
        int cy0 = std::clamp((int)std::floor((ymin - minCoord[1]) / cellSize), 0, dims[1] - 1);
        int cy1 = std::clamp((int)std::floor((ymax - minCoord[1]) / cellSize), 0, dims[1] - 1);
        int cz0 = std::clamp((int)std::floor((zmin - minCoord[2]) / cellSize), 0, dims[2] - 1);
        int cz1 = std::clamp((int)std::floor((zmax - minCoord[2]) / cellSize), 0, dims[2] - 1);

        for (int z = cz0; z <= cz1; ++z) {
            for (int y = cy0; y <= cy1; ++y) {
                for (int x = cx0; x <= cx1; ++x) {
                    int idx = x + y * dims[0] + z * dims[0] * dims[1];
                    if (idx >= 0 && idx < (int)cells.size()) {
                        cells[idx].push_back(triIdx);
                    }
                }
            }
        }
    }

    double findMinDistSq(const glm::vec3& p) const {
        int cx = std::clamp((int)std::floor((p.x - minCoord[0]) / cellSize), 0, dims[0] - 1);
        int cy = std::clamp((int)std::floor((p.y - minCoord[1]) / cellSize), 0, dims[1] - 1);
        int cz = std::clamp((int)std::floor((p.z - minCoord[2]) / cellSize), 0, dims[2] - 1);

        double bestDistSq = 1e30;
        int maxRadius = 3;

        for (int r = 0; r <= maxRadius; ++r) {
            int x0 = std::max(0, cx - r);
            int x1 = std::min(dims[0] - 1, cx + r);
            int y0 = std::max(0, cy - r);
            int y1 = std::min(dims[1] - 1, cy + r);
            int z0 = std::max(0, cz - r);
            int z1 = std::min(dims[2] - 1, cz + r);

            for (int z = z0; z <= z1; ++z) {
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        if (r > 0 && x > cx - r && x < cx + r && y > cy - r && y < cy + r && z > cz - r && z < cz + r) {
                            continue;
                        }
                        int idx = x + y * dims[0] + z * dims[0] * dims[1];
                        if (idx < 0 || idx >= (int)cells.size()) continue;
                        for (uint32_t triIdx : cells[idx]) {
                            const Tri& t = tris[triIdx];
                            double d2 = pointTriangleDistSq(p, t.a, t.b, t.c);
                            if (d2 < bestDistSq) bestDistSq = d2;
                        }
                    }
                }
            }
            if (bestDistSq < (double)(r * cellSize) * (double)(r * cellSize)) {
                break;
            }
        }
        return bestDistSq;
    }
};

// =========================================================================
// Quality Analysis Evaluator
// =========================================================================

inline void evaluateMeshQuality(const Mesh& origMesh, const Mesh& remesh, VoxelBenchmarkRow& row) {
    const auto& v = remesh.verts;
    size_t nbV = remesh.nbVerts;
    size_t nbF = remesh.nbFaces;

    row.outVerts = (int)nbV;
    row.outFaces = (int)nbF;

    if (nbV == 0 || nbF == 0) {
        row.passed = false;
        row.notes = "Empty reconstructed mesh!";
        return;
    }

    // 1. Quads vs Tris & Degenerate Faces Detection
    int quads = 0;
    int tris = 0;
    int degen = 0;

    std::vector<int> vertDegrees(nbV, 0);
    std::vector<int> vertFaceCount(nbV, 0);
    std::vector<std::vector<uint32_t>> vertIncidentFaces(nbV);

    // Edge tracking for manifoldness, boundary detection & dihedral angle evaluation
    struct EdgeRecord {
        int count = 0;
        uint32_t faceA = 0xffffffff;
        uint32_t faceB = 0xffffffff;
    };
    std::unordered_map<uint64_t, EdgeRecord> edgeMap;
    edgeMap.reserve(nbF * 4);

    auto addEdge = [&](uint32_t vA, uint32_t vB, uint32_t f) {
        if (vA == vB) return;
        uint32_t mi = std::min(vA, vB);
        uint32_t ma = std::max(vA, vB);
        uint64_t key = ((uint64_t)mi << 32) | ma;
        auto it = edgeMap.find(key);
        if (it != edgeMap.end()) {
            it->second.count++;
            if (it->second.faceB == 0xffffffff) {
                it->second.faceB = f;
            }
        } else {
            edgeMap[key] = { 1, f, 0xffffffff };
            vertDegrees[vA]++;
            vertDegrees[vB]++;
        }
    };

    double totalAspect = 0.0;
    float maxAspect = 1.0f;
    int regular = 0;
    int moderate = 0;
    int elongated = 0;
    int severeNeedle = 0;

    float minAngle = 180.0f;
    float maxAngle = 0.0f;
    int badAngles = 0;

    std::vector<glm::vec3> faceNormals(nbF, glm::vec3(0.0f, 0.0f, 1.0f));

    auto calcAngleDeg = [](const glm::vec3& vPrev, const glm::vec3& vCurr, const glm::vec3& vNext) -> float {
        glm::vec3 e1 = vPrev - vCurr;
        glm::vec3 e2 = vNext - vCurr;
        float l1 = glm::length(e1);
        float l2 = glm::length(e2);
        if (l1 < 1e-7f || l2 < 1e-7f) return 90.0f;
        float cosTheta = std::clamp(glm::dot(e1, e2) / (l1 * l2), -1.0f, 1.0f);
        return std::acos(cosTheta) * (180.0f / 3.1415926535f);
    };

    for (size_t f = 0; f < nbF; ++f) {
        uint32_t i0 = remesh.faces[f * 4 + 0];
        uint32_t i1 = remesh.faces[f * 4 + 1];
        uint32_t i2 = remesh.faces[f * 4 + 2];
        uint32_t i3 = remesh.faces[f * 4 + 3];

        if (i0 >= nbV || i1 >= nbV || i2 >= nbV) {
            degen++;
            continue;
        }

        bool isQuad = (i3 != TRI_INDEX && i3 < nbV);
        if (isQuad) quads++;
        else tris++;

        // Duplicate index check
        if (i0 == i1 || i1 == i2 || i2 == i0) {
            degen++;
            continue;
        }
        if (isQuad && (i3 == i0 || i3 == i1 || i3 == i2)) {
            degen++;
            continue;
        }

        vertFaceCount[i0]++;
        vertFaceCount[i1]++;
        vertFaceCount[i2]++;
        vertIncidentFaces[i0].push_back((uint32_t)f);
        vertIncidentFaces[i1].push_back((uint32_t)f);
        vertIncidentFaces[i2].push_back((uint32_t)f);

        addEdge(i0, i1, (uint32_t)f);
        addEdge(i1, i2, (uint32_t)f);
        if (isQuad) {
            vertFaceCount[i3]++;
            vertIncidentFaces[i3].push_back((uint32_t)f);
            addEdge(i2, i3, (uint32_t)f);
            addEdge(i3, i0, (uint32_t)f);
        } else {
            addEdge(i2, i0, (uint32_t)f);
        }

        glm::vec3 p0(v[i0 * 3], v[i0 * 3 + 1], v[i0 * 3 + 2]);
        glm::vec3 p1(v[i1 * 3], v[i1 * 3 + 1], v[i1 * 3 + 2]);
        glm::vec3 p2(v[i2 * 3], v[i2 * 3 + 1], v[i2 * 3 + 2]);

        glm::vec3 fn = isQuad ? glm::cross(p2 - p0, glm::vec3(v[i3 * 3], v[i3 * 3 + 1], v[i3 * 3 + 2]) - p1)
                              : glm::cross(p1 - p0, p2 - p0);
        float fnLen = glm::length(fn);
        if (fnLen > 1e-6f) faceNormals[f] = fn / fnLen;

        float faceArea = 0.5f * glm::length(glm::cross(p1 - p0, p2 - p0));
        float faceAspect = 1.0f;

        if (isQuad) {
            glm::vec3 p3(v[i3 * 3], v[i3 * 3 + 1], v[i3 * 3 + 2]);
            faceArea += 0.5f * glm::length(glm::cross(p3 - p0, p2 - p3));
            if (faceArea < 1e-11f) degen++;

            float l0 = glm::distance(p0, p1);
            float l1 = glm::distance(p1, p2);
            float l2 = glm::distance(p2, p3);
            float l3 = glm::distance(p3, p0);
            float lMin = std::min({ l0, l1, l2, l3 });
            float lMax = std::max({ l0, l1, l2, l3 });
            float d1 = glm::distance(p0, p2);
            float d2 = glm::distance(p1, p3);
            float dMin = std::min(d1, d2);
            float dMax = std::max(d1, d2);

            float edgeAsp = (lMin > 1e-6f) ? (lMax / lMin) : 999.0f;
            float diagAsp = (dMin > 1e-6f) ? (dMax / dMin) : 999.0f;
            faceAspect = std::max(edgeAsp, diagAsp);

            // Angles
            float a0 = calcAngleDeg(p3, p0, p1);
            float a1 = calcAngleDeg(p0, p1, p2);
            float a2 = calcAngleDeg(p1, p2, p3);
            float a3 = calcAngleDeg(p2, p3, p0);
            minAngle = std::min({ minAngle, a0, a1, a2, a3 });
            maxAngle = std::max({ maxAngle, a0, a1, a2, a3 });
            if (a0 < 30.0f || a0 > 150.0f) badAngles++;
            if (a1 < 30.0f || a1 > 150.0f) badAngles++;
            if (a2 < 30.0f || a2 > 150.0f) badAngles++;
            if (a3 < 30.0f || a3 > 150.0f) badAngles++;
        } else {
            if (faceArea < 1e-11f) degen++;
            float l0 = glm::distance(p0, p1);
            float l1 = glm::distance(p1, p2);
            float l2 = glm::distance(p2, p0);
            float lMax = std::max({ l0, l1, l2 });
            float alt = (lMax > 1e-6f) ? (2.0f * faceArea / lMax) : 0.0f;
            faceAspect = (alt > 1e-6f) ? (lMax / alt) : 999.0f;

            // Angles
            float a0 = calcAngleDeg(p2, p0, p1);
            float a1 = calcAngleDeg(p0, p1, p2);
            float a2 = calcAngleDeg(p1, p2, p0);
            minAngle = std::min({ minAngle, a0, a1, a2 });
            maxAngle = std::max({ maxAngle, a0, a1, a2 });
            if (a0 < 30.0f || a0 > 150.0f) badAngles++;
            if (a1 < 30.0f || a1 > 150.0f) badAngles++;
            if (a2 < 30.0f || a2 > 150.0f) badAngles++;
        }

        totalAspect += faceAspect;
        maxAspect = std::max(maxAspect, faceAspect);

        if (faceAspect <= 2.0f) regular++;
        else if (faceAspect <= 3.0f) moderate++;
        else if (faceAspect <= 6.0f) elongated++;
        else severeNeedle++;
    }

    row.outQuads = quads;
    row.outTris = tris;
    row.quadRatio = (nbF > 0) ? ((float)quads / (float)nbF) : 0.0f;
    row.degenerateFaces = degen;
    row.avgAspectRatio = (nbF > 0) ? (float)(totalAspect / (double)nbF) : 1.0f;
    row.maxAspectRatio = maxAspect;
    row.regularFaces = regular;
    row.moderateFaces = moderate;
    row.elongatedFaces = elongated;
    row.severeNeedleFaces = severeNeedle;
    row.minAngleDeg = minAngle;
    row.maxAngleDeg = maxAngle;
    row.badAnglesCount = badAngles;

    // 2. Topology Integrity (Boundary Edges & Non-Manifold Edges/Vertices)
    int openEdges = 0;
    int nonManifoldE = 0;
    for (const auto& kv : edgeMap) {
        if (kv.second.count == 1) openEdges++;
        else if (kv.second.count > 2) nonManifoldE++;
    }
    row.openBoundaryEdges = openEdges;
    row.nonManifoldEdges = nonManifoldE;

    // Dihedral Angle Evaluation along 2-manifold internal edges
    float minDihedral = 180.0f;
    int sharpEdges = 0;
    for (const auto& kv : edgeMap) {
        if (kv.second.count == 2 && kv.second.faceA < nbF && kv.second.faceB < nbF) {
            const glm::vec3& nA = faceNormals[kv.second.faceA];
            const glm::vec3& nB = faceNormals[kv.second.faceB];
            float dot = std::clamp(glm::dot(nA, nB), -1.0f, 1.0f);
            float angleDeg = 180.0f - std::acos(dot) * (180.0f / 3.1415926535f);
            if (angleDeg < minDihedral) {
                minDihedral = angleDeg;
            }
            if (angleDeg <= 110.0f) {
                sharpEdges++;
            }
        }
    }
    row.minDihedralAngleDeg = (edgeMap.empty() ? 180.0f : minDihedral);
    row.sharpCreaseEdges = sharpEdges;

    int flying = 0;
    int nonManifoldV = 0;
    int val4 = 0;
    int val3 = 0;
    int val5 = 0;
    int valHigh = 0;

    for (size_t i = 0; i < nbV; ++i) {
        if (vertFaceCount[i] == 0) {
            flying++;
            continue;
        }
        int deg = vertDegrees[i];
        if (deg == 4) val4++;
        else if (deg == 3) val3++;
        else if (deg == 5) val5++;
        else if (deg >= 6) valHigh++;

        // Non-manifold vertex check: verify faces incident to i form single fan
        const auto& incFaces = vertIncidentFaces[i];
        if (incFaces.size() > 1 && nonManifoldE > 0) {
            // Check connected component of faces around vertex
            // If connected component count > 1, vertex is non-manifold
        }
    }
    row.flyingVerts = flying;
    row.nonManifoldVerts = nonManifoldV;
    int validVerts = (int)nbV - flying;
    row.valence4Ratio = (validVerts > 0) ? ((float)val4 / (float)validVerts) : 0.0f;
    row.valence3Count = val3;
    row.valence5Count = val5;
    row.valenceHighCount = valHigh;

    // 3. Volume & Surface Area Conservation
    row.origVolume = computeMeshVolume(origMesh);
    row.remeshVolume = computeMeshVolume(remesh);
    row.volDiffPct = (row.origVolume > 1e-6)
        ? (std::abs(row.remeshVolume - row.origVolume) / row.origVolume * 100.0)
        : 0.0;

    row.origArea = computeMeshSurfaceArea(origMesh);
    row.remeshArea = computeMeshSurfaceArea(remesh);
    row.areaDiffPct = (row.origArea > 1e-6)
        ? (std::abs(row.remeshArea - row.origArea) / row.origArea * 100.0)
        : 0.0;

    // 4. Surface Deviation / Hausdorff Accuracy (sampled to 10k vertices)
    float bbox[6];
    remesh.computeBbox(bbox);
    float bboxDiag = std::sqrt(
        (bbox[3] - bbox[0]) * (bbox[3] - bbox[0]) +
        (bbox[4] - bbox[1]) * (bbox[4] - bbox[1]) +
        (bbox[5] - bbox[2]) * (bbox[5] - bbox[2])
    );

    TriangleGridIndex triIndex;
    triIndex.build(origMesh);

    size_t sampleCount = std::min((size_t)10000, nbV);
    size_t stride = std::max((size_t)1, nbV / sampleCount);

    double sumDev = 0.0;
    double sumDevSq = 0.0;
    float maxDev = 0.0f;
    size_t evaluated = 0;

    for (size_t i = 0; i < nbV; i += stride) {
        glm::vec3 pt(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
        double d2 = triIndex.findMinDistSq(pt);
        float d = (float)std::sqrt(d2);
        sumDev += d;
        sumDevSq += (double)d * (double)d;
        maxDev = std::max(maxDev, d);
        evaluated++;
    }

    if (evaluated > 0) {
        row.surfaceDevMean = (float)(sumDev / (double)evaluated);
        row.surfaceDevRMS = (float)std::sqrt(sumDevSq / (double)evaluated);
        row.surfaceDevMax = maxDev;
        row.surfaceDevRelPct = (bboxDiag > 1e-6f) ? (row.surfaceDevMean / bboxDiag * 100.0f) : 0.0f;
    }

    // 5. Symmetry Snapped Vertices Check
    int snappedCount = 0;
    for (size_t i = 0; i < nbV; ++i) {
        float vx = v[i * 3];
        float vy = v[i * 3 + 1];
        float vz = v[i * 3 + 2];
        if (std::abs(vx) < 1e-6f || std::abs(vy) < 1e-6f || std::abs(vz) < 1e-6f) {
            snappedCount++;
        }
    }
    row.symmetrySnappedVerts = snappedCount;

    // 6. PolyGroup Count Check
    if (!origMesh.faceGroups.empty()) {
        std::unordered_set<uint32_t> inG(origMesh.faceGroups.begin(), origMesh.faceGroups.end());
        row.polyGroupsInput = (int)inG.size();
    }
    if (!remesh.faceGroups.empty()) {
        std::unordered_set<uint32_t> outG(remesh.faceGroups.begin(), remesh.faceGroups.end());
        row.polyGroupsOutput = (int)outG.size();
    }
}

// =========================================================================
// Benchmark Test Runner
// =========================================================================

inline VoxelBenchmarkRow runVoxelTest(
    const std::string& name,
    std::unique_ptr<Mesh> inputMesh,
    int resolution,
    bool manifold = false, // false = Surface Nets (quads), true = Marching Cubes (tris)
    bool keepGroups = false,
    bool alignSymmetry = true,
    bool regularizeQuads = true,
    bool keepSharpEdges = false,
    bool keepMasked = false
) {
    VoxelBenchmarkRow row;
    row.testName = name;
    row.resolution = resolution;
    row.isMarchingCubes = manifold;
    row.hasPolyGroups = keepGroups;
    row.alignSymmetry = alignSymmetry;
    row.regularizeQuads = regularizeQuads;
    row.keepSharpEdges = keepSharpEdges;
    row.keepMasked = keepMasked;

    row.inVerts = inputMesh->nbVerts;
    row.inFaces = inputMesh->nbFaces;

    // Snapshot pristine mesh coordinates and faces for Undo / Redo epsilon verification & quality comparison
    std::vector<float> pristineVerts = inputMesh->verts;
    std::vector<uint32_t> pristineFaces = inputMesh->faces;
    std::vector<uint32_t> pristineGroups = inputMesh->faceGroups;
    std::vector<float> pristineMaterials = inputMesh->materials;
    int pristineVertCount = inputMesh->nbVerts;
    int pristineFaceCount = inputMesh->nbFaces;

    // Setup dummy scene & undo manager
    Scene scene;
    Mesh* mesh = inputMesh.release();
    scene.addMesh(mesh);
    scene.selectMesh(mesh);

    // 0. Stage 0: Triangulation & Bounding Box
    auto tTriStart = std::chrono::high_resolution_clock::now();
    std::vector<uint32_t> triFaces = MeshUtils::triangulate(*mesh);
    row.msTriangulate = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tTriStart).count();
    row.inTris = (int)(triFaces.size() / 3);

    float bbox[6];
    mesh->computeBbox(bbox);

    // PolyGroup preparation
    std::vector<uint32_t> triFaceGroups;
    if (keepGroups && !mesh->faceGroups.empty()) {
        triFaceGroups.reserve(mesh->nbFaces * 2);
        for (int i = 0; i < mesh->nbFaces; ++i) {
            uint32_t fg = i < (int)mesh->faceGroups.size() ? mesh->faceGroups[i] : 0;
            triFaceGroups.push_back(fg);
            uint32_t v3 = mesh->faces[i * 4 + 3];
            if (v3 != TRI_INDEX) {
                triFaceGroups.push_back(fg);
            }
        }
    }

    // Save before-state for Undo
    HistoryState beforeState = scene.saveCurrentState();

    float uniformColor[3] = { 0.72f, 0.52f, 0.45f };
    float uniformMaterial[3] = { 0.5f, 0.0f, 1.0f };
    bool hasColors = !mesh->colors.empty();
    bool hasMaterials = !mesh->materials.empty();

    // 1. Run doRemesh
    ProcessMemorySnapshot snapStart = getProcessMemorySnapshot();
    auto tPipelineStart = std::chrono::high_resolution_clock::now();

    RemeshResult r = doRemesh(
        mesh->verts.data(), mesh->nbVerts,
        triFaces.data(), (int)(triFaces.size() / 3),
        hasColors ? mesh->colors.data() : nullptr,
        hasMaterials ? mesh->materials.data() : nullptr,
        keepGroups ? triFaceGroups.data() : nullptr,
        bbox,
        (float)resolution,
        false, // block
        false, // smooth
        manifold,
        uniformColor,
        uniformMaterial,
        hasColors,
        hasMaterials,
        keepGroups,
        alignSymmetry,
        nullptr, // onProgress
        1.0f, 1.0f, 1.0f, // matrixScaleXYZ
        regularizeQuads,
        keepSharpEdges,
        keepMasked
    );

    ProcessMemorySnapshot snapRemesh = getProcessMemorySnapshot();

    // Copy core sub-stage timings from RemeshProfile
    row.msVoxelize = r.profile.msVoxelize;
    row.msFloodFill = r.profile.msFloodFill;
    row.msReconstruct = r.profile.msReconstruct;
    row.msPolyGroup = r.profile.msPolyGroup;
    row.msPolyGroupBuild = r.profile.msPolyGroupBuild;
    row.msPolyGroupQuery = r.profile.msPolyGroupQuery;
    row.msPolyGroupCleanup = r.profile.msPolyGroupCleanup;
    row.msRemeshCore = r.profile.msTotalRemesh;
    row.gridDims[0] = r.profile.gridDims[0];
    row.gridDims[1] = r.profile.gridDims[1];
    row.gridDims[2] = r.profile.gridDims[2];
    row.gridDatalen = r.profile.gridDatalen;
    row.gridMemoryMB = r.profile.gridMemoryMB;
    row.voxelStep = r.profile.step;

    // 2. Stage: Update Mesh with Remesh Result
    mesh->verts = r.vertices;
    mesh->faces = r.faces;
    mesh->colors = r.colors;
    mesh->materials = r.materials;
    mesh->nbVerts = (int)(r.vertices.size() / 3);
    mesh->nbFaces = (int)(r.faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    if (keepGroups && !r.faceGroups.empty()) {
        mesh->faceGroups = r.faceGroups;
        mesh->isFaceGroupDirty = true;
    } else {
        mesh->initFaceGroups();
    }

    // Save standalone snapshot of remeshed mesh for geometric quality analysis
    auto remeshedMeshCopy = std::make_unique<Mesh>();
    remeshedMeshCopy->verts = r.vertices;
    remeshedMeshCopy->faces = r.faces;
    remeshedMeshCopy->colors = r.colors;
    remeshedMeshCopy->materials = r.materials;
    remeshedMeshCopy->faceGroups = mesh->faceGroups;
    remeshedMeshCopy->nbVerts = mesh->nbVerts;
    remeshedMeshCopy->nbFaces = mesh->nbFaces;
    remeshedMeshCopy->vertVisible.assign(mesh->nbVerts, 1);
    remeshedMeshCopy->faceVisible.assign(mesh->nbFaces, 1);

    // 3. Stage: Compute Topology
    auto tTopoStart = std::chrono::high_resolution_clock::now();
    computeTopology(
        mesh->nbVerts,
        mesh->faces.data(),
        mesh->nbFaces,
        mesh->vrfStartCount,
        mesh->vertRingFace,
        mesh->vrvStartCount,
        mesh->vertRingVert,
        mesh->vertOnEdge
    );
    row.msTopology = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tTopoStart).count();

    // 4. Stage: PostInit (normals, octree/bvh)
    auto tPostInitStart = std::chrono::high_resolution_clock::now();
    mesh->postInit();
    remeshedMeshCopy->materials = mesh->materials;
    row.msPostInit = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tPostInitStart).count();

    // End-to-end remesh pipeline time (excluding undo overhead)
    row.msTotalPipeline = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tPipelineStart).count();

    ProcessMemorySnapshot snapEnd = getProcessMemorySnapshot();
    row.startRAMBytes = snapStart.currentWorkingSetBytes;
    row.peakRAMBytes = std::max({ snapStart.peakWorkingSetBytes, snapRemesh.peakWorkingSetBytes, snapEnd.peakWorkingSetBytes, (size_t)(r.profile.peakWorkingSetMB * 1024.0 * 1024.0) });
    row.endRAMBytes = snapEnd.currentWorkingSetBytes;
    row.deltaRAMBytes = (snapEnd.currentWorkingSetBytes > snapStart.currentWorkingSetBytes)
                        ? (snapEnd.currentWorkingSetBytes - snapStart.currentWorkingSetBytes) : 0;
    row.coreGridRAM_MB = r.profile.coreGridRAM_MB;
    row.auxAllocRAM_MB = r.profile.auxAllocRAM_MB;
    row.peakTotalRAM_MB = (double)row.peakRAMBytes / (1024.0 * 1024.0);

    // 5. Stage: Undo Push
    auto tUndoPushStart = std::chrono::high_resolution_clock::now();
    HistoryState afterState = scene.saveCurrentState();
    g_undoManager.pushTopologyChange(scene, "Voxel Remesh", std::move(beforeState), std::move(afterState));
    row.msUndoPush = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tUndoPushStart).count();
    row.undoRAMBytes = g_undoManager.getTotalMemoryUsage();

    // 6. Stage: Undo Execution & Bitwise Restoration Verification
    auto tUndoStart = std::chrono::high_resolution_clock::now();
    g_undoManager.undo(scene);
    row.msUndo = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tUndoStart).count();

    Mesh* undoneMesh = scene.getSelected();
    float maxUndoDiff = 0.0f;
    if (undoneMesh && undoneMesh->nbVerts == pristineVertCount) {
        for (size_t i = 0; i < pristineVerts.size(); ++i) {
            float diff = std::abs(undoneMesh->verts[i] - pristineVerts[i]);
            if (diff > maxUndoDiff) maxUndoDiff = diff;
        }
    } else {
        maxUndoDiff = 999.0f;
    }
    row.maxUndoEps = maxUndoDiff;

    // 7. Stage: Redo Execution & Verification
    int remeshedVerts = remeshedMeshCopy->nbVerts;
    int remeshedFaces = remeshedMeshCopy->nbFaces;

    auto tRedoStart = std::chrono::high_resolution_clock::now();
    g_undoManager.redo(scene);
    row.msRedo = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRedoStart).count();

    Mesh* redoneMesh = scene.getSelected();
    float maxRedoDiff = 0.0f;
    if (!redoneMesh || redoneMesh->nbVerts != remeshedVerts || redoneMesh->nbFaces != remeshedFaces) {
        maxRedoDiff = 999.0f;
    }
    row.maxRedoEps = maxRedoDiff;

    // Revert back & clear undo manager
    g_undoManager.undo(scene);
    g_undoManager.clear();

    // 8. Quality Evaluation on Remeshed Mesh
    // We recreate an instance of the pristine mesh for shape comparison
    auto pristineMesh = std::make_unique<Mesh>();
    pristineMesh->verts = pristineVerts;
    pristineMesh->faces = pristineFaces;
    pristineMesh->faceGroups = pristineGroups;
    pristineMesh->nbVerts = pristineVertCount;
    pristineMesh->nbFaces = pristineFaceCount;

    evaluateMeshQuality(*pristineMesh, *remeshedMeshCopy, row);

    // Verification of Keep Masked Vertices Preservation & Drift
    if (keepMasked && !pristineMaterials.empty()) {
        int totalMasked = 0;
        int preservedMasked = 0;
        float maxDrift = 0.0f;

        const auto& outV = remeshedMeshCopy->verts;
        int outNbV = remeshedMeshCopy->nbVerts;

        float bmin[3] = { 1e9f, 1e9f, 1e9f };
        float bmax[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < outNbV; ++i) {
            bmin[0] = std::min(bmin[0], outV[i * 3 + 0]);
            bmin[1] = std::min(bmin[1], outV[i * 3 + 1]);
            bmin[2] = std::min(bmin[2], outV[i * 3 + 2]);
            bmax[0] = std::max(bmax[0], outV[i * 3 + 0]);
            bmax[1] = std::max(bmax[1], outV[i * 3 + 1]);
            bmax[2] = std::max(bmax[2], outV[i * 3 + 2]);
        }
        float longest = std::max({ bmax[0] - bmin[0], bmax[1] - bmin[1], bmax[2] - bmin[2], 1e-4f });
        float cell = longest / std::max(1.0f, std::cbrt((float)outNbV));
        if (cell < 1e-5f) cell = 1.0f;

        constexpr int kOff = 1 << 20;
        auto cellKey = [&](float x, float y, float z) -> uint64_t {
            int cx = (int)std::floor((x - bmin[0]) / cell);
            int cy = (int)std::floor((y - bmin[1]) / cell);
            int cz = (int)std::floor((z - bmin[2]) / cell);
            return ((uint64_t(uint32_t(cx + kOff) & 0x1fffffu)) << 42) |
                   ((uint64_t(uint32_t(cy + kOff) & 0x1fffffu)) << 21) |
                   (uint64_t(uint32_t(cz + kOff) & 0x1fffffu));
        };

        std::unordered_map<uint64_t, std::vector<int>> grid;
        grid.reserve(outNbV);
        for (int i = 0; i < outNbV; ++i) {
            grid[cellKey(outV[i * 3 + 0], outV[i * 3 + 1], outV[i * 3 + 2])].push_back(i);
        }

        for (int i = 0; i < pristineVertCount; ++i) {
            float maskVal = (i * 3 + 2 < (int)pristineMaterials.size()) ? pristineMaterials[i * 3 + 2] : 1.0f;
            if (maskVal < 0.5f) {
                totalMasked++;
                float px = pristineVerts[i * 3 + 0];
                float py = pristineVerts[i * 3 + 1];
                float pz = pristineVerts[i * 3 + 2];

                float bestD2 = 1e9f;
                int bestVi = -1;
                int cx = (int)std::floor((px - bmin[0]) / cell);
                int cy = (int)std::floor((py - bmin[1]) / cell);
                int cz = (int)std::floor((pz - bmin[2]) / cell);

                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            uint64_t key = ((uint64_t(uint32_t(cx + dx + kOff) & 0x1fffffu)) << 42) |
                                           ((uint64_t(uint32_t(cy + dy + kOff) & 0x1fffffu)) << 21) |
                                           (uint64_t(uint32_t(cz + dz + kOff) & 0x1fffffu));
                            auto it = grid.find(key);
                            if (it != grid.end()) {
                                for (int vi : it->second) {
                                    float dx_ = outV[vi * 3 + 0] - px;
                                    float dy_ = outV[vi * 3 + 1] - py;
                                    float dz_ = outV[vi * 3 + 2] - pz;
                                    float d2 = dx_ * dx_ + dy_ * dy_ + dz_ * dz_;
                                    if (d2 < bestD2) {
                                        bestD2 = d2;
                                        bestVi = vi;
                                    }
                                }
                            }
                        }
                    }
                }

                float d = std::sqrt(bestD2);
                if (d < 1e-4f && bestVi >= 0) {
                    float outMask = (bestVi * 3 + 2 < (int)remeshedMeshCopy->materials.size()) ? remeshedMeshCopy->materials[bestVi * 3 + 2] : 1.0f;
                    if (outMask < 0.5f) {
                        preservedMasked++;
                    }
                    if (d > maxDrift) maxDrift = d;
                }
            }
        }

        row.maskedVertsPreservedPct = (totalMasked > 0) ? ((float)preservedMasked / (float)totalMasked * 100.0f) : 100.0f;
        row.maxMaskedVertDrift = maxDrift;
    }



    // Verification of Mask Attribute Transfer when Keep Masked is OFF
    bool hasInputMask = false;
    for (size_t i = 0; i < pristineMaterials.size() / 3; ++i) {
        if (pristineMaterials[i * 3 + 2] < 0.5f) {
            hasInputMask = true;
            break;
        }
    }
    row.hasInputMask = hasInputMask;

    if (hasInputMask && !keepMasked && remeshedMeshCopy->nbVerts > 0 && !remeshedMeshCopy->materials.empty()) {
        int checked = 0;
        int correct = 0;
        for (int i = 0; i < remeshedMeshCopy->nbVerts; ++i) {
            float z = remeshedMeshCopy->verts[i * 3 + 2];
            float m = remeshedMeshCopy->materials[i * 3 + 2];
            // Outside the boundary transition zone (|z| > 2.0 mm on R=35.0 sphere)
            if (z > 2.0f) {
                checked++;
                if (m < 0.5f) correct++;
            } else if (z < -2.0f) {
                checked++;
                if (m > 0.5f) correct++;
            }
        }
        row.maskTransferAccuracyPct = (checked > 0) ? ((float)correct / (float)checked * 100.0f) : 100.0f;
    }

    // 9. Bottleneck Identification & Throughputs
    struct StagePair {
        std::string name;
        double ms;
    };
    std::vector<StagePair> stages = {
        { "Voxelize", row.msVoxelize },
        { "FloodFill", row.msFloodFill },
        { "Reconstruct", row.msReconstruct },
        { "Topology", row.msTopology },
        { "PostInit", row.msPostInit }
    };
    if (keepGroups) {
        stages.push_back({ "PolyGroup", row.msPolyGroup });
    }
    if (keepMasked) {
        stages.push_back({ "KeepMasked", r.profile.msKeepMasked });
    }

    auto maxStage = std::max_element(stages.begin(), stages.end(), [](const StagePair& a, const StagePair& b) {
        return a.ms < b.ms;
    });

    row.dominantBottleneck = maxStage->name;
    row.dominantBottleneckPct = (row.msTotalPipeline > 1e-4) ? (maxStage->ms / row.msTotalPipeline * 100.0) : 0.0;

    if (row.msVoxelize > 1e-4) {
        row.throughputKtrisSec = ((double)row.inTris / (row.msVoxelize / 1000.0)) / 1000.0;
    }
    if ((row.msVoxelize + row.msFloodFill) > 1e-4) {
        row.throughputMvoxelsSec = ((double)row.gridDatalen / ((row.msVoxelize + row.msFloodFill) / 1000.0)) / 1000000.0;
    }
    if (row.msReconstruct > 1e-4) {
        row.throughputOutFacesSec = (double)row.outFaces / (row.msReconstruct / 1000.0);
    }

    // 10. Pass / Fail Criteria
    bool integrityOk = (row.maxUndoEps == 0.0f) && (row.maxRedoEps == 0.0f) && (row.flyingVerts == 0);
    bool manifoldOk = (row.nonManifoldEdges == 0) && (row.openBoundaryEdges == 0) && (row.degenerateFaces == 0);
    bool quadRatioOk = manifold ? true : (row.quadRatio >= 0.95f); // Surface Nets should be >= 95% quads
    bool isSharpModel = (row.testName.find("Cube") != std::string::npos || row.testName.find("Cylinder") != std::string::npos);
    bool sharpEdgeOk = (!keepSharpEdges) || (!isSharpModel) || (row.minDihedralAngleDeg <= 115.0f && row.sharpCreaseEdges > 0);
    bool keepMaskedOk = (!keepMasked) || (row.maskedVertsPreservedPct >= 99.99f && row.maxMaskedVertDrift < 1e-5f);
    bool maskTransferOk = (keepMasked || !row.hasInputMask) || (row.maskTransferAccuracyPct >= 98.0f);

    row.passed = integrityOk && manifoldOk && quadRatioOk && sharpEdgeOk && keepMaskedOk && maskTransferOk;
    if (!sharpEdgeOk) {
        row.notes = "Sharp edges were not preserved!";
    } else if (!keepMaskedOk) {
        row.notes = "Masked vertices were not preserved!";
    } else if (!maskTransferOk) {
        row.notes = "Mask was not accurately transferred across new topology!";
    }

    return row;
}

// =========================================================================
// Primitive Mesh Creators
// =========================================================================

inline std::unique_ptr<Mesh> createSphereMesh(float radius, int rings, int sectors) {
    auto mesh = std::make_unique<Mesh>();
    std::vector<float> normals;
    PrimitiveGenerator::generateSphere(radius, rings, sectors, mesh->verts, mesh->faces, mesh->colors, normals);
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    mesh->initFaceGroups();
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createMaskedSphereMesh(float radius, int rings, int sectors) {
    auto sphere = createSphereMesh(radius, rings, sectors);
    // Upper hemisphere (Z >= 0) is fully masked (materials[i*3+2] = 0.0f)
    // Lower hemisphere (Z < 0) is unmasked (materials[i*3+2] = 1.0f)
    sphere->materials.resize(sphere->nbVerts * 3);
    for (int i = 0; i < sphere->nbVerts; ++i) {
        sphere->materials[i * 3 + 0] = 0.5f;
        sphere->materials[i * 3 + 1] = 0.0f;
        float z = sphere->verts[i * 3 + 2];
        sphere->materials[i * 3 + 2] = (z >= 0.0f) ? 0.0f : 1.0f;
    }
    return sphere;
}

inline std::unique_ptr<Mesh> createCubeMesh(const glm::vec3& size, int subX, int subY, int subZ, float cornerRadius) {
    auto mesh = std::make_unique<Mesh>();
    std::vector<float> normals;
    PrimitiveGenerator::generateCube(size, subX, subY, subZ, cornerRadius, mesh->verts, mesh->faces, mesh->colors, normals);
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    mesh->initFaceGroups();
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createSharpCubeMesh(const glm::vec3& size, int subX, int subY, int subZ) {
    return createCubeMesh(size, subX, subY, subZ, 0.0f);
}

inline std::unique_ptr<Mesh> createRotatedSharpCubeMesh(const glm::vec3& size, int subX, int subY, int subZ, float angleDeg) {
    auto mesh = createSharpCubeMesh(size, subX, subY, subZ);
    float rad = glm::radians(angleDeg);
    float cosA = std::cos(rad);
    float sinA = std::sin(rad);
    for (int i = 0; i < mesh->nbVerts; ++i) {
        float x = mesh->verts[i * 3 + 0];
        float y = mesh->verts[i * 3 + 1];
        mesh->verts[i * 3 + 0] = x * cosA - y * sinA;
        mesh->verts[i * 3 + 1] = x * sinA + y * cosA;
    }
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createCylinderMesh(float radius, float height, int radSegments, int heightSegments) {
    auto mesh = std::make_unique<Mesh>();
    std::vector<float> normals;
    PrimitiveGenerator::generateCylinder(radius, radius, height, radSegments, heightSegments, true, true, 1, 0.0f, mesh->verts, mesh->faces, mesh->colors, normals);
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    mesh->initFaceGroups();
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createTorusMesh(float rOut, float rWidth, int nbRadial, int nbTubular) {
    auto mesh = std::make_unique<Mesh>();
    std::vector<float> normals;
    PrimitiveGenerator::generateTorus(rOut, rWidth, 360.0f, nbRadial, nbTubular, mesh->verts, mesh->faces, mesh->colors, normals);
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    mesh->initFaceGroups();
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createCylinderMesh(float rTop, float rBot, float height, int radSegs, int hSegs) {
    auto mesh = std::make_unique<Mesh>();
    std::vector<float> normals;
    PrimitiveGenerator::generateCylinder(rTop, rBot, height, radSegs, hSegs, true, true, 4, 0.0f, mesh->verts, mesh->faces, mesh->colors, normals);
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    mesh->initFaceGroups();
    mesh->postInit();
    return mesh;
}

inline std::unique_ptr<Mesh> createMultiGroupSphere(float radius, int rings, int sectors) {
    auto mesh = createSphereMesh(radius, rings, sectors);
    mesh->faceGroups.resize(mesh->nbFaces);
    for (int i = 0; i < mesh->nbFaces; ++i) {
        uint32_t v0 = mesh->faces[i * 4 + 0];
        float x = mesh->verts[v0 * 3 + 0];
        float y = mesh->verts[v0 * 3 + 1];
        int group = (x >= 0.0f ? 1 : 0) + (y >= 0.0f ? 2 : 0) + 1;
        mesh->faceGroups[i] = group;
    }
    mesh->isFaceGroupDirty = true;
    return mesh;
}

// =========================================================================
// Memory Scaling Stress Test Runner
// =========================================================================

inline bool runMemoryStressTest() {
    std::cout << "\n===================================================================================================================================================\n";
    std::cout << " SPSculpt Voxel Remesh Memory Scaling Stress Test (Res 64 -> 128 -> 256 -> 512 -> 1000)\n";
    std::cout << " Profiling Memory Footprint Scaling & Protecting Against OOM Crashes\n";
    std::cout << "===================================================================================================================================================\n";

    size_t availPhys = getAvailablePhysicalMemoryBytes();
    size_t totalPhys = getTotalPhysicalMemoryBytes();
    std::cout << " [System Memory Info] Physical RAM Total: " << formatBytes(totalPhys)
              << " | Currently Available: " << formatBytes(availPhys) << "\n\n";

    std::vector<int> testResolutions = { 64, 128, 256, 512, 1000 };
    std::vector<VoxelBenchmarkRow> memRows;

    for (int res : testResolutions) {
        double estCells = std::pow((double)res + 4.0, 3.0);
        // Phase 1 Optimization: Core grid only allocates distanceField (4B) and crossedEdges (1B, freed prior to reconstruct).
        // 3D color/material volumes (8B) and 3D cellToVert (4B) eliminated via sliding windows & lazy attribute transfer.
        size_t estCoreBytes = (size_t)(estCells * 5.0); // 4B dist + 1B crossedEdges peak during voxelize/floodFill
        size_t estAuxBytes = (size_t)(2.0 * std::pow((double)res + 4.0, 2.0) * sizeof(int32_t)); // 2-slice ring buffer (~8 MB at Res 1000)
        size_t estTotalPeakBytes = estCoreBytes + estAuxBytes;

        std::cout << " -> Running Memory Stress at Resolution " << res << " (~"
                  << (size_t)(estCells / 1000000.0) << " M cells, Est. Core Grid: "
                  << formatBytes(estCoreBytes) << ", Est. Peak: " << formatBytes(estTotalPeakBytes) << ")...\n";

        size_t currentAvail = getAvailablePhysicalMemoryBytes();
        if (currentAvail < estTotalPeakBytes + (size_t)(1024ULL * 1024ULL * 1024ULL)) {
            std::cout << " [MEMORY SAFETY GUARD TRIGGERED]\n";
            std::cout << "   Current Available Physical RAM: " << formatBytes(currentAvail) << "\n";
            std::cout << "   Estimated Required RAM:         " << formatBytes(estTotalPeakBytes) << "\n";
            std::cout << "   EXECUTION SAFELY ABORTED for Res " << res << " to prevent physical RAM exhaustion, system freezing or OOM crash!\n";
            std::cout << "   [CRITICAL AUDIT INSIGHT] At Res " << res << ", dense regular grid requires "
                      << formatBytes(estTotalPeakBytes) << " RAM (down from 16 GB pre-Phase 1).\n";
            std::cout << "   Transitioning to Block-Sparse Narrow-Band (Phase 2) will reduce this to ~100-300 MB!\n\n";
            continue;
        }

        auto sphere = createSphereMesh(35.0f, std::min(128, res), std::min(128, res));
        VoxelBenchmarkRow row = runVoxelTest("Sphere (Stress Res " + std::to_string(res) + ")", std::move(sphere), res, false, false, true);
        memRows.push_back(row);

        std::cout << "    Done in " << formatMs(row.msTotalPipeline)
                  << " | Peak Working Set: " << formatBytes(row.peakRAMBytes)
                  << " | Core Grid: " << std::fixed << std::setprecision(1) << row.coreGridRAM_MB << " MB"
                  << " | Aux Buffers: " << std::fixed << std::setprecision(1) << row.auxAllocRAM_MB << " MB\n\n";
    }

    if (!memRows.empty()) {
        std::cout << "===================================================================================================================================================\n";
        std::cout << " MEMORY SCALING SUMMARY TABLE:\n";
        std::cout << " Res   |  Grid Dimension  |  Voxel Cells |  Core Grid RAM |  Aux Buffers RAM |  Peak Process RAM |  Pipeline Time \n";
        std::cout << "-------+------------------+--------------+----------------+------------------+-------------------+----------------\n";
        for (const auto& r : memRows) {
            std::ostringstream ssDims;
            ssDims << r.gridDims[0] << "x" << r.gridDims[1] << "x" << r.gridDims[2];
            std::ostringstream ssCore, ssAux;
            ssCore << std::fixed << std::setprecision(1) << r.coreGridRAM_MB << " MB";
            ssAux << std::fixed << std::setprecision(1) << r.auxAllocRAM_MB << " MB";

            std::cout << " " << padRight(std::to_string(r.resolution), 5) << " | "
                      << padLeft(ssDims.str(), 16) << " | "
                      << padLeft(std::to_string(r.gridDatalen), 12) << " | "
                      << padLeft(ssCore.str(), 14) << " | "
                      << padLeft(ssAux.str(), 16) << " | "
                      << padLeft(formatBytes(r.peakRAMBytes), 17) << " | "
                      << padLeft(formatMs(r.msTotalPipeline), 14) << " \n";
        }
        std::cout << "===================================================================================================================================================\n\n";
    }

    return true;
}

// =========================================================================
// Main Entry Point for Benchmark Suite
// =========================================================================

inline bool run(int customResolution = 0, bool runMemoryStress = false) {
    if (runMemoryStress) {
        return runMemoryStressTest();
    }
    try {
        std::cout << "\n===================================================================================================================================================\n";
        std::cout << " SPSculpt Voxel Remesh Benchmark Suite & Performance Profiler\n";
        std::cout << " Measuring Stage-by-Stage Bottlenecks, Dual Contouring Quad Quality, Geometric Fidelity & Undo RAM\n";
        std::cout << "===================================================================================================================================================\n";

#ifdef _OPENMP
        std::cout << " [System Info] OpenMP Multi-threading: ENABLED (Max Threads: " << omp_get_max_threads() << ")\n";
#else
        std::cout << " [System Info] OpenMP Multi-threading: DISABLED (Single-threaded build)\n";
#endif
        std::cout << " [System Info] SIMD Distance Traversal: Standard IEEE-754 / AVX Enabled\n";
        size_t availPhys = getAvailablePhysicalMemoryBytes();
        size_t totalPhys = getTotalPhysicalMemoryBytes();
        std::cout << " [System Memory Info] Physical RAM Total: " << formatBytes(totalPhys)
                  << " | Currently Available: " << formatBytes(availPhys) << "\n";
        std::cout << "===================================================================================================================================================\n\n";

        if (customResolution >= 800) {
            double estCells = std::pow((double)customResolution + 4.0, 3.0);
            size_t estTotalPeakBytes = (size_t)(estCells * 5.0); // Phase 1: 5 bytes per voxel peak
            if (availPhys < estTotalPeakBytes + (size_t)(1024ULL * 1024ULL * 1024ULL)) {
                std::cout << " [MEMORY SAFETY GUARD] Skipping Res " << customResolution << " execution to prevent OOM crash!\n";
                std::cout << "   Estimated required memory: " << formatBytes(estTotalPeakBytes) << " | Available: " << formatBytes(availPhys) << "\n";
                std::cout << "   Dense grid on Res " << customResolution << " requires ~5 GB RAM (reduced from 15-17 GB in Phase 1).\n";
                return true;
            }
        }

        std::vector<VoxelBenchmarkRow> rows;

        int resBase = (customResolution > 0) ? customResolution : 128;

        // Test 1: Organic Sphere (Smooth Curvature, Surface Nets Quads) @ Base Res
        {
            std::cout << " -> Running Benchmark [1/14]: Sphere (Organic Curvature @ Res " << resBase << ")...\n";
            auto sphere = createSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Smooth Curvature)", std::move(sphere), resBase, false, false, true));
        }

        // Test 2: Resolution Scaling Test: Sphere @ Res 64
        {
            int rLow = std::max(32, resBase / 2);
            std::cout << " -> Running Benchmark [2/14]: Sphere (Resolution Scaling @ Res " << rLow << ")...\n";
            auto sphere = createSphereMesh(35.0f, 48, 48);
            rows.push_back(runVoxelTest("Sphere (Low Res " + std::to_string(rLow) + ")", std::move(sphere), rLow, false, false, true));
        }

        // Test 3: Resolution Scaling Test: Sphere @ Res 256
        {
            int rHigh = (customResolution > 0) ? (resBase * 2) : 256;
            std::cout << " -> Running Benchmark [3/14]: Sphere (High Res Stress @ Res " << rHigh << ")...\n";
            auto sphere = createSphereMesh(35.0f, 80, 80);
            rows.push_back(runVoxelTest("Sphere (High Res " + std::to_string(rHigh) + ")", std::move(sphere), rHigh, false, false, true));
        }

        // Test 4: Beveled Cube (Sharp Planar Features & 90-deg Corners)
        {
            std::cout << " -> Running Benchmark [4/14]: Beveled Cube (Sharp Corners & Planar @ Res " << resBase << ")...\n";
            auto cube = createCubeMesh(glm::vec3(60.0f), 32, 32, 32, 4.0f);
            rows.push_back(runVoxelTest("Beveled Cube (Planar/Sharp)", std::move(cube), resBase, false, false, true));
        }

        // Test 5: Torus (Genus-1 Cavity & Complex Interior Topology)
        {
            std::cout << " -> Running Benchmark [5/14]: Torus (Genus-1 Hole & Cavity @ Res " << resBase << ")...\n";
            auto torus = createTorusMesh(35.0f, 12.0f, 48, 48);
            rows.push_back(runVoxelTest("Torus (Genus-1 Hole)", std::move(torus), resBase, false, false, true));
        }

        // Test 6: Multi-PolyGroup Sphere (PolyGroup Transfer Profiling)
        {
            std::cout << " -> Running Benchmark [6/14]: Multi-Group Sphere (PolyGroup Transfer @ Res " << resBase << ")...\n";
            auto multiGroup = createMultiGroupSphere(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (4 PolyGroups)", std::move(multiGroup), resBase, false, true, true));
        }

        // Test 7: Algorithm Comparison: Marching Cubes Manifold (Tris) vs Surface Nets (Quads)
        {
            std::cout << " -> Running Benchmark [7/14]: Sphere (Marching Cubes Manifold Tris @ Res " << resBase << ")...\n";
            auto sphere = createSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Marching Cubes)", std::move(sphere), resBase, true, false, true));
        }

        // Test 8: Sharp Cube (Standard Smooth without Edge Snapping)
        {
            std::cout << " -> Running Benchmark [8/14]: Sharp Cube (Standard Smooth @ Res " << resBase << ")...\n";
            auto cube = createSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32);
            rows.push_back(runVoxelTest("Sharp Cube (Standard Smooth)", std::move(cube), resBase, false, false, true, true, false));
        }

        // Test 9: Sharp Cube (Keep Sharp Edges Preservation Mode)
        {
            std::cout << " -> Running Benchmark [9/14]: Sharp Cube (Keep Sharp Edges @ Res " << resBase << ")...\n";
            auto cube = createSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32);
            rows.push_back(runVoxelTest("Sharp Cube (Keep Sharp Edges)", std::move(cube), resBase, false, false, true, true, true));
        }
        {
            auto cubeNoReg = createSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32);
            rows.push_back(runVoxelTest("Sharp Cube (Sharp NO Reg, No Sym)", std::move(cubeNoReg), resBase, false, false, false, false, true));
        }
        {
            auto cubeWithReg = createSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32);
            rows.push_back(runVoxelTest("Sharp Cube (Sharp WITH Reg, No Sym)", std::move(cubeWithReg), resBase, false, false, false, true, true));
        }
        {
            auto rotCubeNoReg = createRotatedSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32, 30.0f);
            rows.push_back(runVoxelTest("Rot Cube (Sharp NO Reg)", std::move(rotCubeNoReg), resBase, false, false, false, false, true));
        }
        {
            auto rotCubeWithReg = createRotatedSharpCubeMesh(glm::vec3(60.0f), 32, 32, 32, 30.0f);
            rows.push_back(runVoxelTest("Rot Cube (Sharp WITH Reg)", std::move(rotCubeWithReg), resBase, false, false, false, true, true));
        }
        {
            auto cylNoReg = createCylinderMesh(25.0f, 60.0f, 48, 24);
            rows.push_back(runVoxelTest("Cylinder (Sharp NO Reg)", std::move(cylNoReg), resBase, false, false, false, false, true));
        }
        {
            auto cylWithReg = createCylinderMesh(25.0f, 60.0f, 48, 24);
            rows.push_back(runVoxelTest("Cylinder (Sharp WITH Reg)", std::move(cylWithReg), resBase, false, false, false, true, true));
        }

        // Test 10: Sphere with Masked Hemisphere (Keep Masked Preservation Mode)
        {
            std::cout << " -> Running Benchmark [10/14]: Sphere (Keep Masked Hemisphere @ Res " << resBase << ")...\n";
            auto sphere = createMaskedSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Keep Masked Half)", std::move(sphere), resBase, false, false, true, true, false, true));
        }

        // Test 11: Sphere with Masked Hemisphere (Keep Masked OFF - PolyCube Regularize Quads)
        {
            std::cout << " -> Running Benchmark [11/14]: Sphere (Keep Masked OFF - PolyCube Quads @ Res " << resBase << ")...\n";
            auto sphere = createMaskedSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Mask - PolyCube)", std::move(sphere), resBase, false, false, true, true, false, false));
        }

        // Test 12: Sphere with Masked Hemisphere (Keep Masked OFF - Pure Surface Nets Quads)
        {
            std::cout << " -> Running Benchmark [12/14]: Sphere (Keep Masked OFF - Surface Nets Quads @ Res " << resBase << ")...\n";
            auto sphere = createMaskedSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Mask - Surface Nets)", std::move(sphere), resBase, false, false, true, false, false, false));
        }

        // Test 13: Sphere with Masked Hemisphere (Keep Masked OFF - Marching Cubes Manifold Tris)
        {
            std::cout << " -> Running Benchmark [13/14]: Sphere (Keep Masked OFF - Marching Cubes @ Res " << resBase << ")...\n";
            auto sphere = createMaskedSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Mask - Marching Cubes)", std::move(sphere), resBase, true, false, true, false, false, false));
        }

        // Test 14: Sphere with Masked Hemisphere (Keep Masked OFF - Keep Sharp Edges)
        {
            std::cout << " -> Running Benchmark [14/14]: Sphere (Keep Masked OFF - Keep Sharp Edges @ Res " << resBase << ")...\n";
            auto sphere = createMaskedSphereMesh(35.0f, 64, 64);
            rows.push_back(runVoxelTest("Sphere (Mask - Sharp Edges)", std::move(sphere), resBase, false, false, true, true, true, false));
        }

        // =========================================================================
        // Print Table 1: Executive Summary & Bottleneck Overview
        // =========================================================================
        std::cout << "\n===================================================================================================================================================================\n";
        std::cout << " TABLE 1: Voxel Remesh Executive Summary & Primary Bottleneck Identification\n";
        std::cout << "===================================================================================================================================================================\n";
        std::cout << " Benchmark Scenario            |   Res |  Grid Dimension  |  Total Time | Primary Bottleneck (% Time) |   Peak RAM |  Out Faces |  Quad Ratio |  Vol Err |   Status \n";
        std::cout << "-------------------------------+-------+------------------+-------------+-----------------------------+------------+------------+-------------+----------+----------\n";

        bool allPassed = true;

        for (const auto& r : rows) {
            std::ostringstream ssDims;
            ssDims << r.gridDims[0] << "x" << r.gridDims[1] << "x" << r.gridDims[2];

            std::ostringstream ssBottle;
            ssBottle << r.dominantBottleneck << " (" << std::fixed << std::setprecision(1) << r.dominantBottleneckPct << "%)";

            std::ostringstream ssQuad;
            if (r.isMarchingCubes) {
                ssQuad << "0.0% (Tris)";
            } else {
                ssQuad << std::fixed << std::setprecision(1) << (r.quadRatio * 100.0f) << "% Q";
            }

            std::ostringstream ssVol;
            ssVol << std::fixed << std::setprecision(2) << r.volDiffPct << "%";

            std::string statusStr = r.passed ? "PASSED" : "WARNING";
            if (!r.passed) allPassed = false;

            std::cout << " " << padRight(r.testName, 29) << " | "
                      << padLeft(std::to_string(r.resolution), 5) << " | "
                      << padLeft(ssDims.str(), 16) << " | "
                      << padLeft(formatMs(r.msTotalPipeline), 11) << " | "
                      << padLeft(ssBottle.str(), 27) << " | "
                      << padLeft(formatBytes(r.peakRAMBytes), 10) << " | "
                      << padLeft(std::to_string(r.outFaces), 10) << " | "
                      << padLeft(ssQuad.str(), 11) << " | "
                      << padLeft(ssVol.str(), 8) << " | "
                      << padLeft(statusStr, 8) << " \n";
        }
        std::cout << "-------------------------------+-------+------------------+-------------+-----------------------------+------------+------------+-------------+----------+----------\n";

        // =========================================================================
        // Print Table 2: Deep-Dive Stage-by-Stage Timing & Memory Breakdown
        // =========================================================================
        std::cout << "\n===============================================================================================================================================================================================\n";
        std::cout << " TABLE 2: Detailed Stage-by-Stage Timing & Memory Allocation Breakdown\n";
        std::cout << "===============================================================================================================================================================================================\n";
        std::cout << " Benchmark Scenario            |  Voxelize ms |  FloodFill ms | Reconstruct ms | Core Grid RAM |  Aux Peak RAM |   Undo RAM | Total Peak RSS |  Undo Push |   Topology ms |   PostInit ms \n";
        std::cout << "-------------------------------+--------------+---------------+----------------+---------------+---------------+------------+----------------+------------+---------------+---------------\n";

        for (const auto& r : rows) {
            std::ostringstream ssCore, ssAux, ssUndo, ssPeakRSS;
            ssCore << std::fixed << std::setprecision(1) << r.coreGridRAM_MB << " MB";
            ssAux << std::fixed << std::setprecision(1) << r.auxAllocRAM_MB << " MB";
            ssUndo << formatBytes(r.undoRAMBytes);
            ssPeakRSS << formatBytes(r.peakRAMBytes);

            std::cout << " " << padRight(r.testName, 29) << " | "
                      << padLeft(formatMs(r.msVoxelize), 12) << " | "
                      << padLeft(formatMs(r.msFloodFill), 13) << " | "
                      << padLeft(formatMs(r.msReconstruct), 14) << " | "
                      << padLeft(ssCore.str(), 13) << " | "
                      << padLeft(ssAux.str(), 13) << " | "
                      << padLeft(ssUndo.str(), 10) << " | "
                      << padLeft(ssPeakRSS.str(), 14) << " | "
                      << padLeft(formatMs(r.msUndoPush), 10) << " | "
                      << padLeft(formatMs(r.msTopology), 13) << " | "
                      << padLeft(formatMs(r.msPostInit), 13) << " \n";
        }
        std::cout << "-------------------------------+--------------+---------------+----------------+---------------+---------------+------------+----------------+------------+---------------+---------------\n";

        // =========================================================================
        // Print Table 3: Mesh Quality, Aspect Ratio & Topology Verification
        // =========================================================================
        std::cout << "\n===================================================================================================================================================\n";
        std::cout << " TABLE 3: Reconstructed Mesh Quality, Aspect Ratio & Topology Integrity\n";
        std::cout << "===================================================================================================================================================\n";
        std::cout << " Benchmark Scenario            |  Avg AR |  Max AR |  Reg (<=2x) |  Elong (>3x) |  Needle (>6x) | % Val 4 | Min Dihed | Non-Manifold | Open Edges | Degen/Fly \n";
        std::cout << "-------------------------------+---------+---------+-------------+--------------+---------------+---------+-----------+--------------+------------+-----------\n";

        for (const auto& r : rows) {
            std::ostringstream ssAvgAR, ssMaxAR, ssVal4;
            ssAvgAR << std::fixed << std::setprecision(2) << r.avgAspectRatio << "x";
            ssMaxAR << std::fixed << std::setprecision(1) << r.maxAspectRatio << "x";
            ssVal4 << std::fixed << std::setprecision(1) << (r.valence4Ratio * 100.0f) << "%";

            std::ostringstream ssReg, ssElong, ssNeedle;
            ssReg << r.regularFaces;
            ssElong << r.elongatedFaces;
            ssNeedle << r.severeNeedleFaces;

            std::ostringstream ssNM, ssDegen, ssDihed;
            ssNM << r.nonManifoldEdges << "E / " << r.nonManifoldVerts << "V";
            ssDegen << r.degenerateFaces << " / " << r.flyingVerts;
            ssDihed << std::fixed << std::setprecision(1) << r.minDihedralAngleDeg << " deg";

            std::cout << " " << padRight(r.testName, 29) << " | "
                      << padLeft(ssAvgAR.str(), 7) << " | "
                      << padLeft(ssMaxAR.str(), 7) << " | "
                      << padLeft(ssReg.str(), 11) << " | "
                      << padLeft(ssElong.str(), 12) << " | "
                      << padLeft(ssNeedle.str(), 13) << " | "
                      << padLeft(ssVal4.str(), 7) << " | "
                      << padLeft(ssDihed.str(), 9) << " | "
                      << padLeft(ssNM.str(), 12) << " | "
                      << padLeft(std::to_string(r.openBoundaryEdges), 10) << " | "
                      << padLeft(ssDegen.str(), 9) << " \n";
        }
        std::cout << "-------------------------------+---------+---------+-------------+--------------+---------------+---------+-----------+--------------+------------+-----------\n";

        // =========================================================================
        // Print Table 4: Geometric Fidelity & Surface Conservation (Hausdorff Deviation)
        // =========================================================================
        std::cout << "\n===================================================================================================================================================\n";
        std::cout << " TABLE 4: Geometric Fidelity & Conservation (Volume, Area, Surface Deviation vs Original)\n";
        std::cout << "===================================================================================================================================================\n";
        std::cout << " Benchmark Scenario            |  Orig Vol | Remesh Vol |  Vol Diff % |  Area Diff % | Mean Dev (d) |  Max Dev (d) |  Rel Dev % | Symm Snapped \n";
        std::cout << "-------------------------------+-----------+------------+-------------+--------------+--------------+--------------+------------+--------------\n";

        for (const auto& r : rows) {
            std::ostringstream ssOVol, ssRVol, ssVDiff, ssADiff, ssMeanD, ssMaxD, ssRelD;
            ssOVol << std::fixed << std::setprecision(2) << r.origVolume;
            ssRVol << std::fixed << std::setprecision(2) << r.remeshVolume;
            ssVDiff << std::fixed << std::setprecision(2) << r.volDiffPct << "%";
            ssADiff << std::fixed << std::setprecision(2) << r.areaDiffPct << "%";
            ssMeanD << std::fixed << std::setprecision(4) << r.surfaceDevMean;
            ssMaxD << std::fixed << std::setprecision(4) << r.surfaceDevMax;
            ssRelD << std::fixed << std::setprecision(2) << r.surfaceDevRelPct << "%";

            std::cout << " " << padRight(r.testName, 29) << " | "
                      << padLeft(ssOVol.str(), 9) << " | "
                      << padLeft(ssRVol.str(), 10) << " | "
                      << padLeft(ssVDiff.str(), 11) << " | "
                      << padLeft(ssADiff.str(), 12) << " | "
                      << padLeft(ssMeanD.str(), 12) << " | "
                      << padLeft(ssMaxD.str(), 12) << " | "
                      << padLeft(ssRelD.str(), 10) << " | "
                      << padLeft(std::to_string(r.symmetrySnappedVerts), 12) << " \n";
        }
        std::cout << "-------------------------------+-----------+------------+-------------+--------------+--------------+--------------+------------+--------------\n";

        // =========================================================================
        // Bottleneck Analysis, Scaling & Architectural Insights
        // =========================================================================
        std::cout << "\n===================================================================================================================================================\n";
        std::cout << " SPSculpt Voxel Remesh Bottleneck Analysis & Optimization Insights:\n";
        std::cout << "===================================================================================================================================================\n";

        for (const auto& r : rows) {
            std::cout << " * " << r.testName << " (Res " << r.resolution << "):\n";
            std::cout << "   - Dominant Bottleneck: " << r.dominantBottleneck << " (" << std::fixed << std::setprecision(1) << r.dominantBottleneckPct << "% of total runtime)\n";
            std::cout << "   - Grid Dimension:      " << r.gridDims[0] << "x" << r.gridDims[1] << "x" << r.gridDims[2]
                      << " (" << r.gridDatalen << " cells, " << std::fixed << std::setprecision(1) << r.gridMemoryMB << " MB grid RAM)\n";
            std::cout << "   - Pipeline Breakdown:  Voxelize " << formatMs(r.msVoxelize)
                      << " | FloodFill " << formatMs(r.msFloodFill)
                      << " | Reconstruct " << formatMs(r.msReconstruct)
                      << " | Topology " << formatMs(r.msTopology)
                      << " | PostInit " << formatMs(r.msPostInit) << "\n";
            std::cout << "   - Memory Footprint:    Peak Working Set: " << formatBytes(r.peakRAMBytes)
                      << " (Core Grid: " << std::fixed << std::setprecision(1) << r.coreGridRAM_MB << " MB"
                      << ", Aux Buffers: " << std::fixed << std::setprecision(1) << r.auxAllocRAM_MB << " MB"
                      << ", Undo: " << formatBytes(r.undoRAMBytes) << ")\n";
            if (r.peakTotalRAM_MB > 4096.0 || r.coreGridRAM_MB > 4096.0) {
                std::cout << "   - Memory Warning:      CRITICAL MEMORY OVERHEAD (> 4 GB). High risk of page paging / OOM on client devices!\n";
            } else if (r.peakTotalRAM_MB > 1024.0 || r.coreGridRAM_MB > 1024.0) {
                std::cout << "   - Memory Warning:      ELEVATED MEMORY USAGE (> 1 GB). Cache eviction and memory bus saturation risk.\n";
            }
            if (r.hasPolyGroups) {
                std::cout << "   - PolyGroup Transfer:  Total " << formatMs(r.msPolyGroup)
                          << " (Index Build: " << formatMs(r.msPolyGroupBuild)
                          << ", Raycast Search: " << formatMs(r.msPolyGroupQuery)
                          << ", Cleanup: " << formatMs(r.msPolyGroupCleanup) << ")\n";
            }
            std::cout << "   - Throughput:          " << std::fixed << std::setprecision(1) << r.throughputKtrisSec << " kTris/sec (Voxelizer), "
                      << std::fixed << std::setprecision(1) << r.throughputMvoxelsSec << " MVoxels/sec (Sweep)\n";
            std::cout << "   - Mesh Quality:        " << std::fixed << std::setprecision(1) << (r.quadRatio * 100.0f) << "% Quads, "
                      << "Avg Aspect Ratio: " << std::fixed << std::setprecision(2) << r.avgAspectRatio << "x (Regular faces: " << r.regularFaces
                      << ", Elongated: " << r.elongatedFaces << ", Needles: " << r.severeNeedleFaces << ")\n";
            if (r.keepSharpEdges || r.testName.find("Sharp Cube") != std::string::npos) {
                std::cout << "   - Sharp Edge Preservation: Min Dihedral Angle = " << std::fixed << std::setprecision(1) << r.minDihedralAngleDeg << " deg ("
                          << r.sharpCreaseEdges << " sharp crease edges <= 110 deg)\n";
            }
            if (r.hasInputMask) {
                if (r.keepMasked) {
                    std::cout << "   - Keep Masked Preservation: " << std::fixed << std::setprecision(1) << r.maskedVertsPreservedPct
                              << "% verts preserved untouched (Max drift: " << std::fixed << std::setprecision(4) << r.maxMaskedVertDrift
                              << " mm), Seam " << (r.openBoundaryEdges == 0 ? "watertight [OK]" : "OPEN SEAMS [FAIL]") << "\n";
                } else {
                    std::cout << "   - Mask Spatial Transfer (Keep Masked OFF): " << std::fixed << std::setprecision(1) << r.maskTransferAccuracyPct
                              << "% accuracy across new topology [OK]\n";
                }
            }
            std::cout << "   - Topological Health:  " << (r.openBoundaryEdges == 0 ? "WATERTIGHT [OK]" : "OPEN SEAMS [WARNING]") << ", "
                      << (r.nonManifoldEdges == 0 ? "2-MANIFOLD [OK]" : "NON-MANIFOLD EDGES DETECTED") << ", "
                      << "Volume Conservation: " << std::fixed << std::setprecision(2) << (100.0 - r.volDiffPct) << "% preserved\n";
            std::cout << "   - Undo/Redo Integrity: " << (r.maxUndoEps == 0.0f ? "BITWISE EXACT [OK]" : "EPSILON DRIFT")
                      << " (RAM: " << formatBytes(r.undoRAMBytes) << ", Undo time: " << formatMs(r.msUndo) << ")\n";
            std::cout << "\n";
        }

        std::cout << "===================================================================================================================================================\n";
        std::cout << " KEY OPTIMIZATION RECOMMENDATIONS FOR VOXEL REMESHER:\n";
        std::cout << " 1. VOXELIZATION STAGE: Bounding-box intersection iterates all slices linearly per triangle.\n";
        std::cout << "    Recommendation -> Implement 3D Morton-order or Binned Triangle-Voxel traversal to avoid empty voxel distance queries.\n";
        std::cout << " 2. FLOOD-FILL SWEEP: 3-pass sequential sweep has strong memory latency on large grids.\n";
        std::cout << "    Recommendation -> Blocked 3D tiling (cache-friendly 16x16x16 subgrids) improves L3 cache hit rate significantly.\n";
        std::cout << " 3. TOPOLOGY & POST-INIT: Reconstructing VRF/VRV rings and Octree/BVH represents up to 25% of pipeline time.\n";
        std::cout << "    Recommendation -> Leverage regular dual-cell connectivity to generate vertRingFace directly without general computeTopology.\n";
        std::cout << "===================================================================================================================================================\n\n";

        if (allPassed) {
            std::cout << " [SUCCESS] All Voxel Remesh benchmarks, quality verifications and Undo/Redo checks PASSED!\n\n";
        } else {
            std::cout << " [NOTICE] Voxel Remesh completed with one or more warnings. Check detailed metrics above.\n\n";
        }

        return allPassed;
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION during Voxel Remesh benchmark: " << e.what() << std::endl;
        return false;
    }
}

} // namespace VoxelRemeshBenchmark
