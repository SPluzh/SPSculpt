#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

class PolyGroupTool {
public:
    PolyGroupTool() = default;
    ~PolyGroupTool() = default;

    // Create group from masked faces of mesh
    void createGroupFromMask(Mesh* mesh, float maskThreshold = 0.5f);

    // Auto group connected topological components
    void autoGroupByConnectedComponents(Mesh* mesh);

    // Assign specific group ID to a face
    void assignGroupToFace(Mesh* mesh, uint32_t faceIdx, uint32_t groupId);

    // Flood fill group starting from faceIdx
    void floodFillGroup(Mesh* mesh, uint32_t startFaceIdx, uint32_t groupId);

    // Clear all groups (set to 0)
    void clearAllGroups(Mesh* mesh);

    // Get group ID at face
    uint32_t getGroupAtFace(const Mesh* mesh, uint32_t faceIdx) const;

    // Get list of all unique active group IDs (excluding 0)
    std::vector<uint32_t> getAllGroupIDs(const Mesh* mesh) const;
};
