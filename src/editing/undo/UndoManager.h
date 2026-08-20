#pragma once
#include "UndoEntry.h"
#include "SculptUndoEntry.h"
#include "TopologyUndoEntry.h"
#include "SceneMetaUndoEntry.h"
#include "TransformUndoEntry.h"

#include <deque>
#include <memory>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>

class Scene;
class Mesh;

class UndoManager {
public:
    static constexpr size_t DEFAULT_MAX_MEMORY = 4ULL * 1024 * 1024 * 1024; // 4 GB
    static constexpr size_t DEFAULT_MAX_ENTRIES = 0; // 0 = unlimited (limited only by memory)

    UndoManager();
    ~UndoManager() = default;

    // --- API for sculpt stroke / delta tracking ---
    void beginSculptStroke(Scene& scene,
                           uint32_t meshId,
                           const std::vector<uint32_t>& affectedVerts,
                           bool affectsColors = false,
                           bool affectsMaterials = true,
                           const std::string& description = "Sculpt stroke");

    void recordAffectedVertices(Scene& scene,
                                uint32_t meshId,
                                const std::vector<uint32_t>& affectedVerts,
                                bool affectsColors = false,
                                bool affectsMaterials = true);

    void endSculptStroke(Scene& scene);
    void cancelSculptStroke();

    // Helper for direct/atomic sculpt operations
    void pushSculptOperation(Scene& scene,
                             uint32_t meshId,
                             const std::string& description,
                             std::function<void()> operation,
                             bool affectsColors = false,
                             bool affectsMaterials = true);

    // --- API for topology-changing operations ---
    void pushTopologyChange(Scene& scene,
                            const std::string& description,
                            std::function<void()> operation);
    void pushTopologyChange(Scene& scene,
                            const std::string& description,
                            HistoryState beforeState,
                            HistoryState afterState);

    // --- API for scene meta changes ---
    void pushMetaChange(Scene& scene,
                        const std::string& description,
                        std::function<void()> operation);

    void pushTransformChange(Scene& scene, TransformUndoEntry entry);


    // Legacy fallback state push
    void pushLegacyState(Scene& scene, const std::string& description = "Operation");

    // --- Undo / Redo Execution ---
    void undo(Scene& scene);
    void redo(Scene& scene);

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    // Diagnostics & Memory Management
    size_t getTotalMemoryUsage() const;
    size_t getUndoCount() const { return m_undoStack.size(); }
    size_t getRedoCount() const { return m_redoStack.size(); }
    std::string getUndoDescription() const;
    std::string getRedoDescription() const;

    const std::deque<std::unique_ptr<UndoEntry>>& getUndoStack() const { return m_undoStack; }
    const std::deque<std::unique_ptr<UndoEntry>>& getRedoStack() const { return m_redoStack; }

    // Timelapse stack ownership transfer
    std::deque<std::unique_ptr<UndoEntry>> takeUndoStack() {
        auto stack = std::move(m_undoStack);
        m_undoStack.clear();
        m_redoStack.clear();
        return stack;
    }

    void restoreUndoStack(std::deque<std::unique_ptr<UndoEntry>> stack) {
        m_undoStack = std::move(stack);
        m_redoStack.clear();
    }

    static void applyEntry(UndoEntry* entry, Scene& scene, bool isUndo);

    void clear();
    void setMaxMemory(size_t bytes) { m_maxMemory = bytes; trimToMemoryLimit(); }
    void setMaxMemoryGB(double gb)  { setMaxMemory(static_cast<size_t>(gb * 1024.0 * 1024.0 * 1024.0)); }
    void setMaxEntries(size_t n)    { m_maxEntries = n; trimToMemoryLimit(); }
    size_t getMaxMemory() const { return m_maxMemory; }
    double getMaxMemoryGB() const { return static_cast<double>(m_maxMemory) / (1024.0 * 1024.0 * 1024.0); }
    size_t getMaxEntries() const { return m_maxEntries; }

private:
    std::deque<std::unique_ptr<UndoEntry>> m_undoStack;
    std::deque<std::unique_ptr<UndoEntry>> m_redoStack;

    size_t m_maxMemory  = DEFAULT_MAX_MEMORY;
    size_t m_maxEntries = DEFAULT_MAX_ENTRIES;

    // Active sculpt stroke entry being constructed during brush strokes
    std::unique_ptr<SculptUndoEntry> m_activeSculptEntry;
    std::unordered_map<uint32_t, size_t> m_activeMeshDeltaMap;

    void pushEntry(std::unique_ptr<UndoEntry> entry);
    void trimToMemoryLimit();
};

extern UndoManager g_undoManager;
