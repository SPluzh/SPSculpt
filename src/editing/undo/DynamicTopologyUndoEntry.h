#pragma once
#include "UndoEntry.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include <memory>
#include <string>

class DynamicTopologyUndoEntry : public UndoEntry {
public:
    uint32_t meshId = 0;
    std::string description;
    std::unique_ptr<dynamic::TopologyUndo> record;

    DynamicTopologyUndoEntry(uint32_t mId, std::unique_ptr<dynamic::TopologyUndo> rec, const std::string& desc = "Dynamic Topology Stroke")
        : meshId(mId), description(desc), record(std::move(rec)) {}

    UndoEntryType getType() const override { return UndoEntryType::DynamicTopology; }
    std::string getDescription() const override { return description; }

    size_t getMemoryUsage() const override {
        return sizeof(*this) + description.capacity() + (record ? record->bytes() : 0);
    }
};
