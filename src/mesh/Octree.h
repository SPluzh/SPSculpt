#pragma once

#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <unordered_set>

struct OctreeCell {
    OctreeCell* parent = nullptr;
    int depth = 0;
    OctreeCell* children[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    std::vector<uint32_t> iFaces;
    bool dirty = false;
    float aabbLoose[6] = {
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()
    };
    float aabbSplit[6] = {
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()
    };

    ~OctreeCell() {
        // Managed by Octree cellPool
    }

    void setAabbSplit(float xmin, float ymin, float zmin, float xmax, float ymax, float zmax) {
        aabbSplit[0] = xmin; aabbSplit[1] = ymin; aabbSplit[2] = zmin;
        aabbSplit[3] = xmax; aabbSplit[4] = ymax; aabbSplit[5] = zmax;
    }

    void setAabbLoose(float xmin, float ymin, float zmin, float xmax, float ymax, float zmax) {
        aabbLoose[0] = xmin; aabbLoose[1] = ymin; aabbLoose[2] = zmin;
        aabbLoose[3] = xmax; aabbLoose[4] = ymax; aabbLoose[5] = zmax;
    }

    void expandsAabbLoose(float bxmin, float bymin, float bzmin, float bxmax, float bymax, float bzmax) {
        OctreeCell* curr = this;
        while (curr) {
            bool proceed = false;
            if (bxmin < curr->aabbLoose[0]) { curr->aabbLoose[0] = bxmin; proceed = true; }
            if (bymin < curr->aabbLoose[1]) { curr->aabbLoose[1] = bymin; proceed = true; }
            if (bzmin < curr->aabbLoose[2]) { curr->aabbLoose[2] = bzmin; proceed = true; }
            if (bxmax > curr->aabbLoose[3]) { curr->aabbLoose[3] = bxmax; proceed = true; }
            if (bymax > curr->aabbLoose[4]) { curr->aabbLoose[4] = bymax; proceed = true; }
            if (bzmax > curr->aabbLoose[5]) { curr->aabbLoose[5] = bzmax; proceed = true; }
            curr = proceed ? curr->parent : nullptr;
        }
    }
};

class Octree {
public:
    OctreeCell* root = nullptr;

    const float* vertsData = nullptr;
    const uint32_t* facesData = nullptr;
    const float* faceCentersData = nullptr;
    const float* faceBoxesData = nullptr;

    std::vector<OctreeCell*> faceLeaf;
    std::vector<int> facePosInLeaf;

    int nbVerts = 0;
    int nbFaces = 0;
    int maxFaces = 100;

    std::vector<uint32_t> lastPickedVertices;
    std::vector<uint32_t> lastIntersectedFaces;

    std::vector<OctreeCell*> cellPool;
    int poolIndex = 0;

    ~Octree() {
        for (OctreeCell* cell : cellPool) {
            delete cell;
        }
    }

    OctreeCell* getCell();

    void build(int nbVertsVal, int nbFacesVal,
               const float* centersPtrVal, const float* boxesPtrVal,
               const float* vertsPtrVal, const uint32_t* facesPtrVal);

    std::vector<uint32_t> pickVerticesInSphere(
        float cx, float cy, float cz, float radius2, const uint8_t* vertVisible);

    std::vector<uint32_t> collectIntersectRay(
        float vx, float vy, float vz, float rx, float ry, float rz);

    void update(const float* vertsPtrVal, int nbVertsVal,
                const uint32_t* facesPtrVal, int nbFacesVal,
                const float* boxesPtrVal, const uint32_t* iFacesPtr, int nbIFacesVal);

    void rebuildInternal();

private:
    std::vector<uint32_t> m_visitedFlags;
    uint32_t m_visitedEpoch = 0;

    void constructChildren(OctreeCell* cell);
    void constructLeaf(OctreeCell* cell);
    std::vector<uint32_t> collectIntersectSphere(float vx, float vy, float vz, float radiusSquared);
    void pruneIfPossible(OctreeCell* cell, std::unordered_set<OctreeCell*>& deletedCells);
    void balanceOctree(std::vector<OctreeCell*>& leavesToUpdate);
    OctreeCell* addFace(uint32_t faceId, float bxmin, float bymin, float bzmin, float bxmax, float bymax, float bzmax, float cx, float cy, float cz);
    OctreeCell* addFaceLocal(OctreeCell* hintCell, uint32_t faceId, float bxmin, float bymin, float bzmin, float bxmax, float bymax, float bzmax, float cx, float cy, float cz);
};
