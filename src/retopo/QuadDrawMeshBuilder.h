#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <algorithm>
#include <cstdint>
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "retopo/SurfaceSnapper.h"

namespace retopo {

inline bool PolygonHasEdge(const std::vector<uint32_t>& faces, int32_t pIdx, int32_t u, int32_t v) {
    if (pIdx < 0 || (size_t)pIdx * 4 + 3 >= faces.size()) return false;
    uint32_t a = faces[pIdx * 4 + 0];
    uint32_t b = faces[pIdx * 4 + 1];
    uint32_t c = faces[pIdx * 4 + 2];
    uint32_t d = faces[pIdx * 4 + 3];
    bool isQuad = (d != TRI_INDEX);

    uint32_t uu = (uint32_t)u;
    uint32_t vv = (uint32_t)v;

    if ((a == uu && b == vv) || (a == vv && b == uu)) return true;
    if ((b == uu && c == vv) || (b == vv && c == uu)) return true;
    if (isQuad) {
        if ((c == uu && d == vv) || (c == vv && d == uu)) return true;
        if ((d == uu && a == vv) || (d == vv && a == uu)) return true;
    } else {
        if ((c == uu && a == vv) || (c == vv && a == uu)) return true;
    }
    return false;
}

inline bool PolygonHasVertex(const std::vector<uint32_t>& faces, int32_t pIdx, int32_t v) {
    if (pIdx < 0 || (size_t)pIdx * 4 + 3 >= faces.size()) return false;
    uint32_t vv = (uint32_t)v;
    uint32_t a = faces[pIdx * 4 + 0];
    uint32_t b = faces[pIdx * 4 + 1];
    uint32_t c = faces[pIdx * 4 + 2];
    uint32_t d = faces[pIdx * 4 + 3];
    if (a == vv || b == vv || c == vv) return true;
    if (d != TRI_INDEX && d == vv) return true;
    return false;
}

struct QuadPreview {
    bool valid = false;
    int32_t v[4] = {-1, -1, -1, -1};
    glm::vec3 worldPositions[4];
    glm::vec3 screenPositions[4];
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

struct LoopEdge {
    int32_t v0 = -1;
    int32_t v1 = -1;
    glm::vec3 worldPos0{0.0f};
    glm::vec3 worldPos1{0.0f};
};

struct EdgeLoopResult {
    std::vector<LoopEdge> edges;
    bool isClosed = false;
};

struct CutPoint {
    int32_t v0 = -1;
    int32_t v1 = -1;
    float t = 0.5f;
    glm::vec3 worldPos{0.0f};
    int32_t newVertexIdx = -1;
};

struct CutSegment {
    glm::vec3 p0{0.0f};
    glm::vec3 p1{0.0f};
    int32_t polyIndex = -1;
};

struct QuadSplitInfo {
    int32_t polyIndex = -1;
    int32_t cutPtA = -1;
    int32_t cutPtB = -1;
    int32_t q[4] = {-1, -1, -1, -1};
};

struct TriangleSplitInfo {
    int32_t polyIndex = -1;
    int32_t cutPt = -1;
    int32_t t[3] = {-1, -1, -1};
};

struct EdgeCutResult {
    bool valid = false;
    float paramT = 0.5f;
    int32_t primaryV0 = -1;
    int32_t primaryV1 = -1;
    int32_t primaryPoly = -1;
    std::vector<CutPoint> cutPoints;
    std::vector<CutSegment> cutSegments;
    std::vector<QuadSplitInfo> quadSplits;
    std::vector<TriangleSplitInfo> triSplits;
    bool isClosed = false;
};

enum class MultiCutSnapType {
    None = 0,
    Vertex,
    Edge,
    Face
};

struct MultiCutPoint {
    MultiCutSnapType type = MultiCutSnapType::None;
    int32_t vertexIdx = -1;
    int32_t edgeV0 = -1;
    int32_t edgeV1 = -1;
    float edgeT = 0.5f;
    int32_t polyIndex = -1;
    glm::vec3 worldPos{0.0f};
};

struct PolygonCut {
    int32_t polyIndex = -1;

    bool end0IsVertex = false;
    int32_t end0Vertex = -1;
    int32_t end0EdgeV0 = -1;
    int32_t end0EdgeV1 = -1;
    float end0EdgeT = 0.5f;
    glm::vec3 end0WorldPos{0.0f};

    bool end1IsVertex = false;
    int32_t end1Vertex = -1;
    int32_t end1EdgeV0 = -1;
    int32_t end1EdgeV1 = -1;
    float end1EdgeT = 0.5f;
    glm::vec3 end1WorldPos{0.0f};
};

struct MultiCutSliceSegment {
    glm::vec3 p0{0.0f};
    glm::vec3 p1{0.0f};
    int32_t polyIndex = -1;
};

struct MultiCutResult {
    bool valid = false;
    std::vector<PolygonCut> cuts;
    std::vector<MultiCutSliceSegment> previewSegments;
};

struct ExtrudeEdgeResult {
    bool valid = false;
    int32_t newV0 = -1;
    int32_t newV1 = -1;
    int32_t newPoly = -1;
};

struct ExtrudeEdgesResult {
    bool valid = false;
    std::unordered_map<int32_t, int32_t> origToNew;
    std::vector<int32_t> newVertices;
    std::vector<int32_t> newPolygons;
    std::vector<std::pair<int32_t, int32_t>> newFrontEdges;
};

struct ExtrudeVertexResult {
    bool valid = false;
    int32_t newVertex = -1;
    int32_t newPoly = -1;
    int32_t neighA = -1;
    int32_t neighB = -1;
};

struct BorderChain {
    std::vector<int32_t> vertices;
    std::vector<std::pair<int32_t, int32_t>> edges;
    bool isClosed = false;
};

struct StrokeExtrudeResult {
    bool valid = false;
    std::vector<int32_t> newVertices;
    std::vector<int32_t> newPolygons;
    std::vector<std::pair<int32_t, int32_t>> newFrontEdges;
};

struct GrabVertexInfo {
    int32_t index = -1;
    glm::vec3 initLocalPos{0.0f};
    glm::vec3 initWorldPos{0.0f};
    glm::vec3 initNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 initScreenPos{0.0f};
    float weight = 0.0f;
};

struct TubeSliceResult {
    bool valid = false;
    std::vector<glm::vec3> worldPoints;
    glm::vec3 center{0.0f};
    glm::vec3 normal{0.0f};
    float radius = 0.0f;
};

struct TubeBuildResult {
    bool valid = false;
    std::vector<int32_t> newLoopVertices;
    int32_t quadsCreated = 0;
};

struct TubeSegmentResult {
    bool valid = false;
    std::vector<std::vector<glm::vec3>> rings; // rings[ringIndex][radialVertexIndex]
    int32_t radialSegments = 0;
    int32_t spans = 0;
    int32_t quadsCount = 0;
};

class QuadDrawMeshBuilder {
public:
    QuadDrawMeshBuilder();
    ~QuadDrawMeshBuilder();

    // Get existing retopo mesh or create a new clean one in scene
    Mesh* GetOrCreateRetopoMesh(Scene& scene, Mesh* targetMesh);

    // Geometry Primitives
    int32_t AddVertex(Mesh* mesh, const glm::vec3& worldPos);
    bool AddQuad(Mesh* mesh, int32_t a, int32_t b, int32_t c, int32_t d, const glm::vec3& targetNormal);
    bool AddTriangle(Mesh* mesh, int32_t a, int32_t b, int32_t c, const glm::vec3& targetNormal);
    bool SetVertexPosition(Mesh* mesh, int32_t index, const glm::vec3& worldPos);
    bool WeldVertices(Mesh* mesh, int32_t vSource, int32_t vTarget, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool WeldVerticesPair(Mesh* mesh, int32_t src0, int32_t tgt0, int32_t src1, int32_t tgt1, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool WeldVerticesMultiple(Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& welds, std::unordered_set<uint32_t>* pinnedVertices = nullptr, std::vector<int32_t>* outRemap = nullptr);

    // Deletion Operations
    bool DeleteVertex(Mesh* mesh, int32_t ptIndex, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeleteVertices(Mesh* mesh, const std::vector<int32_t>& ptIndices, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeleteEdge(Mesh* mesh, int32_t v0, int32_t v1, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeleteSingleEdge(Mesh* mesh, int32_t v0, int32_t v1, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeleteEdges(Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeleteEdgeLoop(Mesh* mesh, const std::vector<LoopEdge>& loopEdges, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeletePolygon(Mesh* mesh, int32_t polyIndex, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    bool DeletePolygons(Mesh* mesh, const std::vector<int32_t>& polyIndices, std::unordered_set<uint32_t>* pinnedVertices = nullptr);
    int32_t DeleteAllUnconnectedPoints(Mesh* mesh, std::unordered_set<uint32_t>* pinnedVertices = nullptr);

    // Loops
    EdgeLoopResult FindEdgeLoop(const Mesh* mesh, int32_t startV0, int32_t startV1);
    std::vector<int32_t> FindPolygonLoop(const Mesh* mesh, int32_t startPoly, int32_t enterV0, int32_t enterV1);
    std::vector<int32_t> FindVertexLoop(const Camera& camera, const Mesh* mesh, int32_t startV, float screenX, float screenY, std::vector<LoopEdge>* outEdges = nullptr);
    BorderChain FindBorderChain(const Mesh* mesh, int32_t startV0, int32_t startV1);
    std::vector<BorderChain> ExtractBorderChains(const Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges);

    // Screen Queries
    int32_t FindPolygonUnderScreen(
        const Camera& camera, const Mesh* mesh, float screenX, float screenY,
        const Mesh* targetMesh = nullptr, SurfaceSnapper* snapper = nullptr,
        float* outAvgZ = nullptr, const SnapResult* precomputedTargetSnap = nullptr
    );
    QuadPreview FindPotentialQuad(
        const Camera& camera, const Mesh* retopo, const glm::vec3& targetNormal,
        float screenX, float screenY, const Mesh* targetMesh = nullptr, SurfaceSnapper* snapper = nullptr
    );
    float ComputeEdgeParam(const Camera& camera, const Mesh* mesh, int32_t v0, int32_t v1, float screenX, float screenY);
    EdgeHit FindClosestEdgeOfPolygon(const Camera& camera, const Mesh* mesh, int32_t polyIdx, float screenX, float screenY);

    // Edge Loop Cut (Knife)
    EdgeCutResult FindEdgeLoopCut(
        const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const Camera& camera, int32_t startV0, int32_t startV1, float startT, int32_t hintPoly = -1
    );
    bool ApplyEdgeLoopCut(Mesh* retopo, const EdgeCutResult& cutResult);

    // Multi-Cut
    MultiCutResult BuildMultiCutFromPoints(
        const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const std::vector<MultiCutPoint>& points, const MultiCutPoint* candidateHover = nullptr
    );
    MultiCutResult BuildSliceCut(
        const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const glm::vec2& screenP0, const glm::vec2& screenP1, bool cutThrough = true, bool cutInfinite = false
    );
    bool ApplyPolygonCuts(Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper, const std::vector<PolygonCut>& cuts);

    // Extrusion
    ExtrudeEdgeResult ExtrudeEdge(Mesh* mesh, int32_t v0, int32_t v1, const glm::vec3& pos0, const glm::vec3& pos1, const glm::vec3& targetNormal);
    ExtrudeEdgesResult ExtrudeEdges(Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges, const glm::vec3& targetNormal);
    bool CanExtrudeVertex(const Mesh* mesh, int32_t vCorner, int32_t* outNeighA = nullptr, int32_t* outNeighB = nullptr);
    ExtrudeVertexResult ExtrudeVertex(Mesh* mesh, int32_t vCorner, const glm::vec3& targetNormal, const glm::vec3& initialPos);
    StrokeExtrudeResult ExtrudeEdgesToStroke(
        Mesh* mesh, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const BorderChain& chain, const std::vector<glm::vec3>& strokeWorldPoints,
        float weldRadiusPixels = 0.0f, const Camera* camera = nullptr
    );

    // Tube / Contours Retopology
    TubeSliceResult SliceTargetMeshForTube(
        const Camera& camera, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const glm::vec2& screenP0, const glm::vec2& screenP1, int32_t radialSegments
    );
    TubeBuildResult BuildTubeRing(
        Mesh* retopoMesh, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const std::vector<glm::vec3>& newRingPoints,
        const std::vector<int32_t>& previousLoopVertices,
        int32_t spans = 1
    );
    TubeSegmentResult GenerateTubeBetweenPoints(
        const Mesh* targetMesh, SurfaceSnapper& snapper,
        const glm::vec3& startWorldPos, const glm::vec3& endWorldPos,
        int32_t radialSegments, int32_t spans
    );
    TubeSegmentResult GenerateTubeFromPoints(
        const Mesh* targetMesh, SurfaceSnapper& snapper,
        const std::vector<glm::vec3>& centerlinePoints,
        int32_t radialSegments, int32_t spansPerSegment
    );
    TubeBuildResult CommitTubeMesh(
        Mesh* retopoMesh, const Mesh* targetMesh, SurfaceSnapper& snapper,
        const TubeSegmentResult& tubeGeom
    );

    // Brushes (Relax & Grab)
    bool IsBoundaryOrIsolatedVertex(const Mesh* mesh, int32_t ptIndex);
    bool IsCursorNearBorder(
        const Mesh* retopo, const Camera& camera, float screenX, float screenY,
        float brushRadius, const Mesh* target = nullptr, SurfaceSnapper* snapper = nullptr, bool visibleOnly = true
    );
    bool RelaxVertices(
        Mesh* retopo, const Mesh* target, SurfaceSnapper& snapper,
        const Camera& camera, float screenX, float screenY, float brushRadius, float strength,
        bool lockBorder = false, bool lockInterior = false, bool visibleOnly = true,
        const std::unordered_set<uint32_t>* pinnedVertices = nullptr
    );
    bool CollectGrabVertices(
        const Mesh* retopo, const Mesh* target, SurfaceSnapper& snapper,
        const Camera& camera, float screenX, float screenY, float brushRadius, float intensity,
        int32_t falloffType, bool visibleOnly,
        std::vector<GrabVertexInfo>& outVertices, glm::vec3& outGrabCenterWorld,
        const std::unordered_set<uint32_t>* pinnedVertices = nullptr
    );

    // Cache and Notification
    void InvalidateEdgeCache() { m_edgeCacheValid = false; }
    void NotifyMeshUpdated(Mesh* mesh);

private:
    struct EdgeCacheEntry {
        int32_t u;
        int32_t v;
        int32_t count;
    };
    bool m_edgeCacheValid = false;
    std::vector<EdgeCacheEntry> m_edges;
    std::vector<std::vector<int32_t>> m_allNeighbors;
    std::vector<std::vector<int32_t>> m_boundaryNeighbors;
    const Mesh* m_cachedMesh = nullptr;

    void EnsureEdgeCache(const Mesh* retopo);
};

} // namespace retopo
