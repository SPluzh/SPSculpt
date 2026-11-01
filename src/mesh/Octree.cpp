#include "mesh/Octree.h"
#include "common/Logger.h"
#include <cstring>
#include <chrono>
#include <iostream>
#include <omp.h>
#include <atomic>

static inline void initCell(OctreeCell* cell, OctreeCell* parent, int depth) {
    cell->parent = parent;
    cell->depth = depth;
    for (int i = 0; i < 8; ++i) cell->children[i] = nullptr;
    cell->iFaces.clear();
    cell->dirty = false;
    cell->aabbLoose[0] = std::numeric_limits<float>::infinity();
    cell->aabbLoose[1] = std::numeric_limits<float>::infinity();
    cell->aabbLoose[2] = std::numeric_limits<float>::infinity();
    cell->aabbSplit[0] = std::numeric_limits<float>::infinity();
    cell->aabbSplit[1] = std::numeric_limits<float>::infinity();
    cell->aabbSplit[2] = std::numeric_limits<float>::infinity();
    cell->aabbLoose[3] = -std::numeric_limits<float>::infinity();
    cell->aabbLoose[4] = -std::numeric_limits<float>::infinity();
    cell->aabbLoose[5] = -std::numeric_limits<float>::infinity();
    cell->aabbSplit[3] = -std::numeric_limits<float>::infinity();
    cell->aabbSplit[4] = -std::numeric_limits<float>::infinity();
    cell->aabbSplit[5] = -std::numeric_limits<float>::infinity();
}

static inline void setChildSplitBounds(OctreeCell* cell, float xmin, float ymin, float zmin, float xmax, float ymax, float zmax, float xcen, float ycen, float zcen) {
    cell->children[0]->setAabbSplit(xmin, ymin, zmin, xcen, ycen, zcen);
    cell->children[1]->setAabbSplit(xcen, ymin, zmin, xmax, ycen, zcen);
    cell->children[2]->setAabbSplit(xcen, ymin, zcen, xmax, ycen, zmax);
    cell->children[3]->setAabbSplit(xmin, ymin, zcen, xcen, ycen, zmax);
    cell->children[4]->setAabbSplit(xmin, ycen, zmin, xcen, ymax, zcen);
    cell->children[5]->setAabbSplit(xcen, ycen, zmin, xmax, ymax, zcen);
    cell->children[6]->setAabbSplit(xcen, ycen, zcen, xmax, ymax, zmax);
    cell->children[7]->setAabbSplit(xmin, ycen, zcen, xcen, ymax, zmax);
}

inline int getChildIndex(float cx, float cy, float cz, float xcen, float ycen, float zcen) {
    static const int lut[8] = {0, 3, 4, 7, 1, 2, 5, 6};
    int key = ((cx > xcen) << 2) | ((cy > ycen) << 1) | (cz > zcen);
    return lut[key];
}

OctreeCell* Octree::getCell() {
    if (poolIndex < (int)cellPool.size()) {
        OctreeCell* cell = cellPool[poolIndex++];
        initCell(cell, nullptr, 0);
        return cell;
    } else {
        OctreeCell* cell = new OctreeCell();
        cellPool.push_back(cell);
        poolIndex++;
        return cell;
    }
}

void Octree::allocateChildren(OctreeCell* cell, std::atomic<int>& poolIndexAtomic) {
    int base = poolIndexAtomic.fetch_add(8, std::memory_order_relaxed);
    if (base + 8 > (int)cellPool.size()) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        while ((int)cellPool.size() < base + 8 + 1024) {
            cellPool.push_back(new OctreeCell());
        }
    }
    for (int i = 0; i < 8; ++i) {
        OctreeCell* child = cellPool[base + i];
        initCell(child, cell, cell->depth + 1);
        cell->children[i] = child;
    }
}

void Octree::splitCellSequential(OctreeCell* cell, std::atomic<int>& poolIndexAtomic) {
    float* split = cell->aabbSplit;
    float xmin = split[0];
    float ymin = split[1];
    float zmin = split[2];
    float xmax = split[3];
    float ymax = split[4];
    float zmax = split[5];
    float xcen = (xmax + xmin) * 0.5f;
    float ycen = (ymax + ymin) * 0.5f;
    float zcen = (zmax + zmin) * 0.5f;

    allocateChildren(cell, poolIndexAtomic);
    setChildSplitBounds(cell, xmin, ymin, zmin, xmax, ymax, zmax, xcen, ycen, zcen);

    int nbFacesLocal = (int)cell->iFaces.size();

    int counts[8] = {0};
    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t id = cell->iFaces[i] * 3;
        counts[getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen)]++;
    }

    for (int i = 0; i < 8; ++i) {
        cell->children[i]->iFaces.reserve(counts[i]);
    }

    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t iFace = cell->iFaces[i];
        uint32_t id = iFace * 3;
        int idx = getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen);
        cell->children[idx]->iFaces.push_back(iFace);
    }

    cell->iFaces.clear();
}

static void updateAabbLooseRecursive(OctreeCell* cell) {
    if (!cell || cell->children[0] == nullptr) return;

    float bxmin = std::numeric_limits<float>::infinity();
    float bymin = std::numeric_limits<float>::infinity();
    float bzmin = std::numeric_limits<float>::infinity();
    float bxmax = -std::numeric_limits<float>::infinity();
    float bymax = -std::numeric_limits<float>::infinity();
    float bzmax = -std::numeric_limits<float>::infinity();
    bool hasValidChild = false;

    for (int i = 0; i < 8; ++i) {
        OctreeCell* child = cell->children[i];
        if (child) {
            updateAabbLooseRecursive(child);
            float* cb = child->aabbLoose;
            if (cb[0] <= cb[3]) {
                hasValidChild = true;
                if (cb[0] < bxmin) bxmin = cb[0];
                if (cb[1] < bymin) bymin = cb[1];
                if (cb[2] < bzmin) bzmin = cb[2];
                if (cb[3] > bxmax) bxmax = cb[3];
                if (cb[4] > bymax) bymax = cb[4];
                if (cb[5] > bzmax) bzmax = cb[5];
            }
        }
    }
    if (hasValidChild) {
        cell->setAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    }
}

void Octree::build(int nbVertsVal, int nbFacesVal,
                   const float* centersPtrVal, const float* boxesPtrVal,
                   const float* vertsPtrVal, const uint32_t* facesPtrVal) {
    nbVerts = nbVertsVal;
    nbFaces = nbFacesVal;

    vertsData = vertsPtrVal;
    facesData = facesPtrVal;
    faceCentersData = centersPtrVal;
    faceBoxesData = boxesPtrVal;

    if (nbFaces <= 0 || nbVerts <= 0 || !vertsPtrVal || !facesPtrVal) {
        root = nullptr;
        faceLeaf.clear();
        facePosInLeaf.clear();
        return;
    }

    if (m_visitedFlags.size() < (size_t)nbVerts) {
        m_visitedFlags.assign(nbVerts, 0);
    }

    rebuildInternal();
}

void Octree::rebuildInternal() {
    poolIndex = 0;
    if (nbFaces <= 0 || nbVerts <= 0 || !vertsData || !facesData) {
        root = nullptr;
        faceLeaf.clear();
        facePosInLeaf.clear();
        return;
    }

    maxFaces = 100;
    if (nbFaces > 1000000) {
        maxFaces = nbFaces / 1000;
    } else if (nbFaces > 100000) {
        maxFaces = nbFaces / 500;
    }
    maxFaces = std::max(100, std::min(10000, maxFaces));

    float xmin = std::numeric_limits<float>::infinity();
    float ymin = std::numeric_limits<float>::infinity();
    float zmin = std::numeric_limits<float>::infinity();
    float xmax = -std::numeric_limits<float>::infinity();
    float ymax = -std::numeric_limits<float>::infinity();
    float zmax = -std::numeric_limits<float>::infinity();

#pragma omp parallel for reduction(min:xmin,ymin,zmin) reduction(max:xmax,ymax,zmax) if(nbVerts > 5000)
    for (int i = 0; i < nbVerts; ++i) {
        float vx = vertsData[i * 3];
        float vy = vertsData[i * 3 + 1];
        float vz = vertsData[i * 3 + 2];
        if (vx < xmin) xmin = vx;
        if (vx > xmax) xmax = vx;
        if (vy < ymin) ymin = vy;
        if (vy > ymax) ymax = vy;
        if (vz < zmin) zmin = vz;
        if (vz > zmax) zmax = vz;
    }

    float dx = xmax - xmin;
    float dy = ymax - ymin;
    float dz = zmax - zmin;

    float offset = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.2f;
    float eps = 1e-16f;
    if (xmax - xmin < eps) { xmin -= offset; xmax += offset; }
    if (ymax - ymin < eps) { ymin -= offset; ymax += offset; }
    if (zmax - zmin < eps) { zmin -= offset; zmax += offset; }

    float dfx = dx * 0.3f;
    float dfy = dy * 0.3f;
    float dfz = dz * 0.3f;
    float xmin2 = xmin - dfx;
    float xmax2 = xmax + dfx;
    float ymin2 = ymin - dfy;
    float ymax2 = ymax + dfy;
    float zmin2 = zmin - dfz;
    float zmax2 = zmax + dfz;

    if (cellPool.size() < 65536) {
        cellPool.reserve(65536);
        while (cellPool.size() < 65536) {
            cellPool.push_back(new OctreeCell());
        }
    }
    std::atomic<int> poolIndexAtomic(0);

    root = cellPool[poolIndexAtomic.fetch_add(1, std::memory_order_relaxed)];
    initCell(root, nullptr, 0);
    root->setAabbLoose(xmin, ymin, zmin, xmax, ymax, zmax);
    root->setAabbSplit(xmin2, ymin2, zmin2, xmax2, ymax2, zmax2);

    if ((int)faceLeaf.size() != nbFaces) {
        faceLeaf.resize(nbFaces);
    }
    if ((int)facePosInLeaf.size() != nbFaces) {
        facePosInLeaf.resize(nbFaces);
    }
#pragma omp parallel for if(nbFaces > 50000) schedule(static)
    for (int i = 0; i < nbFaces; ++i) {
        faceLeaf[i] = nullptr;
        facePosInLeaf[i] = 0;
    }

    const int MAX_DEPTH = 8;
    if (nbFaces <= maxFaces) {
        root->iFaces.resize(nbFaces);
        for (int i = 0; i < nbFaces; ++i) {
            root->iFaces[i] = i;
        }
        constructLeaf(root);
        poolIndex = poolIndexAtomic.load(std::memory_order_relaxed);
        return;
    }

    float* split = root->aabbSplit;
    float s_xmin = split[0], s_ymin = split[1], s_zmin = split[2];
    float s_xmax = split[3], s_ymax = split[4], s_zmax = split[5];
    float xcen = (s_xmax + s_xmin) * 0.5f;
    float ycen = (s_ymax + s_ymin) * 0.5f;
    float zcen = (s_zmax + s_zmin) * 0.5f;

    allocateChildren(root, poolIndexAtomic);
    setChildSplitBounds(root, s_xmin, s_ymin, s_zmin, s_xmax, s_ymax, s_zmax, xcen, ycen, zcen);

    int numThreads = omp_get_max_threads();
    std::vector<std::vector<int>> threadCounts(numThreads, std::vector<int>(8, 0));
    std::vector<std::vector<int>> threadOffsets(numThreads, std::vector<int>(8, 0));
    std::vector<int> totalCounts(8, 0);

#pragma omp parallel num_threads(numThreads)
    {
        int tid = omp_get_thread_num();
        int chunkSize = (nbFaces + numThreads - 1) / numThreads;
        int start = tid * chunkSize;
        int end = std::min(start + chunkSize, nbFaces);

        int localCounts[8] = {0};
        for (int i = start; i < end; ++i) {
            uint32_t id = i * 3;
            int idx = getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen);
            localCounts[idx]++;
        }
        for (int k = 0; k < 8; ++k) {
            threadCounts[tid][k] = localCounts[k];
        }
    }

    for (int k = 0; k < 8; ++k) {
        int sum = 0;
        for (int t = 0; t < numThreads; ++t) {
            threadOffsets[t][k] = sum;
            sum += threadCounts[t][k];
        }
        totalCounts[k] = sum;
        root->children[k]->iFaces.resize(sum);
    }

#pragma omp parallel num_threads(numThreads)
    {
        int tid = omp_get_thread_num();
        int chunkSize = (nbFaces + numThreads - 1) / numThreads;
        int start = tid * chunkSize;
        int end = std::min(start + chunkSize, nbFaces);

        int localWritePos[8];
        for (int k = 0; k < 8; ++k) {
            localWritePos[k] = threadOffsets[tid][k];
        }

        for (int i = start; i < end; ++i) {
            uint32_t id = i * 3;
            int idx = getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen);
            root->children[idx]->iFaces[localWritePos[idx]++] = i;
        }
    }

#pragma omp parallel for schedule(dynamic, 1)
    for (int k = 0; k < 8; ++k) {
        OctreeCell* child = root->children[k];
        if ((int)child->iFaces.size() > maxFaces && child->depth < MAX_DEPTH) {
            splitCellSequential(child, poolIndexAtomic);
        }
    }

    std::vector<OctreeCell*> level2Nodes;
    std::vector<OctreeCell*> leaves;
    leaves.reserve(8192);

    for (int k = 0; k < 8; ++k) {
        OctreeCell* child = root->children[k];
        if (child->children[0] != nullptr) {
            for (int g = 0; g < 8; ++g) {
                OctreeCell* grandchild = child->children[g];
                if (!grandchild->iFaces.empty()) {
                    level2Nodes.push_back(grandchild);
                }
            }
        } else if (!child->iFaces.empty()) {
            leaves.push_back(child);
        }
    }

#pragma omp parallel
    {
        std::vector<OctreeCell*> threadLeaves;
        std::vector<OctreeCell*> threadStack;
        threadLeaves.reserve(1024);
        threadStack.reserve(64);

#pragma omp for schedule(dynamic, 1)
        for (size_t i = 0; i < level2Nodes.size(); ++i) {
            OctreeCell* subtreeNode = level2Nodes[i];
            threadStack.push_back(subtreeNode);

            while (!threadStack.empty()) {
                OctreeCell* cell = threadStack.back();
                threadStack.pop_back();

                int cellNbFaces = (int)cell->iFaces.size();
                if (cellNbFaces > maxFaces && cell->depth < MAX_DEPTH) {
                    splitCellSequential(cell, poolIndexAtomic);
                    for (int c = 0; c < 8; ++c) {
                        if (!cell->children[c]->iFaces.empty()) {
                            threadStack.push_back(cell->children[c]);
                        }
                    }
                } else if (cellNbFaces > 0) {
                    threadLeaves.push_back(cell);
                }
            }
        }

#pragma omp critical
        {
            leaves.insert(leaves.end(), threadLeaves.begin(), threadLeaves.end());
        }
    }

#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < leaves.size(); ++i) {
        OctreeCell* cell = leaves[i];
        int nbFacesLocal = (int)cell->iFaces.size();
        float bxmin = std::numeric_limits<float>::infinity();
        float bymin = std::numeric_limits<float>::infinity();
        float bzmin = std::numeric_limits<float>::infinity();
        float bxmax = -std::numeric_limits<float>::infinity();
        float bymax = -std::numeric_limits<float>::infinity();
        float bzmax = -std::numeric_limits<float>::infinity();

        for (int j = 0; j < nbFacesLocal; ++j) {
            uint32_t id = cell->iFaces[j];
            faceLeaf[id] = cell;
            facePosInLeaf[id] = j;

            uint32_t id6 = id * 6;
            float f_xmin = faceBoxesData[id6];
            float f_ymin = faceBoxesData[id6 + 1];
            float f_zmin = faceBoxesData[id6 + 2];
            float f_xmax = faceBoxesData[id6 + 3];
            float f_ymax = faceBoxesData[id6 + 4];
            float f_zmax = faceBoxesData[id6 + 5];
            if (f_xmin < bxmin) bxmin = f_xmin;
            if (f_xmax > bxmax) bxmax = f_xmax;
            if (f_ymin < bymin) bymin = f_ymin;
            if (f_ymax > bymax) bymax = f_ymax;
            if (f_zmin < bzmin) bzmin = f_zmin;
            if (f_zmax > bzmax) bzmax = f_zmax;
        }
        cell->setAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    }

#pragma omp parallel for schedule(dynamic, 1)
    for (int k = 0; k < 8; ++k) {
        if (root->children[k]) {
            updateAabbLooseRecursive(root->children[k]);
        }
    }

    float bxmin = root->aabbLoose[0];
    float bymin = root->aabbLoose[1];
    float bzmin = root->aabbLoose[2];
    float bxmax = root->aabbLoose[3];
    float bymax = root->aabbLoose[4];
    float bzmax = root->aabbLoose[5];
    for (int k = 0; k < 8; ++k) {
        OctreeCell* child = root->children[k];
        if (child && child->aabbLoose[0] <= child->aabbLoose[3]) {
            if (child->aabbLoose[0] < bxmin) bxmin = child->aabbLoose[0];
            if (child->aabbLoose[1] < bymin) bymin = child->aabbLoose[1];
            if (child->aabbLoose[2] < bzmin) bzmin = child->aabbLoose[2];
            if (child->aabbLoose[3] > bxmax) bxmax = child->aabbLoose[3];
            if (child->aabbLoose[4] > bymax) bymax = child->aabbLoose[4];
            if (child->aabbLoose[5] > bzmax) bzmax = child->aabbLoose[5];
        }
    }
    root->setAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);

    poolIndex = poolIndexAtomic.load(std::memory_order_relaxed);
}

void Octree::constructChildren(OctreeCell* cell) {
    float* split = cell->aabbSplit;
    float xmin = split[0];
    float ymin = split[1];
    float zmin = split[2];
    float xmax = split[3];
    float ymax = split[4];
    float zmax = split[5];
    float xcen = (xmax + xmin) * 0.5f;
    float ycen = (ymax + ymin) * 0.5f;
    float zcen = (zmax + zmin) * 0.5f;

    for (int i = 0; i < 8; ++i) {
        cell->children[i] = getCell();
        cell->children[i]->parent = cell;
        cell->children[i]->depth = cell->depth + 1;
    }
    setChildSplitBounds(cell, xmin, ymin, zmin, xmax, ymax, zmax, xcen, ycen, zcen);

    int nbFacesLocal = cell->iFaces.size();

    int counts[8] = {0};
    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t id = cell->iFaces[i] * 3;
        counts[getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen)]++;
    }

    for (int i = 0; i < 8; ++i) {
        cell->children[i]->iFaces.reserve(counts[i]);
    }

    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t iFace = cell->iFaces[i];
        uint32_t id = iFace * 3;
        int idx = getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen);
        cell->children[idx]->iFaces.push_back(iFace);
    }

    cell->iFaces.clear();
}

void Octree::constructLeaf(OctreeCell* cell) {
    int nbFacesLocal = cell->iFaces.size();
    float bxmin = std::numeric_limits<float>::infinity();
    float bymin = std::numeric_limits<float>::infinity();
    float bzmin = std::numeric_limits<float>::infinity();
    float bxmax = -std::numeric_limits<float>::infinity();
    float bymax = -std::numeric_limits<float>::infinity();
    float bzmax = -std::numeric_limits<float>::infinity();

    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t id = cell->iFaces[i];
        faceLeaf[id] = cell;
        facePosInLeaf[id] = i;

        uint32_t id6 = id * 6;
        float xmin = faceBoxesData[id6];
        float ymin = faceBoxesData[id6 + 1];
        float zmin = faceBoxesData[id6 + 2];
        float xmax = faceBoxesData[id6 + 3];
        float ymax = faceBoxesData[id6 + 4];
        float zmax = faceBoxesData[id6 + 5];
        if (xmin < bxmin) bxmin = xmin;
        if (xmax > bxmax) bxmax = xmax;
        if (ymin < bymin) bymin = ymin;
        if (ymax > bymax) bymax = ymax;
        if (zmin < bzmin) bzmin = zmin;
        if (zmax > bzmax) bzmax = zmax;
    }
    cell->expandsAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
}

void Octree::collectIntersectSphere(float vx, float vy, float vz, float radiusSquared, std::vector<uint32_t>& iFacesInCells) {
    iFacesInCells.clear();
    if (!root) return;

    if (iFacesInCells.capacity() < 262144) {
        iFacesInCells.reserve(524288);
    }

    OctreeCell* stack[256];
    int stackPtr = 0;
    stack[stackPtr++] = root;

    while (stackPtr > 0) {
        OctreeCell* cell = stack[--stackPtr];

        float* loose = cell->aabbLoose;
        float dx = 0.0f;
        if (loose[0] > vx) dx = loose[0] - vx;
        else if (loose[3] < vx) dx = loose[3] - vx;
        float dx2 = dx * dx;
        if (dx2 > radiusSquared) continue;

        float dy = 0.0f;
        if (loose[1] > vy) dy = loose[1] - vy;
        else if (loose[4] < vy) dy = loose[4] - vy;
        float dy2 = dy * dy;
        if (dx2 + dy2 > radiusSquared) continue;

        float dz = 0.0f;
        if (loose[2] > vz) dz = loose[2] - vz;
        else if (loose[5] < vz) dz = loose[5] - vz;
        if (dx2 + dy2 + dz * dz > radiusSquared)
            continue;

        if (cell->children[0] != nullptr) {
            for (int i = 0; i < 8; ++i) {
                if (stackPtr < 256) {
                    stack[stackPtr++] = cell->children[i];
                }
            }
        } else {
            iFacesInCells.insert(iFacesInCells.end(), cell->iFaces.begin(), cell->iFaces.end());
        }
    }
}

std::vector<uint32_t> Octree::collectIntersectSphere(float vx, float vy, float vz, float radiusSquared) {
    std::vector<uint32_t> iFacesInCells;
    collectIntersectSphere(vx, vy, vz, radiusSquared, iFacesInCells);
    return iFacesInCells;
}

void Octree::pickVerticesInSphere(
    float cx, float cy, float cz, float radius2, const uint8_t* vertVisible,
    std::vector<uint32_t>& pickedVertices) {
    
    pickedVertices.clear();
    if (!root) {
        lastCollectCellsMs = lastCandidateVertsMs = lastFilterVertsMs = 0.0;
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    collectIntersectSphere(cx, cy, cz, radius2, m_tempFaces);
    auto t1 = std::chrono::high_resolution_clock::now();
    lastCollectCellsMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (m_visitedFlags.size() < (size_t)nbVerts) {
        m_visitedFlags.assign(nbVerts, 0);
    }
    m_visitedEpoch++;
    if (m_visitedEpoch == 0) {
        std::fill(m_visitedFlags.begin(), m_visitedFlags.end(), 0);
        m_visitedEpoch = 1;
    }

    const size_t numFaces = m_tempFaces.size();
    if (numFaces == 0) {
        lastCandidateVertsMs = 0.0;
        lastFilterVertsMs = 0.0;
        return;
    }

    const uint32_t curEpoch = m_visitedEpoch;
    uint32_t* const vFlags = m_visitedFlags.data();
    const uint32_t limitVerts = (uint32_t)nbVerts;
    const uint32_t limitFaces = (uint32_t)nbFaces;

    lastCandidateVertsMs = 0.0;

    if (numFaces < 2048) {
        pickedVertices.reserve(numFaces);
        for (uint32_t iFace : m_tempFaces) {
            if (iFace >= limitFaces) continue;
            const uint32_t* fv = facesData + (iFace << 2);
            for (int k = 0; k < 4; ++k) {
                uint32_t v = fv[k];
                if (v < limitVerts && vFlags[v] != curEpoch) {
                    vFlags[v] = curEpoch;
                    if (vertVisible && vertVisible[v] == 0) continue;
                    uint32_t j = v * 3;
                    float dx = cx - vertsData[j];
                    float dx2 = dx * dx;
                    if (dx2 >= radius2) continue;
                    float dy = cy - vertsData[j + 1];
                    float dy2 = dy * dy;
                    if (dx2 + dy2 >= radius2) continue;
                    float dz = cz - vertsData[j + 2];
                    if (dx2 + dy2 + dz * dz < radius2) {
                        pickedVertices.push_back(v);
                    }
                }
            }
        }
    } else {
        int maxThreads = omp_get_max_threads();
        if ((int)m_threadPicked.size() < maxThreads) {
            m_threadPicked.resize(maxThreads);
        }

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& localPicked = m_threadPicked[tid];
            localPicked.clear();
            size_t estCount = (numFaces / maxThreads) + 64;
            if (localPicked.capacity() < estCount) {
                localPicked.reserve(estCount);
            }

            #pragma omp for schedule(static)
            for (int64_t i = 0; i < (int64_t)numFaces; ++i) {
                uint32_t iFace = m_tempFaces[i];
                if (iFace >= limitFaces) continue;
                const uint32_t* fv = facesData + (iFace << 2);
                for (int k = 0; k < 4; ++k) {
                    uint32_t v = fv[k];
                    if (v >= limitVerts) continue;
                    if (vFlags[v] == curEpoch) continue;

                    std::atomic_ref<uint32_t> ref(vFlags[v]);
                    if (ref.exchange(curEpoch, std::memory_order_relaxed) != curEpoch) {
                        if (vertVisible && vertVisible[v] == 0) continue;
                        uint32_t j = v * 3;
                        float dx = cx - vertsData[j];
                        float dx2 = dx * dx;
                        if (dx2 >= radius2) continue;
                        float dy = cy - vertsData[j + 1];
                        float dy2 = dy * dy;
                        if (dx2 + dy2 >= radius2) continue;
                        float dz = cz - vertsData[j + 2];
                        if (dx2 + dy2 + dz * dz < radius2) {
                            localPicked.push_back(v);
                        }
                    }
                }
            }
        }

        size_t totalFound = 0;
        for (int t = 0; t < maxThreads; ++t) {
            totalFound += m_threadPicked[t].size();
        }
        pickedVertices.resize(totalFound);
        size_t offset = 0;
        for (int t = 0; t < maxThreads; ++t) {
            size_t count = m_threadPicked[t].size();
            if (count > 0) {
                std::memcpy(pickedVertices.data() + offset, m_threadPicked[t].data(), count * sizeof(uint32_t));
                offset += count;
            }
        }
    }

    auto t2 = std::chrono::high_resolution_clock::now();
    lastFilterVertsMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
}

std::vector<uint32_t> Octree::pickVerticesInSphere(
    float cx, float cy, float cz, float radius2, const uint8_t* vertVisible) {
    
    std::vector<uint32_t> pickedVertices;
    pickVerticesInSphere(cx, cy, cz, radius2, vertVisible, pickedVertices);
    return pickedVertices;
}

void Octree::update(const float* vertsPtrVal, int nbVertsVal,
                    const uint32_t* facesPtrVal, int nbFacesVal,
                    const float* boxesPtrVal, const uint32_t* iFacesPtr, int nbIFacesVal) {
    auto tStart = std::chrono::high_resolution_clock::now();

    if (!root) {
        build(nbVertsVal, nbFacesVal, faceCentersData, boxesPtrVal, vertsPtrVal, facesPtrVal);
        return;
    }

    nbVerts = nbVertsVal;
    nbFaces = nbFacesVal;

    vertsData = vertsPtrVal;
    facesData = facesPtrVal;
    faceBoxesData = boxesPtrVal;

    if (faceLeaf.size() != (size_t)nbFaces) {
        faceLeaf.resize(nbFaces, nullptr);
    }
    if (facePosInLeaf.size() != (size_t)nbFaces) {
        facePosInLeaf.resize(nbFaces, 0);
    }

    if (iFacesPtr == nullptr || nbIFacesVal < 0 || nbIFacesVal > nbFaces * 0.60f) {
        rebuildInternal();
        return;
    }

    if (nbIFacesVal == 0) {
        return;
    }

    auto tSub = std::chrono::high_resolution_clock::now();

    float* rootSplit = root->aabbSplit;
    float xmin = rootSplit[0];
    float ymin = rootSplit[1];
    float zmin = rootSplit[2];
    float xmax = rootSplit[3];
    float ymax = rootSplit[4];
    float zmax = rootSplit[5];

    bool needRebuild = false;
    for (int i = 0; i < nbIFacesVal; ++i) {
        uint32_t idFace = iFacesPtr[i];
        if (idFace >= (uint32_t)nbFaces) continue;
        uint32_t idb = idFace * 6;
        float ibux = faceBoxesData[idb];
        float ibuy = faceBoxesData[idb + 1];
        float ibuz = faceBoxesData[idb + 2];
        float iblx = faceBoxesData[idb + 3];
        float ibly = faceBoxesData[idb + 4];
        float iblz = faceBoxesData[idb + 5];

        if (ibux > xmax || iblx < xmin || ibuy > ymax || ibly < ymin || ibuz > zmax || iblz < zmin) {
            needRebuild = true;
            break;
        }
    }

    double checkMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    if (needRebuild) {
        rebuildInternal();
        return;
    }

    tSub = std::chrono::high_resolution_clock::now();
    std::vector<std::pair<uint32_t, OctreeCell*>> facesToMove;
    facesToMove.reserve(std::max(0, nbIFacesVal));

    bool overMovedLimit = false;
    size_t movedThreshold = std::max((size_t)100000, (size_t)(nbFaces * 0.25f));

    for (int i = 0; i < nbIFacesVal; ++i) {
        uint32_t idFace = iFacesPtr[i];
        if (idFace >= (uint32_t)nbFaces) continue;
        uint32_t idb = idFace * 6;
        uint32_t idCen = idFace * 3;
        OctreeCell* leaf = faceLeaf[idFace];
        if (!leaf) {
            facesToMove.push_back({idFace, nullptr});
            if (facesToMove.size() > movedThreshold) {
                overMovedLimit = true;
                break;
            }
            continue;
        }
        
        float* ab = leaf->aabbSplit;
        float vx = faceCentersData[idCen];
        float vy = faceCentersData[idCen + 1];
        float vz = faceCentersData[idCen + 2];

        bool outsideSplit = (vx <= ab[0] || vy <= ab[1] || vz <= ab[2] || vx > ab[3] || vy > ab[4] || vz > ab[5]);
        bool outsideLoose = false;
        if (outsideSplit) {
            float* loose = leaf->aabbLoose;
            outsideLoose = (faceBoxesData[idb] < loose[0] || faceBoxesData[idb + 1] < loose[1] || faceBoxesData[idb + 2] < loose[2]
                         || faceBoxesData[idb + 3] > loose[3] || faceBoxesData[idb + 4] > loose[4] || faceBoxesData[idb + 5] > loose[5]);
        }

        if (outsideSplit && outsideLoose) {
            facesToMove.push_back({idFace, leaf});
            if (facesToMove.size() > movedThreshold) {
                overMovedLimit = true;
                break;
            }
        }
    }

    double findMovedMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    if (overMovedLimit) {
        rebuildInternal();
        return;
    }

    tSub = std::chrono::high_resolution_clock::now();
    std::vector<OctreeCell*> leavesToUpdate;

    for (const auto& item : facesToMove) {
        uint32_t idFace = item.first;
        OctreeCell* leaf = item.second;
        if (!leaf) continue;

        auto& facesInLeaf = leaf->iFaces;
        int iPos = facePosInLeaf[idFace];
        if (iPos >= 0 && (size_t)iPos < facesInLeaf.size()) {
            uint32_t iFaceLast = facesInLeaf.back();
            facesInLeaf[iPos] = iFaceLast;
            if (iFaceLast < facePosInLeaf.size()) {
                facePosInLeaf[iFaceLast] = iPos;
            }
            facesInLeaf.pop_back();
        }
        faceLeaf[idFace] = nullptr;
        if (!leaf->dirty) {
            leaf->dirty = true;
            leavesToUpdate.push_back(leaf);
        }
    }
    double removeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    tSub = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < nbIFacesVal; ++i) {
        uint32_t idFace = iFacesPtr[i];
        if (idFace >= (uint32_t)nbFaces) continue;
        OctreeCell* leaf = faceLeaf[idFace];
        if (!leaf) continue;

        uint32_t idb = idFace * 6;
        float fx0 = faceBoxesData[idb];
        float fy0 = faceBoxesData[idb + 1];
        float fz0 = faceBoxesData[idb + 2];
        float fx1 = faceBoxesData[idb + 3];
        float fy1 = faceBoxesData[idb + 4];
        float fz1 = faceBoxesData[idb + 5];

        float* loose = leaf->aabbLoose;
        if (fx0 >= loose[0] && fy0 >= loose[1] && fz0 >= loose[2] &&
            fx1 <= loose[3] && fy1 <= loose[4] && fz1 <= loose[5]) {
            continue;
        }

        leaf->expandsAabbLoose(fx0, fy0, fz0, fx1, fy1, fz1);
    }
    double expandMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    tSub = std::chrono::high_resolution_clock::now();
    for (const auto& item : facesToMove) {
        uint32_t idFace = item.first;
        OctreeCell* oldLeaf = item.second;
        if (idFace >= (uint32_t)nbFaces) continue;
        uint32_t idb = idFace * 6;
        uint32_t idc = idFace * 3;
        OctreeCell* newleaf = addFaceLocal(oldLeaf, idFace, faceBoxesData[idb], faceBoxesData[idb + 1], faceBoxesData[idb + 2],
                                           faceBoxesData[idb + 3], faceBoxesData[idb + 4], faceBoxesData[idb + 5],
                                           faceCentersData[idc], faceCentersData[idc + 1], faceCentersData[idc + 2]);
        if (newleaf) {
            facePosInLeaf[idFace] = newleaf->iFaces.size() - 1;
            faceLeaf[idFace] = newleaf;
            if (!newleaf->dirty) {
                newleaf->dirty = true;
                leavesToUpdate.push_back(newleaf);
            }
        } else {
            if (oldLeaf) {
                facePosInLeaf[idFace] = oldLeaf->iFaces.size();
                oldLeaf->iFaces.push_back(idFace);
                faceLeaf[idFace] = oldLeaf;
                if (!oldLeaf->dirty) {
                    oldLeaf->dirty = true;
                    leavesToUpdate.push_back(oldLeaf);
                }
            }
        }
    }
    double reinsertMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    tSub = std::chrono::high_resolution_clock::now();
    balanceOctree(leavesToUpdate);
    double balanceMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSub).count();

    double totalUpdateMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    if (totalUpdateMs > 3.0) {
        sculpt_log("     (Octree Detail: Total=%.2fms | Check=%.2fms | FindMoved=%.2fms [%zu moved] | Remove=%.2fms | Expand=%.2fms | Reinsert=%.2fms | Balance=%.2fms)\n",
                  totalUpdateMs, checkMs, findMovedMs, facesToMove.size(), removeMs, expandMs, reinsertMs, balanceMs);
    }
}

void Octree::pruneIfPossible(OctreeCell* cell, std::unordered_set<OctreeCell*>& deletedCells) {
    OctreeCell* curr = cell;
    while (curr->parent) {
        OctreeCell* parent = curr->parent;
        if (parent->children[0] == nullptr)
            return;

        for (int i = 0; i < 8; ++i) {
            OctreeCell* child = parent->children[i];
            if (!child->iFaces.empty() || child->children[0] != nullptr) {
                return;
            }
        }

        for (int i = 0; i < 8; ++i) {
            deletedCells.insert(parent->children[i]);
            parent->children[i] = nullptr;
        }
        curr = parent;
    }
}

void Octree::balanceOctree(std::vector<OctreeCell*>& leavesToUpdate) {
    for (OctreeCell* leaf : leavesToUpdate) {
        leaf->dirty = false;
    }

    std::unordered_set<OctreeCell*> deletedCells;

    const int MAX_DEPTH = 8;

    for (OctreeCell* leaf : leavesToUpdate) {
        if (deletedCells.count(leaf)) {
            continue;
        }
        if (leaf->iFaces.empty()) {
            pruneIfPossible(leaf, deletedCells);
        } else if (leaf->iFaces.size() > (size_t)maxFaces && leaf->depth < MAX_DEPTH) {
            constructChildren(leaf);
            for (int i = 0; i < 8; ++i) {
                if (!leaf->children[i]->iFaces.empty()) {
                    constructLeaf(leaf->children[i]);
                }
            }
        }
    }
}

OctreeCell* Octree::addFaceLocal(OctreeCell* hintCell, uint32_t faceId, float bxmin, float bymin, float bzmin, float bxmax, float bymax, float bzmax, float cx, float cy, float cz) {
    if (!root) return nullptr;

    OctreeCell* cell = hintCell ? hintCell->parent : root;
    if (!cell) cell = root;

    while (cell->parent) {
        float* split = cell->aabbSplit;
        if (cx > split[0] && cy > split[1] && cz > split[2] && cx <= split[3] && cy <= split[4] && cz <= split[5]) {
            break;
        }
        cell = cell->parent;
    }

    cell->expandsAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    while (cell->children[0] != nullptr) {
        float* split = cell->aabbSplit;
        float xcen = (split[0] + split[3]) * 0.5f;
        float ycen = (split[1] + split[4]) * 0.5f;
        float zcen = (split[2] + split[5]) * 0.5f;
        int idx = getChildIndex(cx, cy, cz, xcen, ycen, zcen);
        cell = cell->children[idx];
        cell->expandsAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    }

    cell->iFaces.push_back(faceId);
    return cell;
}

OctreeCell* Octree::addFace(uint32_t faceId, float bxmin, float bymin, float bzmin, float bxmax, float bymax, float bzmax, float cx, float cy, float cz) {
    if (!root) return nullptr;

    float* split = root->aabbSplit;
    if (cx <= split[0] || cy <= split[1] || cz <= split[2] || cx > split[3] || cy > split[4] || cz > split[5])
        return nullptr;

    OctreeCell* cell = root;
    cell->expandsAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    while (cell->children[0] != nullptr) {
        split = cell->aabbSplit;
        float xcen = (split[0] + split[3]) * 0.5f;
        float ycen = (split[1] + split[4]) * 0.5f;
        float zcen = (split[2] + split[5]) * 0.5f;
        int idx = getChildIndex(cx, cy, cz, xcen, ycen, zcen);
        cell = cell->children[idx];
        cell->expandsAabbLoose(bxmin, bymin, bzmin, bxmax, bymax, bzmax);
    }

    cell->iFaces.push_back(faceId);
    return cell;
}

std::vector<uint32_t> Octree::collectIntersectRay(float vx, float vy, float vz, float rx, float ry, float rz) {
    std::vector<uint32_t> collectFaces;
    if (!root) return collectFaces;

    float irx = 1.0f / rx;
    float iry = 1.0f / ry;
    float irz = 1.0f / rz;

    OctreeCell* stack[256];
    int stackPtr = 0;
    stack[stackPtr++] = root;

    while (stackPtr > 0) {
        OctreeCell* cell = stack[--stackPtr];

        float* loose = cell->aabbLoose;
        float t1 = (loose[0] - vx) * irx;
        float t2 = (loose[3] - vx) * irx;
        float t3 = (loose[1] - vy) * iry;
        float t4 = (loose[4] - vy) * iry;
        float t5 = (loose[2] - vz) * irz;
        float t6 = (loose[5] - vz) * irz;

        float tmin = std::max({std::min(t1, t2), std::min(t3, t4), std::min(t5, t6)});
        float tmax = std::min({std::max(t1, t2), std::max(t3, t4), std::max(t5, t6)});

        if (tmax < 0 || tmin > tmax)
            continue;

        if (cell->children[0] != nullptr) {
            for (int i = 0; i < 8; ++i) {
                if (stackPtr < 256) {
                    stack[stackPtr++] = cell->children[i];
                }
            }
        } else {
            collectFaces.insert(collectFaces.end(), cell->iFaces.begin(), cell->iFaces.end());
        }
    }
    return collectFaces;
}

void Octree::scale(float sx, float sy, float sz) {
    if (!root) return;
    float asx = std::abs(sx);
    float asy = std::abs(sy);
    float asz = std::abs(sz);
    for (int i = 0; i < poolIndex; ++i) {
        OctreeCell* cell = cellPool[i];
        if (!cell) continue;
        cell->aabbLoose[0] *= asx; cell->aabbLoose[3] *= asx;
        cell->aabbLoose[1] *= asy; cell->aabbLoose[4] *= asy;
        cell->aabbLoose[2] *= asz; cell->aabbLoose[5] *= asz;
        cell->aabbSplit[0] *= asx; cell->aabbSplit[3] *= asx;
        cell->aabbSplit[1] *= asy; cell->aabbSplit[4] *= asy;
        cell->aabbSplit[2] *= asz; cell->aabbSplit[5] *= asz;
    }
}
