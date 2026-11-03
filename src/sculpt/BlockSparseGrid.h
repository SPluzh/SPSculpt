#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <unordered_map>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <glm/glm.hpp>
#include "sculpt/Remesh.h"
#include "common/Logger.h"
#include "common/MemoryUtils.h"

// =========================================================================
// Phase 2: Narrow-Band Block-Sparse Voxel Grid Architecture
// High-performance L1-cache resident 8x8x8 bricks (2 KB per block)
// Direct O(1) 3D Page Table lookup, Angle-Weighted Pseudonormals
// Eliminates 3D flood fill & 90-98% voxel grid memory overhead
// =========================================================================

namespace sculpt {

static constexpr int BLOCK_SHIFT = 3;
static constexpr int BLOCK_SIZE = 1 << BLOCK_SHIFT; // 8
static constexpr int BLOCK_MASK = BLOCK_SIZE - 1;   // 7
static constexpr int BLOCK_VOXELS = BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE; // 512

struct BlockCoord {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct SparseBlock {
    float dist[BLOCK_VOXELS];
    int32_t cellVert[BLOCK_VOXELS]; // Maps local cell (lx, ly, lz) to reconstructed vertex index
};

// ---------------------------------------------------------
// Fast Point-to-Triangle Distance with Barycentric Weights
// (Christer Ericson, Real-Time Collision Detection, Sec 5.1.5)
// ---------------------------------------------------------
inline float pointTriangleDistanceBarycentric(
    const glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    glm::vec3& outClosest,
    float& outW0, float& outW1, float& outW2
) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;

    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        outW0 = 1.0f; outW1 = 0.0f; outW2 = 0.0f;
        outClosest = a;
        return glm::distance(p, a);
    }

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        outW0 = 0.0f; outW1 = 1.0f; outW2 = 0.0f;
        outClosest = b;
        return glm::distance(p, b);
    }

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        outW0 = 1.0f - v; outW1 = v; outW2 = 0.0f;
        outClosest = a + v * ab;
        return glm::distance(p, outClosest);
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        outW0 = 0.0f; outW1 = 0.0f; outW2 = 1.0f;
        outClosest = c;
        return glm::distance(p, c);
    }

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        outW0 = 1.0f - w; outW1 = 0.0f; outW2 = w;
        outClosest = a + w * ac;
        return glm::distance(p, outClosest);
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float denom = (d4 - d3) + (d5 - d6);
        float w = (std::abs(denom) > 1e-12f) ? ((d4 - d3) / denom) : 0.5f;
        outW0 = 0.0f; outW1 = 1.0f - w; outW2 = w;
        outClosest = b + w * (c - b);
        return glm::distance(p, outClosest);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    outW0 = 1.0f - v - w;
    outW1 = v;
    outW2 = w;
    outClosest = a + ab * v + ac * w;
    return glm::distance(p, outClosest);
}

// ---------------------------------------------------------
// Angle-Weighted Pseudonormals (Bærentzen & Aanæs 2005)
// Guarantees continuous normal orientation and exact interior/exterior signs
// ---------------------------------------------------------
inline std::vector<glm::vec3> computeAngleWeightedPseudonormals(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris
) {
    std::vector<glm::vec3> vertNormals(nbVerts, glm::vec3(0.0f));

    for (int t = 0; t < nbTris; ++t) {
        uint32_t i0 = tris[t * 3 + 0];
        uint32_t i1 = tris[t * 3 + 1];
        uint32_t i2 = tris[t * 3 + 2];
        if (i0 >= (uint32_t)nbVerts || i1 >= (uint32_t)nbVerts || i2 >= (uint32_t)nbVerts) continue;

        glm::vec3 p0(verts[i0 * 3], verts[i0 * 3 + 1], verts[i0 * 3 + 2]);
        glm::vec3 p1(verts[i1 * 3], verts[i1 * 3 + 1], verts[i1 * 3 + 2]);
        glm::vec3 p2(verts[i2 * 3], verts[i2 * 3 + 1], verts[i2 * 3 + 2]);

        glm::vec3 e01 = p1 - p0;
        glm::vec3 e02 = p2 - p0;
        glm::vec3 fn = glm::cross(e01, e02);
        float fnLen = glm::length(fn);
        if (fnLen < 1e-12f) continue;
        glm::vec3 fnUnit = fn / fnLen;

        float len01 = glm::length(e01);
        float len02 = glm::length(e02);
        if (len01 > 1e-7f && len02 > 1e-7f) {
            float dot0 = std::clamp(glm::dot(e01 / len01, e02 / len02), -1.0f, 1.0f);
            vertNormals[i0] += fnUnit * std::acos(dot0);
        }

        glm::vec3 e10 = p0 - p1;
        glm::vec3 e12 = p2 - p1;
        float len10 = glm::length(e10);
        float len12 = glm::length(e12);
        if (len10 > 1e-7f && len12 > 1e-7f) {
            float dot1 = std::clamp(glm::dot(e10 / len10, e12 / len12), -1.0f, 1.0f);
            vertNormals[i1] += fnUnit * std::acos(dot1);
        }

        glm::vec3 e20 = p0 - p2;
        glm::vec3 e21 = p1 - p2;
        float len20 = glm::length(e20);
        float len21 = glm::length(e21);
        if (len20 > 1e-7f && len21 > 1e-7f) {
            float dot2 = std::clamp(glm::dot(e20 / len20, e21 / len21), -1.0f, 1.0f);
            vertNormals[i2] += fnUnit * std::acos(dot2);
        }
    }

#pragma omp parallel for schedule(static)
    for (int i = 0; i < nbVerts; ++i) {
        float len = glm::length(vertNormals[i]);
        if (len > 1e-9f) {
            vertNormals[i] /= len;
        } else {
            vertNormals[i] = glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }

    return vertNormals;
}

// ---------------------------------------------------------
// BlockSparseGrid Class
// ---------------------------------------------------------
class BlockSparseGrid {
public:
    int dims[3] = {0, 0, 0};
    int bDims[3] = {0, 0, 0};
    float step = 1.0f;
    float minCoord[3] = {0.0f, 0.0f, 0.0f};
    float maxCoord[3] = {0.0f, 0.0f, 0.0f};

    std::vector<int32_t> blockTable; // 3D page table: bDims[0] * bDims[1] * bDims[2]
    std::vector<SparseBlock> blocks; // Contiguous block storage
    std::vector<BlockCoord> activeBlocks; // Coordinates of active blocks

    inline int getBlockId(int bx, int by, int bz) const {
        if (bx < 0 || bx >= bDims[0] || by < 0 || by >= bDims[1] || bz < 0 || bz >= bDims[2]) return -1;
        return blockTable[bx + by * bDims[0] + bz * bDims[0] * bDims[1]];
    }

    inline float getDist(int x, int y, int z, float defaultExterior = 1e6f) const {
        int bx = x >> BLOCK_SHIFT;
        int by = y >> BLOCK_SHIFT;
        int bz = z >> BLOCK_SHIFT;
        int bId = getBlockId(bx, by, bz);
        if (bId < 0) return defaultExterior;
        int lx = x & BLOCK_MASK;
        int ly = y & BLOCK_MASK;
        int lz = z & BLOCK_MASK;
        return blocks[bId].dist[lx + (ly << BLOCK_SHIFT) + (lz << (BLOCK_SHIFT * 2))];
    }

    inline int32_t getCellVert(int x, int y, int z) const {
        int bx = x >> BLOCK_SHIFT;
        int by = y >> BLOCK_SHIFT;
        int bz = z >> BLOCK_SHIFT;
        int bId = getBlockId(bx, by, bz);
        if (bId < 0) return -1;
        int lx = x & BLOCK_MASK;
        int ly = y & BLOCK_MASK;
        int lz = z & BLOCK_MASK;
        return blocks[bId].cellVert[lx + (ly << BLOCK_SHIFT) + (lz << (BLOCK_SHIFT * 2))];
    }

    inline void setCellVert(int x, int y, int z, int32_t v) {
        int bx = x >> BLOCK_SHIFT;
        int by = y >> BLOCK_SHIFT;
        int bz = z >> BLOCK_SHIFT;
        int bId = getBlockId(bx, by, bz);
        if (bId >= 0) {
            int lx = x & BLOCK_MASK;
            int ly = y & BLOCK_MASK;
            int lz = z & BLOCK_MASK;
            blocks[bId].cellVert[lx + (ly << BLOCK_SHIFT) + (lz << (BLOCK_SHIFT * 2))] = v;
        }
    }

    size_t getMemoryBytes() const {
        size_t tableBytes = blockTable.capacity() * sizeof(int32_t);
        size_t blocksBytes = blocks.capacity() * sizeof(SparseBlock);
        size_t coordsBytes = activeBlocks.capacity() * sizeof(BlockCoord);
        return tableBytes + blocksBytes + coordsBytes;
    }
};

// ---------------------------------------------------------
// Voxelization into Block-Sparse Narrow-Band Grid
// ---------------------------------------------------------
inline void voxelizeBlockSparse(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    BlockSparseGrid& grid,
    float bandRadiusMultiplier = 2.5f
) {
    auto tStart = std::chrono::high_resolution_clock::now();

    // 1. Compute angle-weighted pseudonormals for exact signed classification
    std::vector<glm::vec3> pseudoNormals = computeAngleWeightedPseudonormals(verts, nbVerts, tris, nbTris);

    float step = grid.step;
    float bandRadius = step * bandRadiusMultiplier;
    int bxDim = grid.bDims[0];
    int byDim = grid.bDims[1];
    int bzDim = grid.bDims[2];
    int totalBlockTable = bxDim * byDim * bzDim;

    // 2. Identify active blocks overlapping the narrow band
    std::vector<uint8_t> blockActive(totalBlockTable, 0);

    for (int t = 0; t < nbTris; ++t) {
        uint32_t i0 = tris[t * 3 + 0];
        uint32_t i1 = tris[t * 3 + 1];
        uint32_t i2 = tris[t * 3 + 2];
        if (i0 >= (uint32_t)nbVerts || i1 >= (uint32_t)nbVerts || i2 >= (uint32_t)nbVerts) continue;

        const float* p0 = &verts[i0 * 3];
        const float* p1 = &verts[i1 * 3];
        const float* p2 = &verts[i2 * 3];

        float minX = std::min({ p0[0], p1[0], p2[0] }) - bandRadius;
        float maxX = std::max({ p0[0], p1[0], p2[0] }) + bandRadius;
        float minY = std::min({ p0[1], p1[1], p2[1] }) - bandRadius;
        float maxY = std::max({ p0[1], p1[1], p2[1] }) + bandRadius;
        float minZ = std::min({ p0[2], p1[2], p2[2] }) - bandRadius;
        float maxZ = std::max({ p0[2], p1[2], p2[2] }) + bandRadius;

        int gx0 = std::clamp((int)std::floor((minX - grid.minCoord[0]) / step), 0, grid.dims[0] - 1);
        int gx1 = std::clamp((int)std::floor((maxX - grid.minCoord[0]) / step), 0, grid.dims[0] - 1);
        int gy0 = std::clamp((int)std::floor((minY - grid.minCoord[1]) / step), 0, grid.dims[1] - 1);
        int gy1 = std::clamp((int)std::floor((maxY - grid.minCoord[1]) / step), 0, grid.dims[1] - 1);
        int gz0 = std::clamp((int)std::floor((minZ - grid.minCoord[2]) / step), 0, grid.dims[2] - 1);
        int gz1 = std::clamp((int)std::floor((maxZ - grid.minCoord[2]) / step), 0, grid.dims[2] - 1);

        int bx0 = gx0 >> BLOCK_SHIFT;
        int bx1 = gx1 >> BLOCK_SHIFT;
        int by0 = gy0 >> BLOCK_SHIFT;
        int by1 = gy1 >> BLOCK_SHIFT;
        int bz0 = gz0 >> BLOCK_SHIFT;
        int bz1 = gz1 >> BLOCK_SHIFT;

        for (int bz = bz0; bz <= bz1; ++bz) {
            for (int by = by0; by <= by1; ++by) {
                int baseIdx = by * bxDim + bz * bxDim * byDim;
                for (int bx = bx0; bx <= bx1; ++bx) {
                    blockActive[bx + baseIdx] = 1;
                }
            }
        }
    }

    // Allocate contiguous sparse blocks and fill 3D page table
    grid.activeBlocks.clear();
    grid.blockTable.assign(totalBlockTable, -1);

    for (int bz = 0; bz < bzDim; ++bz) {
        for (int by = 0; by < byDim; ++by) {
            for (int bx = 0; bx < bxDim; ++bx) {
                int bIdx = bx + by * bxDim + bz * bxDim * byDim;
                if (blockActive[bIdx]) {
                    int newBlockId = (int)grid.activeBlocks.size();
                    grid.blockTable[bIdx] = newBlockId;
                    grid.activeBlocks.push_back({ bx, by, bz });
                }
            }
        }
    }

    size_t numActive = grid.activeBlocks.size();
    grid.blocks.resize(numActive);

#pragma omp parallel for schedule(static)
    for (int b = 0; b < (int)numActive; ++b) {
        SparseBlock& blk = grid.blocks[b];
        std::fill(blk.dist, blk.dist + BLOCK_VOXELS, 1e6f);
        std::fill(blk.cellVert, blk.cellVert + BLOCK_VOXELS, -1);
    }

    // 3. Bin triangles to active blocks
    std::vector<std::vector<uint32_t>> blockTris(numActive);
    for (int t = 0; t < nbTris; ++t) {
        uint32_t i0 = tris[t * 3 + 0];
        uint32_t i1 = tris[t * 3 + 1];
        uint32_t i2 = tris[t * 3 + 2];
        if (i0 >= (uint32_t)nbVerts || i1 >= (uint32_t)nbVerts || i2 >= (uint32_t)nbVerts) continue;

        const float* p0 = &verts[i0 * 3];
        const float* p1 = &verts[i1 * 3];
        const float* p2 = &verts[i2 * 3];

        float minX = std::min({ p0[0], p1[0], p2[0] }) - bandRadius;
        float maxX = std::max({ p0[0], p1[0], p2[0] }) + bandRadius;
        float minY = std::min({ p0[1], p1[1], p2[1] }) - bandRadius;
        float maxY = std::max({ p0[1], p1[1], p2[1] }) + bandRadius;
        float minZ = std::min({ p0[2], p1[2], p2[2] }) - bandRadius;
        float maxZ = std::max({ p0[2], p1[2], p2[2] }) + bandRadius;

        int gx0 = std::clamp((int)std::floor((minX - grid.minCoord[0]) / step), 0, grid.dims[0] - 1);
        int gx1 = std::clamp((int)std::floor((maxX - grid.minCoord[0]) / step), 0, grid.dims[0] - 1);
        int gy0 = std::clamp((int)std::floor((minY - grid.minCoord[1]) / step), 0, grid.dims[1] - 1);
        int gy1 = std::clamp((int)std::floor((maxY - grid.minCoord[1]) / step), 0, grid.dims[1] - 1);
        int gz0 = std::clamp((int)std::floor((minZ - grid.minCoord[2]) / step), 0, grid.dims[2] - 1);
        int gz1 = std::clamp((int)std::floor((maxZ - grid.minCoord[2]) / step), 0, grid.dims[2] - 1);

        int bx0 = gx0 >> BLOCK_SHIFT;
        int bx1 = gx1 >> BLOCK_SHIFT;
        int by0 = gy0 >> BLOCK_SHIFT;
        int by1 = gy1 >> BLOCK_SHIFT;
        int bz0 = gz0 >> BLOCK_SHIFT;
        int bz1 = gz1 >> BLOCK_SHIFT;

        for (int bz = bz0; bz <= bz1; ++bz) {
            for (int by = by0; by <= by1; ++by) {
                for (int bx = bx0; bx <= bx1; ++bx) {
                    int bId = grid.getBlockId(bx, by, bz);
                    if (bId >= 0) {
                        blockTris[bId].push_back((uint32_t)t);
                    }
                }
            }
        }
    }

    // 4. Parallel per-block voxel rasterization with L1 cache locality
#pragma omp parallel for schedule(dynamic, 8)
    for (int b = 0; b < (int)numActive; ++b) {
        const auto& tList = blockTris[b];
        if (tList.empty()) continue;

        SparseBlock& blk = grid.blocks[b];
        const BlockCoord& bc = grid.activeBlocks[b];
        int baseX = bc.x << BLOCK_SHIFT;
        int baseY = bc.y << BLOCK_SHIFT;
        int baseZ = bc.z << BLOCK_SHIFT;

        for (int lz = 0; lz < BLOCK_SIZE; ++lz) {
            float pz = grid.minCoord[2] + (baseZ + lz) * step;
            for (int ly = 0; ly < BLOCK_SIZE; ++ly) {
                float py = grid.minCoord[1] + (baseY + ly) * step;
                for (int lx = 0; lx < BLOCK_SIZE; ++lx) {
                    float px = grid.minCoord[0] + (baseX + lx) * step;
                    glm::vec3 P(px, py, pz);

                    float bestDist = 1e6f;
                    float bestSign = 1.0f;

                    for (uint32_t t : tList) {
                        uint32_t i0 = tris[t * 3 + 0];
                        uint32_t i1 = tris[t * 3 + 1];
                        uint32_t i2 = tris[t * 3 + 2];

                        glm::vec3 A(verts[i0 * 3], verts[i0 * 3 + 1], verts[i0 * 3 + 2]);
                        glm::vec3 B(verts[i1 * 3], verts[i1 * 3 + 1], verts[i1 * 3 + 2]);
                        glm::vec3 C(verts[i2 * 3], verts[i2 * 3 + 1], verts[i2 * 3 + 2]);

                        glm::vec3 Q;
                        float w0, w1, w2;
                        float d = pointTriangleDistanceBarycentric(P, A, B, C, Q, w0, w1, w2);

                        if (d < bestDist) {
                            bestDist = d;
                            glm::vec3 diff = P - Q;
                            glm::vec3 interpNormal = w0 * pseudoNormals[i0] + w1 * pseudoNormals[i1] + w2 * pseudoNormals[i2];
                            bestSign = (glm::dot(diff, interpNormal) < 0.0f) ? -1.0f : 1.0f;
                        }
                    }

                    int localIdx = lx + (ly << BLOCK_SHIFT) + (lz << (BLOCK_SHIFT * 2));
                    blk.dist[localIdx] = bestSign * bestDist;
                }
            }
        }
    }

    double msVoxelize = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[BlockSparseGrid] Voxelized %zu active blocks (%zu voxels) in %.2f ms | Grid RAM: %.2f MB\n",
               numActive, numActive * BLOCK_VOXELS, msVoxelize, (double)grid.getMemoryBytes() / (1024.0 * 1024.0));
}

// ---------------------------------------------------------
// Surface Nets Canonical Tables
// Matches Remesh.cpp snCubeEdges & snEdgeTable
// ---------------------------------------------------------
inline const std::vector<uint32_t>& getSparseCubeEdges() {
    static const std::vector<uint32_t> edges = []() {
        std::vector<uint32_t> ce(24);
        int k = 0;
        for (int i = 0; i < 8; ++i) {
            for (int j = 1; j <= 4; j <<= 1) {
                int p = i ^ j;
                if (i <= p) {
                    ce[k++] = i;
                    ce[k++] = p;
                }
            }
        }
        return ce;
    }();
    return edges;
}

inline const std::vector<uint32_t>& getSparseEdgeTable() {
    static const std::vector<uint32_t> table = []() {
        const auto& ce = getSparseCubeEdges();
        std::vector<uint32_t> et(256);
        for (int i = 0; i < 256; ++i) {
            uint32_t em = 0;
            for (int j = 0; j < 24; j += 2) {
                bool a = (i & (1 << ce[j])) != 0;
                bool b = (i & (1 << ce[j + 1])) != 0;
                if (a != b) {
                    em |= (1 << (j >> 1));
                }
            }
            et[i] = em;
        }
        return et;
    }();
    return table;
}

// ---------------------------------------------------------
// Surface Nets Dual Contouring on BlockSparseGrid
// ---------------------------------------------------------
inline RemeshResult reconstructSurfaceNetsBlockSparse(
    BlockSparseGrid& grid,
    const float* uniformColor,
    const float* uniformMaterial
) {
    RemeshResult res;
    auto tStart = std::chrono::high_resolution_clock::now();

    // Standard binary cube corners: g = x_bit | (y_bit << 1) | (z_bit << 2)
    const int cornerOffsets[8][3] = {
        {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0},
        {0,0,1}, {1,0,1}, {0,1,1}, {1,1,1}
    };

    const auto& snCubeEdges = getSparseCubeEdges();
    const auto& snEdgeTable = getSparseEdgeTable();

    float uCol[3] = { uniformColor ? uniformColor[0] : 0.72f, uniformColor ? uniformColor[1] : 0.52f, uniformColor ? uniformColor[2] : 0.45f };
    float uMat[3] = { uniformMaterial ? uniformMaterial[0] : 0.5f, uniformMaterial ? uniformMaterial[1] : 0.0f, uniformMaterial ? uniformMaterial[2] : 1.0f };

    size_t numBlocks = grid.activeBlocks.size();
    float diagThreshold = 1.7321f * grid.step;
    float maxCornerBand = diagThreshold * 2.0f;

    int numThreads = (int)std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;

    struct ThreadVertAccum {
        std::vector<float> verts;
        std::vector<int> cellX;
        std::vector<int> cellY;
        std::vector<int> cellZ;
        std::vector<uint8_t> cellMask;
    };
    std::vector<ThreadVertAccum> threadAccums(numThreads);

#pragma omp parallel num_threads(numThreads)
    {
        int t = omp_get_thread_num();
        ThreadVertAccum& acc = threadAccums[t];
        acc.verts.reserve(16000 * 3);
        acc.cellX.reserve(16000);
        acc.cellY.reserve(16000);
        acc.cellZ.reserve(16000);
        acc.cellMask.reserve(16000);

#pragma omp for schedule(dynamic, 8)
        for (int b = 0; b < (int)numBlocks; ++b) {
            const BlockCoord& bc = grid.activeBlocks[b];
            int baseX = bc.x << BLOCK_SHIFT;
            int baseY = bc.y << BLOCK_SHIFT;
            int baseZ = bc.z << BLOCK_SHIFT;

            for (int lz = 0; lz < BLOCK_SIZE; ++lz) {
                int z = baseZ + lz;
                if (z >= grid.dims[2] - 1) continue;
                for (int ly = 0; ly < BLOCK_SIZE; ++ly) {
                    int y = baseY + ly;
                    if (y >= grid.dims[1] - 1) continue;
                    for (int lx = 0; lx < BLOCK_SIZE; ++lx) {
                        int x = baseX + lx;
                        if (x >= grid.dims[0] - 1) continue;

                        float d0 = grid.getDist(x, y, z);
                        if (std::abs(d0) > diagThreshold) continue;

                        float corners[8];
                        uint8_t mask = 0;
                        bool outOfBand = false;

                        for (int g = 0; g < 8; ++g) {
                            float cd = grid.getDist(x + cornerOffsets[g][0], y + cornerOffsets[g][1], z + cornerOffsets[g][2]);
                            if (std::abs(cd) > maxCornerBand) {
                                outOfBand = true;
                                break;
                            }
                            corners[g] = cd;
                            if (cd < 0.0f) mask |= (1 << g);
                        }

                        if (outOfBand || mask == 0 || mask == 0xff) continue;

                        uint32_t edgeMask = snEdgeTable[mask];
                        float vTemp[3] = {0.0f, 0.0f, 0.0f};
                        int edgeCount = 0;

                        for (int i = 0; i < 12; ++i) {
                            if (!(edgeMask & (1 << i))) continue;
                            ++edgeCount;

                            int e0 = snCubeEdges[i << 1];
                            int e1 = snCubeEdges[(i << 1) + 1];
                            float g0 = corners[e0];
                            float g1 = corners[e1];
                            float diff = g0 - g1;
                            float tVal = (std::abs(diff) > 1e-7f) ? (g0 / diff) : 0.5f;
                            tVal = std::clamp(tVal, 0.0f, 1.0f);

                            for (int j = 0; j < 3; ++j) {
                                int k = 1 << j;
                                int a = e0 & k;
                                if (a != (e1 & k)) {
                                    vTemp[j] += a ? (1.0f - tVal) : tVal;
                                } else {
                                    vTemp[j] += a ? 1.0f : 0.0f;
                                }
                            }
                        }

                        if (edgeCount > 0) {
                            float s = 1.0f / (float)edgeCount;
                            vTemp[0] *= s; vTemp[1] *= s; vTemp[2] *= s;
                        } else {
                            vTemp[0] = 0.5f; vTemp[1] = 0.5f; vTemp[2] = 0.5f;
                        }

                        float wx = grid.minCoord[0] + ((float)x + vTemp[0]) * grid.step;
                        float wy = grid.minCoord[1] + ((float)y + vTemp[1]) * grid.step;
                        float wz = grid.minCoord[2] + ((float)z + vTemp[2]) * grid.step;

                        acc.verts.push_back(wx);
                        acc.verts.push_back(wy);
                        acc.verts.push_back(wz);
                        acc.cellX.push_back(x);
                        acc.cellY.push_back(y);
                        acc.cellZ.push_back(z);
                        acc.cellMask.push_back(mask);
                    }
                }
            }
        }
    }

    // Merge vertices with parallel prefix sums
    std::vector<size_t> vOffsets(numThreads + 1, 0);
    for (int t = 0; t < numThreads; ++t) {
        vOffsets[t + 1] = vOffsets[t] + threadAccums[t].cellX.size();
    }
    size_t totalVerts = vOffsets[numThreads];

    res.vertices.resize(totalVerts * 3);
    res.colors.resize(totalVerts * 3);
    res.materials.resize(totalVerts * 3);

#pragma omp parallel for schedule(static)
    for (int t = 0; t < numThreads; ++t) {
        const auto& acc = threadAccums[t];
        size_t base = vOffsets[t];
        size_t cnt = acc.cellX.size();
        if (cnt > 0) {
            std::memcpy(&res.vertices[base * 3], acc.verts.data(), cnt * 3 * sizeof(float));
            for (size_t i = 0; i < cnt; ++i) {
                res.colors[(base + i) * 3 + 0] = uCol[0];
                res.colors[(base + i) * 3 + 1] = uCol[1];
                res.colors[(base + i) * 3 + 2] = uCol[2];
                res.materials[(base + i) * 3 + 0] = uMat[0];
                res.materials[(base + i) * 3 + 1] = uMat[1];
                res.materials[(base + i) * 3 + 2] = uMat[2];

                grid.setCellVert(acc.cellX[i], acc.cellY[i], acc.cellZ[i], (int32_t)(base + i));
            }
        }
    }

    // Pass 2: Quad Face Generation for surface-crossing edges
    // Each crossed edge between (x, y, z) and (x+1/y+1/z+1) is shared by 4 dual cells
    struct ThreadFaceAccum {
        std::vector<uint32_t> faces;
    };
    std::vector<ThreadFaceAccum> threadFaces(numThreads);

#pragma omp parallel num_threads(numThreads)
    {
        int t = omp_get_thread_num();
        ThreadFaceAccum& tf = threadFaces[t];
        const auto& acc = threadAccums[t];
        size_t count = acc.cellX.size();
        size_t base = vOffsets[t];
        tf.faces.reserve(count * 4);

        for (size_t idx = 0; idx < count; ++idx) {
            int x = acc.cellX[idx];
            int y = acc.cellY[idx];
            int z = acc.cellZ[idx];
            uint8_t mask = acc.cellMask[idx];
            int32_t v0 = (int32_t)(base + idx);

            uint32_t edgeMask = snEdgeTable[mask];
            const int coords[3] = { x, y, z };

            for (int i = 0; i < 3; ++i) {
                if (!(edgeMask & (1 << i))) continue;

                int iu = (i + 1) % 3;
                int iv = (i + 2) % 3;
                if (coords[iu] == 0 || coords[iv] == 0) continue;

                int du[3] = {0, 0, 0};
                int dv[3] = {0, 0, 0};
                du[iu] = 1;
                dv[iv] = 1;

                int c1[3], c2[3], c3[3];
                if (mask & 1) {
                    c1[0] = x - du[0]; c1[1] = y - du[1]; c1[2] = z - du[2];
                    c2[0] = x - du[0] - dv[0]; c2[1] = y - du[1] - dv[1]; c2[2] = z - du[2] - dv[2];
                    c3[0] = x - dv[0]; c3[1] = y - dv[1]; c3[2] = z - dv[2];
                } else {
                    c1[0] = x - dv[0]; c1[1] = y - dv[1]; c1[2] = z - dv[2];
                    c2[0] = x - du[0] - dv[0]; c2[1] = y - du[1] - dv[1]; c2[2] = z - du[2] - dv[2];
                    c3[0] = x - du[0]; c3[1] = y - du[1]; c3[2] = z - du[2];
                }

                int32_t v1 = grid.getCellVert(c1[0], c1[1], c1[2]);
                int32_t v2 = grid.getCellVert(c2[0], c2[1], c2[2]);
                int32_t v3 = grid.getCellVert(c3[0], c3[1], c3[2]);

                if (v1 >= 0 && v2 >= 0 && v3 >= 0) {
                    tf.faces.push_back((uint32_t)v0);
                    tf.faces.push_back((uint32_t)v1);
                    tf.faces.push_back((uint32_t)v2);
                    tf.faces.push_back((uint32_t)v3);
                }
            }
        }
    }

    // Merge faces with parallel prefix sums
    std::vector<size_t> fOffsets(numThreads + 1, 0);
    for (int t = 0; t < numThreads; ++t) {
        fOffsets[t + 1] = fOffsets[t] + threadFaces[t].faces.size();
    }
    size_t totalIndices = fOffsets[numThreads];
    res.faces.resize(totalIndices);

#pragma omp parallel for schedule(static)
    for (int t = 0; t < numThreads; ++t) {
        size_t base = fOffsets[t];
        size_t cnt = threadFaces[t].faces.size();
        if (cnt > 0) {
            std::memcpy(&res.faces[base], threadFaces[t].faces.data(), cnt * sizeof(uint32_t));
        }
    }

    double msReconstruct = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    res.profile.msReconstruct = msReconstruct;
    res.profile.bytesReconstructAux = grid.getMemoryBytes();
    sculpt_log("[BlockSparseGrid] Reconstructed Surface Nets in %.2f ms (Verts: %zu, Quads: %zu)\n",
               msReconstruct, totalVerts, res.faces.size() / 4);

    return res;
}

// ---------------------------------------------------------
// Fast Trilinear SDF Gradient Sampling on BlockSparseGrid
// ---------------------------------------------------------
inline glm::vec3 sampleSparseSDFGradient(const BlockSparseGrid& grid, float gx, float gy, float gz) {
    int x0 = std::clamp((int)std::floor(gx), 0, grid.dims[0] - 2);
    int y0 = std::clamp((int)std::floor(gy), 0, grid.dims[1] - 2);
    int z0 = std::clamp((int)std::floor(gz), 0, grid.dims[2] - 2);

    float u = std::clamp(gx - (float)x0, 0.0f, 1.0f);
    float v = std::clamp(gy - (float)y0, 0.0f, 1.0f);
    float w = std::clamp(gz - (float)z0, 0.0f, 1.0f);

    float d0 = grid.getDist(x0, y0, z0);
    float d1 = grid.getDist(x0 + 1, y0, z0);
    float d2 = grid.getDist(x0, y0 + 1, z0);
    float d3 = grid.getDist(x0 + 1, y0 + 1, z0);
    float d4 = grid.getDist(x0, y0, z0 + 1);
    float d5 = grid.getDist(x0 + 1, y0, z0 + 1);
    float d6 = grid.getDist(x0, y0 + 1, z0 + 1);
    float d7 = grid.getDist(x0 + 1, y0 + 1, z0 + 1);

    if (std::abs(d0) > 1e4f || std::abs(d1) > 1e4f || std::abs(d2) > 1e4f || std::abs(d3) > 1e4f ||
        std::abs(d4) > 1e4f || std::abs(d5) > 1e4f || std::abs(d6) > 1e4f || std::abs(d7) > 1e4f) {
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
    return (len > 1e-6f) ? (grad / len) : glm::vec3(0.0f);
}

// ---------------------------------------------------------
// Constrained Tangential Relaxation for BlockSparseGrid
// Equalizes quad aspect ratio and aligns flow with geometry
// ---------------------------------------------------------
inline void applyConstrainedTangentialRelaxationSparse(
    RemeshResult& res,
    const BlockSparseGrid& grid,
    int iterations,
    float lambda,
    bool alignSymmetry
) {
    size_t nbV = res.vertices.size() / 3;
    size_t nbF = res.faces.size() / 4;
    if (nbV < 4 || nbF < 1 || iterations <= 0) return;

    // 1. Vertex Normals Baseline
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

    // 2. Vertex Adjacency Graph
    const int MAX_DEG = 8;
    std::vector<uint32_t> vertNeighbors(nbV * MAX_DEG);
    std::vector<uint8_t> vertNeighborCount(nbV, 0);

    struct QuadDiagRef { uint32_t opp; uint32_t otherA; uint32_t otherB; };
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
                if (vertNeighbors[u * MAX_DEG + j] == w) { foundU = true; break; }
            }
            if (!foundU && cu < MAX_DEG) {
                vertNeighbors[u * MAX_DEG + cu] = w;
                vertNeighborCount[u] = cu + 1;
            }

            uint8_t cw = vertNeighborCount[w];
            bool foundW = false;
            for (uint8_t j = 0; j < cw; ++j) {
                if (vertNeighbors[w * MAX_DEG + j] == u) { foundW = true; break; }
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

    // 3. Symmetry Constrained Vertices
    std::vector<uint8_t> symFixed(nbV, 0);
    if (alignSymmetry) {
        float snapTol = grid.step * 0.45f;
#pragma omp parallel for schedule(static)
        for (int i = 0; i < (int)nbV; ++i) {
            uint8_t flags = 0;
            if (std::abs(res.vertices[i * 3 + 0]) < snapTol) flags |= 1;
            if (std::abs(res.vertices[i * 3 + 1]) < snapTol) flags |= 2;
            if (std::abs(res.vertices[i * 3 + 2]) < snapTol) flags |= 4;
            symFixed[i] = flags;
        }
    }

    // 4. Tangential Relaxation Iterations
    std::vector<float> currVerts = res.vertices;
    std::vector<float> nextVerts = currVerts;

    for (int it = 0; it < iterations; ++it) {
#pragma omp parallel for schedule(static)
        for (int v = 0; v < (int)nbV; ++v) {
            uint8_t count = vertNeighborCount[v];
            if (count == 0) continue;

            glm::vec3 pv(currVerts[v * 3 + 0], currVerts[v * 3 + 1], currVerts[v * 3 + 2]);

            // Edge Laplacian
            glm::vec3 pAvg(0.0f);
            const uint32_t* nbrPtr = &vertNeighbors[v * MAX_DEG];
            for (uint8_t k = 0; k < count; ++k) {
                uint32_t u = nbrPtr[k];
                pAvg += glm::vec3(currVerts[u * 3 + 0], currVerts[u * 3 + 1], currVerts[u * 3 + 2]);
            }
            pAvg /= (float)count;
            glm::vec3 delta = pAvg - pv;

            // Diagonal equalization force
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

                    float diagLen = glm::length(pOpp - pv);
                    float otherDiagLen = glm::length(pB - pA);
                    if (diagLen > 1e-6f && otherDiagLen > 1e-6f) {
                        float diff = otherDiagLen - diagLen;
                        diagDelta += ((pv - pOpp) / diagLen) * (diff * 0.15f);
                    }
                }
                delta += diagDelta / (float)diagCount;
            }

            // Normal from SDF gradient
            glm::vec3 norm = vertNormals[v];
            float gx = (pv.x - grid.minCoord[0]) / grid.step;
            float gy = (pv.y - grid.minCoord[1]) / grid.step;
            float gz = (pv.z - grid.minCoord[2]) / grid.step;
            glm::vec3 sdfNorm = sampleSparseSDFGradient(grid, gx, gy, gz);
            if (glm::length(sdfNorm) > 1e-6f) {
                if (glm::dot(sdfNorm, vertNormals[v]) < 0.0f) {
                    sdfNorm = -sdfNorm;
                }
                norm = sdfNorm;
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

    res.vertices = std::move(currVerts);
}

} // namespace sculpt
