#pragma once

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;
struct BrushSettings;

enum RelaxAlgorithm {
    RELAX_ALGO_LAPLACIAN = 0,
    RELAX_ALGO_TANGENTIAL = 1,
    RELAX_ALGO_PROJECT = 2
};

enum RelaxMode {
    RELAX_MODE_AUTOLOCK = 0,
    RELAX_MODE_INTERIOR = 1,
    RELAX_MODE_BORDER = 2,
    RELAX_MODE_ALL = 3
};

class RelaxEngine {
public:
    RelaxEngine();
    ~RelaxEngine();

    RelaxEngine(const RelaxEngine&) = delete;
    RelaxEngine& operator=(const RelaxEngine&) = delete;

    // Call when starting a brush stroke to precompute topology, incident polygons,
    // boundaries, creases, and cache initial point positions for reprojection.
    bool beginStroke(
        Mesh* mesh,
        int algorithm,
        bool preserveCreases,
        float creaseAngleDeg
    );

    // Call when dragging ends or stroke is cancelled to release stroke cache.
    void endStroke();

    // Relax vertices within 3D brush radius.
    // Supports Laplacian, Tangential Slide, and Surface Reprojection modes,
    // along with boundary locking, hard edge/crease preservation, multi-pass iterations,
    // and vertex mask filtering.
    int relaxVertices(
        Mesh* mesh,
        const std::vector<uint32_t>& pickedVertices,
        const glm::vec3& center,
        float radius,
        float intensity,
        const BrushSettings& settings
    );

    // Determines whether the contact point is closer to boundary edges.
    bool isCursorNearBorder(
        const Mesh* mesh,
        const glm::vec3& center,
        float brushRadius
    ) const;

    static glm::vec3 closestPointOnTriangle(
        const glm::vec3& p,
        const glm::vec3& a,
        const glm::vec3& b,
        const glm::vec3& c
    );

private:
    bool ensureMeshTopology(
        Mesh* mesh,
        bool preserveCreases,
        float creaseAngleDeg
    );

    struct BoundaryEdgeEntry {
        uint32_t u = 0;
        uint32_t v = 0;
    };

    // Stroke cache state
    bool m_strokeActive = false;
    Mesh* m_cachedMesh = nullptr;
    int m_cachedAlgorithm = RELAX_ALGO_TANGENTIAL;
    bool m_cachedPreserveCreases = true;
    float m_cachedCreaseAngleDeg = 45.0f;

    // Topology structures
    std::vector<glm::vec3> m_initialPositions;
    std::vector<std::vector<uint32_t>> m_boundaryNeighbors;
    std::vector<std::vector<uint32_t>> m_creaseNeighbors;
    std::vector<BoundaryEdgeEntry> m_boundaryEdges;
};
