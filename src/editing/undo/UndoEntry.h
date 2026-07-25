#pragma once
#include <cstddef>
#include <string>

enum class UndoEntryType {
    Sculpt,      // Delta for vertex positions, colors, materials
    Topology,    // Full state snapshot for mesh topology changes
    SceneMeta    // Scene metadata changes (transforms, names, visibility, selection)
};

class UndoEntry {
public:
    virtual ~UndoEntry() = default;
    virtual UndoEntryType getType() const = 0;
    virtual size_t getMemoryUsage() const = 0;
    virtual std::string getDescription() const = 0;
};
