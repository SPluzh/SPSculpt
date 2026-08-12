#pragma once
#include "UndoEntry.h"
#include <vector>
#include <cstdint>
#include <string>

// Stores granular delta of modified vertices for a single mesh
struct VertexDelta {
    uint32_t meshId = 0;
    std::vector<uint32_t> indices;    // Modified vertex indices

    // Data BEFORE operation (for undo)
    std::vector<float> prevVerts;     // indices.size() * 3
    std::vector<float> prevColors;    // indices.size() * 3 (if recorded)
    std::vector<float> prevMaterials; // indices.size() * 3 (if recorded)

    // Data AFTER operation (for redo)
    std::vector<float> nextVerts;     // indices.size() * 3
    std::vector<float> nextColors;
    std::vector<float> nextMaterials;

    bool hasColors    = false;
    bool hasMaterials = false;

    // Layer delta tracking
    bool hasLayerDeltas = false;
    int activeLayerIdx = -1;
    std::vector<float> prevLayerDeltas;
    std::vector<float> nextLayerDeltas;

    // Base layer tracking when layers exist
    bool hasBaseDeltas = false;
    std::vector<float> prevBaseVerts;
    std::vector<float> nextBaseVerts;
    std::vector<float> prevBaseColors;
    std::vector<float> nextBaseColors;
    std::vector<float> prevBaseMaterials;
    std::vector<float> nextBaseMaterials;
};

class SculptUndoEntry : public UndoEntry {
public:
    std::string description = "Sculpt stroke";
    std::vector<VertexDelta> deltas; // Delts per mesh

    UndoEntryType getType() const override { return UndoEntryType::Sculpt; }

    size_t getMemoryUsage() const override {
        size_t total = sizeof(*this);
        for (const auto& d : deltas) {
            total += d.indices.capacity() * sizeof(uint32_t);
            total += d.prevVerts.capacity() * sizeof(float);
            total += d.nextVerts.capacity() * sizeof(float);
            if (d.hasColors) {
                total += d.prevColors.capacity() * sizeof(float);
                total += d.nextColors.capacity() * sizeof(float);
            }
            if (d.hasMaterials) {
                total += d.prevMaterials.capacity() * sizeof(float);
                total += d.nextMaterials.capacity() * sizeof(float);
            }
            if (d.hasLayerDeltas) {
                total += d.prevLayerDeltas.capacity() * sizeof(float);
                total += d.nextLayerDeltas.capacity() * sizeof(float);
            }
            if (d.hasBaseDeltas) {
                total += d.prevBaseVerts.capacity() * sizeof(float);
                total += d.nextBaseVerts.capacity() * sizeof(float);
                total += d.prevBaseColors.capacity() * sizeof(float);
                total += d.nextBaseColors.capacity() * sizeof(float);
                total += d.prevBaseMaterials.capacity() * sizeof(float);
                total += d.nextBaseMaterials.capacity() * sizeof(float);
            }
        }
        return total;
    }

    std::string getDescription() const override { return description; }
};
