#pragma once
#include "mesh/Mesh.h"
#include <vector>
#include <cstdint>

class MeshResolution : public Mesh {
public:
    std::vector<float>    detailsXYZ;
    std::vector<float>    detailsRGB;
    std::vector<float>    detailsPBR;
    std::vector<uint32_t> vertMapping;
    bool                  evenMapping = false;

    MeshResolution() = default;
    explicit MeshResolution(const Mesh& other, bool keepMesh = false);
    ~MeshResolution() = default;

    bool getEvenMapping() const { return evenMapping; }
    void setEvenMapping(bool boolVal) { evenMapping = boolVal; }

    const std::vector<uint32_t>& getVerticesMapping() const { return vertMapping; }
    std::vector<uint32_t>& getVerticesMapping() { return vertMapping; }
    void setVerticesMapping(const std::vector<uint32_t>& vm) { vertMapping = vm; }
    void clearVerticesMapping() { vertMapping.clear(); }

    void higherSynthesis(MeshResolution& meshDown);
    void lowerAnalysis(MeshResolution& meshUp);
    void copyDataFromHigherRes(MeshResolution& meshUp);
    void computePartialSubdivision(std::vector<float>& subdVerts,
                                   std::vector<float>& subdColors,
                                   std::vector<float>& subdMaterials,
                                   int nbVerticesUp);
    void applyDetails();
    void computeDetails(const std::vector<float>& subdVerts,
                        const std::vector<float>& subdColors,
                        const std::vector<float>& subdMaterials,
                        int nbVerticesUp);
};
