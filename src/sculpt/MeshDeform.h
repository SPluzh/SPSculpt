#pragma once

#include <string>
#include <vector>

class Mesh;
class Scene;

enum class MeshDeformOp {
    Smooth = 0,
    Relax = 1,
    Inflate = 2
};

struct MeshDeformOptions {
    float amount = 0.0f;          // Smooth/Relax: 0..100, Inflate: -100..100
    bool preserveVolume = true;   // Smooth: Taubin smoothing (avoids shrinkage)
    bool preserveBoundary = true; // Keep open boundary vertices (vertOnEdge == 1) fixed
    bool useMask = true;          // Respect painted mask (materials[v*3 + 2])
};

// Computes deformed vertex positions from `baseVerts` without mutating `mesh`.
// Returns false if the mesh has no valid geometry or topology.
bool deformMeshPositions(
    const Mesh& mesh,
    const std::vector<float>& baseVerts,
    MeshDeformOp op,
    const MeshDeformOptions& options,
    std::vector<float>& outVerts
);

// Writes `newVerts` back into `mesh`, updates normals, dirty GPU blocks, and optionally BVH.
void applyDeformedPositionsToMesh(
    Mesh& mesh,
    const std::vector<float>& newVerts,
    bool updateBVH = true
);

// Commits a finished deformation as a single undoable step (supports LayerStack recording).
void commitMeshDeformationUndo(
    Scene& scene,
    Mesh& mesh,
    std::vector<float>&& beforeVerts,
    std::vector<float>&& beforeNormals,
    const std::string& description
);

