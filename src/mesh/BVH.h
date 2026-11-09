#pragma once

#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <glm/glm.hpp>

class Mesh;

struct RayHit {
    float t = std::numeric_limits<float>::infinity();
    uint32_t faceId = 0xFFFFFFFF;
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f };
};

struct BVHPickStats {
    double collectCellsMs = 0.0;
    double visitedInitMs = 0.0;
    double filterVertsMs = 0.0;
    double mergeMs = 0.0;
    uint32_t candLeaves = 0;
    uint32_t candFaces = 0;
    uint32_t outVerts = 0;
    bool usedMT = false;
    int numThreads = 1;
};

struct BVHRefitStats {
    double leafDedupMs = 0.0;
    double leafAABBMs = 0.0;
    double parentGatherMs = 0.0;
    double innerUpdateMs = 0.0;
    uint32_t inputFaces = 0;
    uint32_t dirtyLeaves = 0;
    uint32_t dirtyInnerNodes = 0;
    bool usedPrecomputed = false;
    bool fellBackToRefitAll = false;
};

// 32-byte cache-aligned BVH node (2 nodes fit perfectly into a 64-byte L1 cache line)
struct alignas(16) BVHNode {
    glm::vec3 aabbMin{ std::numeric_limits<float>::infinity() };
    uint32_t firstIndex = 0; // Leaf: offset into m_primitiveIndices; Inner: index of left child (right = firstIndex + 1)
    glm::vec3 aabbMax{ -std::numeric_limits<float>::infinity() };
    uint32_t count = 0;      // > 0 for leaf (number of faces); 0 for inner node

    bool isLeaf() const { return count > 0; }
    bool isInner() const { return count == 0 && firstIndex != 0xFFFFFFFF; }
    bool isEmptyLeaf() const { return count == 0 && firstIndex == 0xFFFFFFFF; }
};

class BVH {
public:
    static constexpr uint32_t kLeafSize = 16;

    BVH() = default;
    ~BVH() = default;

    // Copies clear acceleration cache to avoid duplicating large spatial buffers in undo/snapshots
    BVH(const BVH&) { clear(); }
    BVH& operator=(const BVH&) { clear(); return *this; }

    BVH(BVH&& other) noexcept {
        m_nodes = std::move(other.m_nodes);
        m_parents = std::move(other.m_parents);
        m_primitiveIndices = std::move(other.m_primitiveIndices);
        m_faceToLeaf = std::move(other.m_faceToLeaf);
        m_leafCount = other.m_leafCount; other.m_leafCount = 0;
        m_editsSinceBuild = other.m_editsSinceBuild; other.m_editsSinceBuild = 0;
        m_scratch[0].clear();
        m_scratch[1].clear();
        m_leafVisitedFlags = std::move(other.m_leafVisitedFlags);
        m_leafVisitedEpoch = other.m_leafVisitedEpoch; other.m_leafVisitedEpoch = 0;
        m_nodeVisitedFlags = std::move(other.m_nodeVisitedFlags);
        m_nodeVisitedEpoch = other.m_nodeVisitedEpoch; other.m_nodeVisitedEpoch = 0;
        m_dirtyLeaves = std::move(other.m_dirtyLeaves);
        m_dirtyInnerNodes = std::move(other.m_dirtyInnerNodes);
        m_dirtyInnerNodesTemp = std::move(other.m_dirtyInnerNodesTemp);
        lastCollectCellsMs = other.lastCollectCellsMs;
        lastCandidateVertsMs = other.lastCandidateVertsMs;
        lastFilterVertsMs = other.lastFilterVertsMs;
        m_lastPickStats[0] = other.m_lastPickStats[0];
        m_lastPickStats[1] = other.m_lastPickStats[1];
        m_lastRefitStats = other.m_lastRefitStats;
        m_leafCap = std::move(other.m_leafCap);
        m_editedLeaves = std::move(other.m_editedLeaves);
        m_leafEdited = std::move(other.m_leafEdited);
        m_unusedEntries = other.m_unusedEntries; other.m_unusedEntries = 0;
    }

    BVH& operator=(BVH&& other) noexcept {
        if (this != &other) {
            clear();
            m_nodes = std::move(other.m_nodes);
            m_parents = std::move(other.m_parents);
            m_primitiveIndices = std::move(other.m_primitiveIndices);
            m_faceToLeaf = std::move(other.m_faceToLeaf);
            m_leafCount = other.m_leafCount; other.m_leafCount = 0;
            m_editsSinceBuild = other.m_editsSinceBuild; other.m_editsSinceBuild = 0;
            m_scratch[0].clear();
            m_scratch[1].clear();
            m_leafVisitedFlags = std::move(other.m_leafVisitedFlags);
            m_leafVisitedEpoch = other.m_leafVisitedEpoch; other.m_leafVisitedEpoch = 0;
            m_nodeVisitedFlags = std::move(other.m_nodeVisitedFlags);
            m_nodeVisitedEpoch = other.m_nodeVisitedEpoch; other.m_nodeVisitedEpoch = 0;
            m_dirtyLeaves = std::move(other.m_dirtyLeaves);
            m_dirtyInnerNodes = std::move(other.m_dirtyInnerNodes);
            m_dirtyInnerNodesTemp = std::move(other.m_dirtyInnerNodesTemp);
            lastCollectCellsMs = other.lastCollectCellsMs;
            lastCandidateVertsMs = other.lastCandidateVertsMs;
            lastFilterVertsMs = other.lastFilterVertsMs;
            m_lastPickStats[0] = other.m_lastPickStats[0];
            m_lastPickStats[1] = other.m_lastPickStats[1];
            m_lastRefitStats = other.m_lastRefitStats;
            m_leafCap = std::move(other.m_leafCap);
            m_editedLeaves = std::move(other.m_editedLeaves);
            m_leafEdited = std::move(other.m_leafEdited);
            m_unusedEntries = other.m_unusedEntries; other.m_unusedEntries = 0;
        }
        return *this;
    }

    // Clears all internal buffers
    void clear();

    // Builds the BVH over the given mesh faces (supports mixed triangles and quads)
    void build(const Mesh& mesh);

    // Casts a ray against the mesh, returning true if hit. Updates hit.t with nearest hit distance.
    bool raycast(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir, RayHit& hit) const;

    // Backward-compatible query: collects all face IDs whose leaf AABBs intersect the ray
    void collectIntersectRay(const glm::vec3& origin, const glm::vec3& dir, std::vector<uint32_t>& outFaces) const;

    // Gathers all unique visible vertex indices within radiusSquared of center
    void pickVerticesInSphere(
        const Mesh& mesh, const glm::vec3& center, float radiusSquared,
        std::vector<uint32_t>& outVertices, const uint8_t* vertVisible = nullptr,
        int scratchIndex = 0) const;

    void pickVerticesInSphere(
        const Mesh& mesh, float cx, float cy, float cz, float radiusSquared,
        std::vector<uint32_t>& outVertices, const uint8_t* vertVisible = nullptr,
        int scratchIndex = 0) const {
        pickVerticesInSphere(mesh, glm::vec3(cx, cy, cz), radiusSquared, outVertices, vertVisible, scratchIndex);
    }

    std::vector<uint32_t> pickVerticesInSphere(
        const Mesh& mesh, const glm::vec3& center, float radiusSquared,
        const uint8_t* vertVisible = nullptr, int scratchIndex = 0) const {
        std::vector<uint32_t> res;
        pickVerticesInSphere(mesh, center, radiusSquared, res, vertVisible, scratchIndex);
        return res;
    }

    std::vector<uint32_t> pickVerticesInSphere(
        const Mesh& mesh, float cx, float cy, float cz, float radiusSquared,
        const uint8_t* vertVisible = nullptr, int scratchIndex = 0) const {
        std::vector<uint32_t> res;
        pickVerticesInSphere(mesh, glm::vec3(cx, cy, cz), radiusSquared, res, vertVisible, scratchIndex);
        return res;
    }

    // Incremental refit for vertices moved during a sculpt stroke (O(K log N))
    void refit(const Mesh& mesh, const uint32_t* modifiedFaces, size_t count, bool usePrecomputedBoxes = false);

    // Full refit of all leaf and inner node AABBs without rebuilding tree topology
    void refitAll(const Mesh& mesh, bool usePrecomputedBoxes = false);

    // Uniform/non-uniform scaling of all AABBs
    void scale(float sx, float sy, float sz);

    // Warmup query scratch buffers to eliminate first-call page faults on large meshes
    void warmup(int nbVerts);

    // Dynamic topology edits
    void insertTriangle(const Mesh& mesh, uint32_t t, uint32_t nearby = 0xFFFFFFFF);
    void removeTriangle(uint32_t t);
    void renumberTriangle(uint32_t from, uint32_t to);
    void resizeTriangles(size_t count);
    void touchTriangle(uint32_t t);
    void refitChanged(const Mesh& mesh);
    void queryTriangles(const Mesh& mesh, const glm::vec3& center, float radius, std::vector<uint32_t>& out) const;

    // True if cumulative sculpting strokes degraded AABB overlap enough to warrant a background rebuild
    bool isDegraded() const;

    // Diagnostics & inspections
    bool isBuilt() const { return !m_nodes.empty(); }
    bool empty() const { return m_nodes.empty(); }
    size_t nodeCount() const { return m_nodes.size(); }
    size_t leafCount() const { return m_leafCount; }
    size_t primitiveCount() const { return m_primitiveIndices.size(); }
    size_t editsSinceBuild() const { return m_editsSinceBuild; }
    size_t unusedEntries() const { return m_unusedEntries; }
    size_t memoryBytes() const;
    size_t countEmptyLeaves() const {
        size_t c = 0;
        for (const auto& n : m_nodes) {
            if (n.count == 0 && n.firstIndex == 0xFFFFFFFF) ++c;
        }
        return c;
    }
    size_t computeMaxDepth() const {
        if (m_nodes.empty()) return 0;
        size_t maxD = 0;
        struct Item { uint32_t idx; size_t d; };
        std::vector<Item> st = {{0, 1}};
        while (!st.empty()) {
            Item cur = st.back();
            st.pop_back();
            if (cur.idx >= m_nodes.size()) continue;
            if (cur.d > maxD) maxD = cur.d;
            const BVHNode& n = m_nodes[cur.idx];
            if (!n.isLeaf() && n.firstIndex != 0xFFFFFFFF && n.firstIndex + 1 < m_nodes.size()) {
                st.push_back({n.firstIndex, cur.d + 1});
                st.push_back({n.firstIndex + 1, cur.d + 1});
            }
        }
        return maxD;
    }
    void resetDiagnosticCounters() const {
        m_emptyLeafHazardCount = 0;
        m_leafCapHazardCount = 0;
        m_stackOverflowHazardCount = 0;
    }
    size_t emptyLeafHazardCount() const { return m_emptyLeafHazardCount; }
    size_t leafCapHazardCount() const { return m_leafCapHazardCount; }
    size_t stackOverflowHazardCount() const { return m_stackOverflowHazardCount; }

    void getRootAABB(float outAabb[6]) const {
        if (!m_nodes.empty()) {
            outAabb[0] = m_nodes[0].aabbMin.x;
            outAabb[1] = m_nodes[0].aabbMin.y;
            outAabb[2] = m_nodes[0].aabbMin.z;
            outAabb[3] = m_nodes[0].aabbMax.x;
            outAabb[4] = m_nodes[0].aabbMax.y;
            outAabb[5] = m_nodes[0].aabbMax.z;
        } else {
            outAabb[0] = outAabb[1] = outAabb[2] = 0.0f;
            outAabb[3] = outAabb[4] = outAabb[5] = 0.0f;
        }
    }

    const std::vector<BVHNode>& nodes() const { return m_nodes; }
    const std::vector<uint32_t>& primitiveIndices() const { return m_primitiveIndices; }

    mutable double lastCollectCellsMs = 0.0;
    mutable double lastCandidateVertsMs = 0.0;
    mutable double lastFilterVertsMs = 0.0;
    mutable BVHPickStats m_lastPickStats[2];
    mutable BVHRefitStats m_lastRefitStats;
    mutable size_t m_emptyLeafHazardCount = 0;
    mutable size_t m_leafCapHazardCount = 0;
    mutable size_t m_stackOverflowHazardCount = 0;

    const BVHPickStats& getLastPickStats(int scratchIndex = 0) const { return m_lastPickStats[scratchIndex & 1]; }
    const BVHRefitStats& getLastRefitStats() const { return m_lastRefitStats; }

    // Validation
    bool validateTree(const Mesh& mesh, std::string& outErr) const;

private:
    struct QueryScratch {
        std::vector<uint8_t> visitedFlags;
        uint8_t visitedEpoch = 0;
        std::vector<uint32_t> candidateLeaves;
        std::vector<std::vector<uint32_t>> threadPicked;

        void clear() {
            visitedFlags.clear();
            visitedEpoch = 0;
            candidateLeaves.clear();
            threadPicked.clear();
        }
    };
    mutable QueryScratch m_scratch[2];

    std::vector<BVHNode> m_nodes;
    std::vector<uint32_t> m_parents;
    std::vector<uint32_t> m_primitiveIndices;
    std::vector<uint32_t> m_faceToLeaf;

    size_t m_leafCount = 0;
    size_t m_editsSinceBuild = 0;

    // Epoch tracking for unique dirty leaves / parents during refit
    std::vector<uint8_t> m_leafVisitedFlags;
    uint8_t m_leafVisitedEpoch = 0;
    std::vector<uint8_t> m_nodeVisitedFlags;
    uint8_t m_nodeVisitedEpoch = 0;

    // Reusable buffers for refit
    mutable std::vector<uint32_t> m_dirtyLeaves;
    mutable std::vector<uint32_t> m_dirtyInnerNodes;
    mutable std::vector<uint32_t> m_dirtyInnerNodesTemp;

    // Dynamic topology edit tracking
    std::vector<uint32_t> m_leafCap;
    std::vector<uint32_t> m_editedLeaves;
    std::vector<uint8_t> m_leafEdited;
    size_t m_unusedEntries = 0;

    void editLeaf(uint32_t leaf);
    void splitLeaf(const Mesh& mesh, uint32_t leaf);
    void computeLeafAABB(const Mesh& mesh, BVHNode& node, uint32_t nodeIdx, bool usePrecomputedBoxes = false, bool assignFaceToLeaf = false);
};

