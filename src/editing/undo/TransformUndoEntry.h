#pragma once
#include "UndoEntry.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

struct TransformMeshState {
    uint32_t meshId = 0;
    glm::mat4 beforeMatrix = glm::mat4(1.0f);
    glm::mat4 afterMatrix = glm::mat4(1.0f);

    bool bakedScale = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float scaleZ = 1.0f;

    bool hasVertexDeformation = false;
    std::vector<float> beforeVerts;
    std::vector<float> afterVerts;
    std::vector<float> beforeNormals;
    std::vector<float> afterNormals;
};

class TransformUndoEntry : public UndoEntry {
public:
    TransformUndoEntry(const std::string& desc = "Transform")
        : m_description(desc) {}

    UndoEntryType getType() const override { return UndoEntryType::SceneMeta; }
    std::string getDescription() const override { return m_description; }

    size_t getMemoryUsage() const override {
        size_t bytes = sizeof(*this) + m_description.capacity();
        bytes += m_meshStates.capacity() * sizeof(TransformMeshState);
        for (const auto& item : m_meshStates) {
            bytes += item.beforeVerts.capacity() * sizeof(float);
            bytes += item.afterVerts.capacity() * sizeof(float);
            bytes += item.beforeNormals.capacity() * sizeof(float);
            bytes += item.afterNormals.capacity() * sizeof(float);
        }
        return bytes;
    }

    std::vector<TransformMeshState> m_meshStates;

private:
    std::string m_description;
};
