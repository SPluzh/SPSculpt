#pragma once
#include "mesh/MeshResolution.h"
#include <vector>
#include <memory>

enum class RenderHint { NONE = 0, SCULPT = 1, CAMERA = 2, PICKING = 3 };

class Multimesh : public Mesh {
public:
    std::vector<std::unique_ptr<MeshResolution>> meshes;
    int sel = 0;
    static RenderHint renderHint;

    Multimesh() = default;
    explicit Multimesh(std::unique_ptr<MeshResolution> baseMesh);
    explicit Multimesh(Mesh* mesh);
    ~Multimesh() = default;

    MeshResolution* getCurrentMesh();
    const MeshResolution* getCurrentMesh() const;

    void setSelection(int s);
    int getSelection() const { return sel; }

    MeshResolution* addLevel();
    MeshResolution* computeReverse();
    MeshResolution* lowerLevel();
    MeshResolution* higherLevel();
    void selectResolution(int targetSel);
    void deleteLower();
    void deleteHigher();

    void syncVisibility(int fromSel, int toSel);
    int getLowIndexRender() const;
    void syncToCurrentMesh();
    void updateResolution();

    void flip(int axisIndex) override;
    void mirror(int axisIndex, bool positiveToNegative, SymmetryMode mode) override;

    size_t getNbLevels() const { return meshes.size(); }
};
