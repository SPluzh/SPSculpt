#pragma once

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;

enum class ComponentMode {
    Vertex = 0,
    Edge,
    Face
};

class SelectionManager {
public:
    struct EdgeTopology {
        uint32_t v0 = 0xffffffff;
        uint32_t v1 = 0xffffffff;
        int32_t f0 = -1;
        int32_t f1 = -1;
        int8_t k0 = -1; // local edge index in f0
        int8_t k1 = -1; // local edge index in f1
    };

    SelectionManager();
    ~SelectionManager() = default;

    // Component mode (Vertex, Edge, Face)
    ComponentMode getMode() const { return m_mode; }
    void setMode(ComponentMode mode) { m_mode = mode; }

    // Query active selection
    bool hasSelection() const;
    bool hasSelectionInMode(ComponentMode mode) const;

    bool isVertexSelected(uint32_t vertIdx) const;
    bool isEdgeSelected(uint32_t edgeIdx) const;
    bool isFaceSelected(uint32_t faceIdx) const;

    size_t getSelectedVertexCount() const { return m_selectedVerts.size(); }
    size_t getSelectedEdgeCount() const { return m_selectedEdges.size(); }
    size_t getSelectedFaceCount() const { return m_selectedFaces.size(); }

    const std::unordered_set<uint32_t>& getSelectedVertices() const { return m_selectedVerts; }
    const std::unordered_set<uint32_t>& getSelectedEdges() const { return m_selectedEdges; }
    const std::unordered_set<uint32_t>& getSelectedFaces() const { return m_selectedFaces; }

    // Modification methods
    void selectVertex(uint32_t vertIdx, bool add = false);
    void deselectVertex(uint32_t vertIdx);
    void toggleVertex(uint32_t vertIdx);

    void selectEdge(uint32_t edgeIdx, bool add = false);
    void deselectEdge(uint32_t edgeIdx);
    void toggleEdge(uint32_t edgeIdx);

    void selectFace(uint32_t faceIdx, bool add = false);
    void deselectFace(uint32_t faceIdx);
    void toggleFace(uint32_t faceIdx);

    void clear();
    void clearMode(ComponentMode mode);
    void selectAll(const Mesh& mesh);
    void invertSelection(const Mesh& mesh);

    // Topological operations
    void selectEdgeLoop(const Mesh& mesh, uint32_t edgeIdx, bool add = true);
    void selectEdgeRing(const Mesh& mesh, uint32_t edgeIdx, bool add = true);
    void expandCurrentEdgeLoops(const Mesh& mesh);
    void expandCurrentEdgeRings(const Mesh& mesh);

    // Inter-tool conversion
    // Returns unique list of vertex indices for TransformTool deformation
    std::vector<uint32_t> getEffectiveVertexIndices(const Mesh& mesh) const;

    // Returns unique list of edge indices for EdgeCreaseTool
    std::vector<uint32_t> getEffectiveEdgeIndices(const Mesh& mesh) const;

    // Local centroid of all effective vertices
    glm::vec3 computeCentroidLocal(const Mesh& mesh) const;

    // Topology cache management
    void ensureEdgeTopology(const Mesh& mesh) const;
    void invalidateTopologyCache() { m_edgeTopologyValid = false; m_edgeTopology.clear(); }
    const std::vector<EdgeTopology>& getEdgeTopology(const Mesh& mesh) const {
        ensureEdgeTopology(mesh);
        return m_edgeTopology;
    }

private:
    ComponentMode m_mode = ComponentMode::Edge;
    std::unordered_set<uint32_t> m_selectedVerts;
    std::unordered_set<uint32_t> m_selectedEdges;
    std::unordered_set<uint32_t> m_selectedFaces;

    mutable std::vector<EdgeTopology> m_edgeTopology;
    mutable uint32_t m_cachedMeshId = 0;
    mutable int m_cachedNbEdges = 0;
    mutable int m_cachedNbFaces = 0;
    mutable bool m_edgeTopologyValid = false;
};
