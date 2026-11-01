#include "mesh/Octree.h"
#include "common/Logger.h"
#include <cstring>
#include <chrono>
#include <iostream>
#include <omp.h>
#include <atomic>

OctreeCell* Octree::getCell() {
    if (poolIndex < (int)cellPool.size()) {
        OctreeCell* cell = cellPool[poolIndex++];
        cell->parent = nullptr;
        cell->depth = 0;
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
        return cell;
    } else {
        OctreeCell* cell = new OctreeCell();
        cellPool.push_back(cell);
        poolIndex++;
        return cell;
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
    root = getCell();

    faceLeaf.assign(nbFaces, nullptr);
    facePosInLeaf.assign(nbFaces, 0);

    root->iFaces.resize(nbFaces);
    for (int i = 0; i < nbFaces; ++i) {
        root->iFaces[i] = i;
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

    root->setAabbLoose(xmin, ymin, zmin, xmax, ymax, zmax);
    root->setAabbSplit(xmin2, ymin2, zmin2, xmax2, ymax2, zmax2);

    std::vector<OctreeCell*> stack;
    stack.push_back(root);
    std::vector<OctreeCell*> leaves;

    const int MAX_DEPTH = 8;

    while (!stack.empty()) {
        OctreeCell* cell = stack.back();
        stack.pop_back();

        int cellNbFaces = cell->iFaces.size();
        if (cellNbFaces > maxFaces && cell->depth < MAX_DEPTH) {
            constructChildren(cell);
            for (int i = 0; i < 8; ++i) {
                stack.push_back(cell->children[i]);
            }
        } else if (cellNbFaces > 0) {
            leaves.push_back(cell);
        }
    }

    for (auto* leaf : leaves) {
        constructLeaf(leaf);
    }
}

inline int getChildIndex(float cx, float cy, float cz, float xcen, float ycen, float zcen) {
    static const int lut[8] = {0, 3, 4, 7, 1, 2, 5, 6};
    int key = ((cx > xcen) << 2) | ((cy > ycen) << 1) | (cz > zcen);
    return lut[key];
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

    int nbFacesLocal = cell->iFaces.size();

    int counts[8] = {0};
    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t id = cell->iFaces[i] * 3;
        counts[getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen)]++;
    }

    for (int i = 0; i < 8; ++i) {
        cell->children[i]->iFaces.reserve(counts[i]);
    }

    std::vector<uint32_t>* childFaces[8] = {
        &cell->children[0]->iFaces,
        &cell->children[1]->iFaces,
        &cell->children[2]->iFaces,
        &cell->children[3]->iFaces,
        &cell->children[4]->iFaces,
        &cell->children[5]->iFaces,
        &cell->children[6]->iFaces,
        &cell->children[7]->iFaces
    };

    for (int i = 0; i < nbFacesLocal; ++i) {
        uint32_t iFace = cell->iFaces[i];
        uint32_t id = iFace * 3;
        int idx = getChildIndex(faceCentersData[id], faceCentersData[id + 1], faceCentersData[id + 2], xcen, ycen, zcen);
        childFaces[idx]->push_back(iFace);
    }

    cell->children[0]->setAabbSplit(xmin, ymin, zmin, xcen, ycen, zcen);
    cell->children[1]->setAabbSplit(xcen, ymin, zmin, xmax, ycen, zcen);
    cell->children[2]->setAabbSplit(xcen, ymin, zcen, xmax, ycen, zmax);
    cell->children[3]->setAabbSplit(xmin, ymin, zcen, xcen, ycen, zmax);
    cell->children[4]->setAabbSplit(xmin, ycen, zmin, xcen, ymax, zcen);
    cell->children[5]->setAabbSplit(xcen, ycen, zmin, xmax, ymax, zcen);
    cell->children[6]->setAabbSplit(xcen, ycen, zcen, xmax, ymax, zmax);
    cell->children[7]->setAabbSplit(xmin, ycen, zcen, xcen, ymax, zmax);

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

    if (iFacesInCells.capacity() < 65536) {
        iFacesInCells.reserve(131072);
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
