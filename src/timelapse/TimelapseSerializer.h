#pragma once

#include "editing/undo/UndoEntry.h"
#include "editing/undo/SculptUndoEntry.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "editing/undo/SceneMetaUndoEntry.h"
#include "scene/Scene.h"
#include "common/Version.h"
#include <string>
#include <vector>
#include <memory>
#include <iostream>

struct TimelapseMetadata {
    std::string title = "Sculpt Session";
    std::string author = "Artist";
    std::string creationDate;
    std::string appVersion = Version::STRING;
    int totalStrokes = 0;
};

class TimelapseSerializer {
public:
    static bool saveToFile(
        const std::string& filepath,
        const HistoryState& initialState,
        const std::vector<std::unique_ptr<UndoEntry>>& timeline,
        const TimelapseMetadata& metadata = {}
    );

    static bool loadFromFile(
        const std::string& filepath,
        HistoryState& outInitialState,
        std::vector<std::unique_ptr<UndoEntry>>& outTimeline,
        TimelapseMetadata& outMetadata
    );

private:
    static void writeMeshState(std::ostream& os, const MeshState& ms);
    static void readMeshState(std::istream& is, MeshState& ms);

    static void writeHistoryState(std::ostream& os, const HistoryState& hs);
    static void readHistoryState(std::istream& is, HistoryState& hs);

    static void writeVertexDelta(std::ostream& os, const VertexDelta& delta);
    static void readVertexDelta(std::istream& is, VertexDelta& delta);
};
