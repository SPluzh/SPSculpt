#pragma once

#include "UndoEntry.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

class Mesh;

namespace UndoSerializer {

// Serializes undo and redo stacks into `outBuffer` (appending to existing data).
// Non-topology entries are compressed per-entry with LZ4 in parallel via OpenMP;
// TopologyUndoEntry payloads are written directly as they are already LZ4-compressed.
bool serializeStacks(
    const std::deque<std::unique_ptr<UndoEntry>>& undoStack,
    const std::deque<std::unique_ptr<UndoEntry>>& redoStack,
    std::vector<uint8_t>& outBuffer
);

// Deserializes undo and redo stacks starting at `byteOffset` in `data` (advances `byteOffset`).
// `meshIdMap` is used to rebind runtime Mesh* pointers inside MeasurementAnchor.
bool deserializeStacks(
    const uint8_t* data,
    size_t size,
    size_t& byteOffset,
    std::deque<std::unique_ptr<UndoEntry>>& outUndoStack,
    std::deque<std::unique_ptr<UndoEntry>>& outRedoStack,
    const std::unordered_map<uint32_t, Mesh*>& meshIdMap
);

} // namespace UndoSerializer

