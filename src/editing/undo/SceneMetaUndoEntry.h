#pragma once
#include "UndoEntry.h"
#include "scene/Scene.h"
#include <string>

class SceneMetaUndoEntry : public UndoEntry {
public:
    std::string description = "Scene change";
    HistoryState before;
    HistoryState after;

    UndoEntryType getType() const override { return UndoEntryType::SceneMeta; }

    size_t getMemoryUsage() const override {
        auto calcState = [](const HistoryState& hs) {
            size_t total = sizeof(hs);
            for (const auto& ms : hs.meshes) {
                total += ms.verts.capacity() * sizeof(float);
                total += ms.colors.capacity() * sizeof(float);
                total += ms.materials.capacity() * sizeof(float);
                total += ms.faces.capacity() * sizeof(uint32_t);
                total += ms.vrfStartCount.capacity() * sizeof(uint32_t);
                total += ms.vertRingFace.capacity() * sizeof(uint32_t);
                total += ms.vrvStartCount.capacity() * sizeof(uint32_t);
                total += ms.vertRingVert.capacity() * sizeof(uint32_t);
                total += ms.vertOnEdge.capacity() * sizeof(uint8_t);
                total += ms.vertVisible.capacity() * sizeof(uint8_t);
            }
            return total;
        };
        return calcState(before) + calcState(after);
    }

    std::string getDescription() const override { return description; }
};
