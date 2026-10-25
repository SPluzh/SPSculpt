#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;

namespace Thickness {

    struct ThicknessParams {
        float thickness = 0.0f;     // Distance of extrusion (world units)
        float offset = 0.0f;        // -1.0 (inward) to 1.0 (outward), 0.0 = centered
        int   rimSegments = 1;      // Number of quads along the rim (1 to 4)
        bool  creaseRim = true;     // Crease the outer & inner boundary loops of the rim
        bool  generateGroups = true;// Generate distinct PolyGroups for Front, Back, Rim
        bool  evenThickness = true; // Angle-weighted miter compensation for uniform wall thickness
    };

    struct ThicknessCache {
        bool isValid = false;
        int baseNbVerts = 0;
        int baseNbFaces = 0;
        int rimSegments = 1;
        bool isClosedMesh = false;
        int outNbVerts = 0;
        int outNbFaces = 0;

        // Per base vertex: normal vector with miter compensation scale
        std::vector<glm::vec3> miterNormals;

        // Boundary vertex indices in base mesh (if open mesh)
        std::vector<uint32_t> boundaryBaseVerts;
        // Mapping from base vertex index to boundaryBaseVerts index (-1 if not on boundary)
        std::vector<int32_t> baseToBoundaryIdx;

        void clear() {
            isValid = false;
            baseNbVerts = 0;
            baseNbFaces = 0;
            rimSegments = 1;
            isClosedMesh = false;
            outNbVerts = 0;
            outNbFaces = 0;
            miterNormals.clear();
            boundaryBaseVerts.clear();
            baseToBoundaryIdx.clear();
        }
    };

    // Generates complete mesh with thickness and topology from baseMesh into outMesh
    void generateThickness(
        const Mesh& baseMesh,
        Mesh& outMesh,
        const ThicknessParams& params,
        ThicknessCache* cache = nullptr
    );

    // Fast geometry update (zero topology reallocation) when baseMesh verts move during sculpting
    void evaluateThicknessGeometry(
        const Mesh& baseMesh,
        Mesh& outMesh,
        const ThicknessParams& params,
        ThicknessCache& cache
    );

} // namespace Thickness
