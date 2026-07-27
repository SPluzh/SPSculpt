#include "editing/undo/UndoManager.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "common/Logger.h"
#include <algorithm>
#include <unordered_set>

UndoManager g_undoManager;

namespace {
    // Helper to keep track of recorded vertex indices per mesh during an active stroke
    static std::unordered_map<uint32_t, std::unordered_set<uint32_t>> s_recordedVertSets;
}

UndoManager::UndoManager() {}

void UndoManager::beginSculptStroke(Scene& scene,
                                     uint32_t meshId,
                                     const std::vector<uint32_t>& affectedVerts,
                                     bool affectsColors,
                                     bool affectsMaterials,
                                     const std::string& description) {
    if (!m_activeSculptEntry) {
        m_activeSculptEntry = std::make_unique<SculptUndoEntry>();
        m_activeSculptEntry->description = description;
        m_activeMeshDeltaMap.clear();
        s_recordedVertSets.clear();
        sculpt_log_lvl(LogLevel::Debug, "[Undo] Begin sculpt stroke: '%s'\n", description.c_str());
    }
    recordAffectedVertices(scene, meshId, affectedVerts, affectsColors, affectsMaterials);
}

void UndoManager::recordAffectedVertices(Scene& scene,
                                         uint32_t meshId,
                                         const std::vector<uint32_t>& affectedVerts,
                                         bool affectsColors,
                                         bool affectsMaterials) {
    if (!m_activeSculptEntry || affectedVerts.empty()) return;

    Mesh* mesh = scene.getMeshById(meshId);
    if (!mesh) return;

    size_t deltaIdx = 0;
    auto mapIt = m_activeMeshDeltaMap.find(meshId);
    if (mapIt == m_activeMeshDeltaMap.end()) {
        VertexDelta newDelta;
        newDelta.meshId = meshId;
        newDelta.hasColors = affectsColors;
        newDelta.hasMaterials = affectsMaterials;
        m_activeSculptEntry->deltas.push_back(newDelta);
        deltaIdx = m_activeSculptEntry->deltas.size() - 1;
        m_activeMeshDeltaMap[meshId] = deltaIdx;
    } else {
        deltaIdx = mapIt->second;
    }

    VertexDelta& delta = m_activeSculptEntry->deltas[deltaIdx];
    if (affectsColors) delta.hasColors = true;
    if (affectsMaterials) delta.hasMaterials = true;

    auto& recordedSet = s_recordedVertSets[meshId];

    for (uint32_t v : affectedVerts) {
        if (v >= (uint32_t)mesh->nbVerts) continue;
        if (recordedSet.insert(v).second) {
            // First time this vertex is recorded in this stroke
            delta.indices.push_back(v);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 0]);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 1]);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 2]);

            if (affectsColors && !mesh->colors.empty()) {
                delta.prevColors.push_back(mesh->colors[v * 3 + 0]);
                delta.prevColors.push_back(mesh->colors[v * 3 + 1]);
                delta.prevColors.push_back(mesh->colors[v * 3 + 2]);
            }
            if (affectsMaterials && !mesh->materials.empty()) {
                delta.prevMaterials.push_back(mesh->materials[v * 3 + 0]);
                delta.prevMaterials.push_back(mesh->materials[v * 3 + 1]);
                delta.prevMaterials.push_back(mesh->materials[v * 3 + 2]);
            }
        }
    }
}

void UndoManager::endSculptStroke(Scene& scene) {
    if (!m_activeSculptEntry) return;

    bool hasAnyChange = false;
    size_t totalRecordedVerts = 0;
    for (auto& delta : m_activeSculptEntry->deltas) {
        Mesh* mesh = scene.getMeshById(delta.meshId);
        if (!mesh) continue;

        totalRecordedVerts += delta.indices.size();
        delta.nextVerts.clear();
        delta.nextVerts.reserve(delta.indices.size() * 3);
        if (delta.hasColors) {
            delta.nextColors.clear();
            delta.nextColors.reserve(delta.indices.size() * 3);
        }
        if (delta.hasMaterials) {
            delta.nextMaterials.clear();
            delta.nextMaterials.reserve(delta.indices.size() * 3);
        }

        for (uint32_t v : delta.indices) {
            delta.nextVerts.push_back(mesh->verts[v * 3 + 0]);
            delta.nextVerts.push_back(mesh->verts[v * 3 + 1]);
            delta.nextVerts.push_back(mesh->verts[v * 3 + 2]);

            if (delta.hasColors && !mesh->colors.empty()) {
                delta.nextColors.push_back(mesh->colors[v * 3 + 0]);
                delta.nextColors.push_back(mesh->colors[v * 3 + 1]);
                delta.nextColors.push_back(mesh->colors[v * 3 + 2]);
            }
            if (delta.hasMaterials && !mesh->materials.empty()) {
                delta.nextMaterials.push_back(mesh->materials[v * 3 + 0]);
                delta.nextMaterials.push_back(mesh->materials[v * 3 + 1]);
                delta.nextMaterials.push_back(mesh->materials[v * 3 + 2]);
            }
        }

        if (delta.prevVerts != delta.nextVerts ||
            (delta.hasColors && delta.prevColors != delta.nextColors) ||
            (delta.hasMaterials && delta.prevMaterials != delta.nextMaterials)) {
            hasAnyChange = true;
        }
    }

    if (hasAnyChange && !m_activeSculptEntry->deltas.empty()) {
        sculpt_log_lvl(LogLevel::Info, "[Undo] Sculpt stroke completed: '%s' (%zu verts recorded, size: %.2f KB)\n",
                       m_activeSculptEntry->description.c_str(), totalRecordedVerts, m_activeSculptEntry->getMemoryUsage() / 1024.0f);
        pushEntry(std::move(m_activeSculptEntry));
    } else {
        sculpt_log_lvl(LogLevel::Debug, "[Undo] Sculpt stroke canceled or no changes detected\n");
        m_activeSculptEntry.reset();
    }

    m_activeMeshDeltaMap.clear();
    s_recordedVertSets.clear();
}

void UndoManager::cancelSculptStroke() {
    sculpt_log_lvl(LogLevel::Debug, "[Undo] Sculpt stroke canceled\n");
    m_activeSculptEntry.reset();
    m_activeMeshDeltaMap.clear();
    s_recordedVertSets.clear();
}

void UndoManager::pushSculptOperation(Scene& scene,
                                       uint32_t meshId,
                                       const std::string& description,
                                       std::function<void()> operation,
                                       bool affectsColors,
                                       bool affectsMaterials) {
    Mesh* mesh = scene.getMeshById(meshId);
    if (!mesh) {
        operation();
        return;
    }

    std::vector<uint32_t> allVerts(mesh->nbVerts);
    for (uint32_t i = 0; i < (uint32_t)mesh->nbVerts; ++i) allVerts[i] = i;

    beginSculptStroke(scene, meshId, allVerts, affectsColors, affectsMaterials, description);
    operation();
    endSculptStroke(scene);
}

void UndoManager::pushTopologyChange(Scene& scene,
                                      const std::string& description,
                                      std::function<void()> operation) {
    auto entry = std::make_unique<TopologyUndoEntry>();
    entry->description = description;
    entry->before = scene.saveCurrentState();
    
    operation();

    entry->after = scene.saveCurrentState();
    sculpt_log_lvl(LogLevel::Info, "[Undo] Topology change pushed: '%s' (size: %.2f MB)\n",
                   description.c_str(), entry->getMemoryUsage() / (1024.0f * 1024.0f));
    pushEntry(std::move(entry));
}

void UndoManager::pushMetaChange(Scene& scene,
                                  const std::string& description,
                                  std::function<void()> operation) {
    auto entry = std::make_unique<SceneMetaUndoEntry>();
    entry->description = description;
    entry->before = scene.saveCurrentState();

    operation();

    entry->after = scene.saveCurrentState();
    sculpt_log_lvl(LogLevel::Info, "[Undo] Meta change pushed: '%s'\n", description.c_str());
    pushEntry(std::move(entry));
}

void UndoManager::pushLegacyState(Scene& scene, const std::string& description) {
    pushTopologyChange(scene, description, [](){});
}

void UndoManager::pushEntry(std::unique_ptr<UndoEntry> entry) {
    m_redoStack.clear();
    m_undoStack.push_back(std::move(entry));
    trimToMemoryLimit();
    sculpt_log_lvl(LogLevel::Debug, "[Undo] Stack size: %zu undo, %zu redo (total mem: %.2f MB)\n",
                   m_undoStack.size(), m_redoStack.size(), getTotalMemoryUsage() / (1024.0f * 1024.0f));
}

void UndoManager::trimToMemoryLimit() {
    if (m_maxEntries > 0) {
        while (m_undoStack.size() > m_maxEntries) {
            sculpt_log_lvl(LogLevel::Warning, "[Undo] Trimming stack due to maxEntries limit (%zu entries)\n", m_maxEntries);
            m_undoStack.pop_front();
        }
    }
    while (getTotalMemoryUsage() > m_maxMemory && !m_undoStack.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Trimming stack due to maxMemory limit (%.2f MB / %.2f GB)\n",
                       getTotalMemoryUsage() / (1024.0f * 1024.0f), (double)m_maxMemory / (1024.0 * 1024.0 * 1024.0));
        m_undoStack.pop_front();
    }
}


size_t UndoManager::getTotalMemoryUsage() const {
    size_t total = 0;
    for (const auto& e : m_undoStack) {
        if (e) total += e->getMemoryUsage();
    }
    for (const auto& e : m_redoStack) {
        if (e) total += e->getMemoryUsage();
    }
    return total;
}

std::string UndoManager::getUndoDescription() const {
    if (m_undoStack.empty()) return "";
    return m_undoStack.back()->getDescription();
}

std::string UndoManager::getRedoDescription() const {
    if (m_redoStack.empty()) return "";
    return m_redoStack.back()->getDescription();
}

void UndoManager::clear() {
    m_undoStack.clear();
    m_redoStack.clear();
    m_activeSculptEntry.reset();
    m_activeMeshDeltaMap.clear();
    s_recordedVertSets.clear();
}

void UndoManager::applyEntry(UndoEntry* entry, Scene& scene, bool isUndo) {
    if (!entry) return;

    if (entry->getType() == UndoEntryType::Sculpt) {
        auto* e = static_cast<SculptUndoEntry*>(entry);
        for (const auto& delta : e->deltas) {
            Mesh* mesh = scene.getMeshById(delta.meshId);
            if (!mesh) continue;

            const auto& srcVerts = isUndo ? delta.prevVerts : delta.nextVerts;
            const auto& srcColors = isUndo ? delta.prevColors : delta.nextColors;
            const auto& srcMats = isUndo ? delta.prevMaterials : delta.nextMaterials;

            size_t count = delta.indices.size();
            for (size_t i = 0; i < count; ++i) {
                uint32_t vi = delta.indices[i];
                if (vi >= (uint32_t)mesh->nbVerts) continue;

                mesh->verts[vi * 3 + 0] = srcVerts[i * 3 + 0];
                mesh->verts[vi * 3 + 1] = srcVerts[i * 3 + 1];
                mesh->verts[vi * 3 + 2] = srcVerts[i * 3 + 2];

                if (delta.hasColors && i * 3 + 2 < srcColors.size() && vi * 3 + 2 < mesh->colors.size()) {
                    mesh->colors[vi * 3 + 0] = srcColors[i * 3 + 0];
                    mesh->colors[vi * 3 + 1] = srcColors[i * 3 + 1];
                    mesh->colors[vi * 3 + 2] = srcColors[i * 3 + 2];
                }
                if (delta.hasMaterials && i * 3 + 2 < srcMats.size() && vi * 3 + 2 < mesh->materials.size()) {
                    mesh->materials[vi * 3 + 0] = srcMats[i * 3 + 0];
                    mesh->materials[vi * 3 + 1] = srcMats[i * 3 + 1];
                    mesh->materials[vi * 3 + 2] = srcMats[i * 3 + 2];
                }
            }

            if (!delta.indices.empty()) {
                auto [minIt, maxIt] = std::minmax_element(delta.indices.begin(), delta.indices.end());
                mesh->dirtyVertMin = *minIt;
                mesh->dirtyVertMax = *maxIt;
                mesh->isVertexDirty = true;
                if (delta.hasColors) mesh->isColorDirty = true;
                if (delta.hasMaterials) mesh->isMaterialDirty = true;
                mesh->isDirty = true;

                updateFaceNormalsAndBoxes(
                    mesh->verts.data(), mesh->nbVerts,
                    mesh->faces.data(), mesh->nbFaces,
                    nullptr, -1,
                    mesh->faceNormals.data(),
                    mesh->faceBoxes.data(),
                    mesh->faceCenters.data()
                );
                updateVertexNormals(
                    nullptr, -1, mesh->nbVerts,
                    mesh->vrfStartCount.data(),
                    mesh->vertRingFace.data(),
                    mesh->faceNormals.data(),
                    mesh->normals.data()
                );
                mesh->octree.build(
                    mesh->nbVerts, mesh->nbFaces,
                    mesh->faceCenters.data(),
                    mesh->faceBoxes.data(),
                    mesh->verts.data(),
                    mesh->faces.data()
                );
            }
        }
    } else if (entry->getType() == UndoEntryType::Topology) {
        auto* e = static_cast<TopologyUndoEntry*>(entry);
        scene.restoreState(isUndo ? e->before : e->after);
    } else if (entry->getType() == UndoEntryType::SceneMeta) {
        auto* e = static_cast<SceneMetaUndoEntry*>(entry);
        scene.restoreState(isUndo ? e->before : e->after);
    }
}

void UndoManager::undo(Scene& scene) {
    if (m_undoStack.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Nothing to undo!\n");
        return;
    }

    auto entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    sculpt_log_lvl(LogLevel::Info, "[Undo] Executing UNDO: '%s'\n", entry->getDescription().c_str());
    applyEntry(entry.get(), scene, true);

    m_redoStack.push_back(std::move(entry));
}

void UndoManager::redo(Scene& scene) {
    if (m_redoStack.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Nothing to redo!\n");
        return;
    }

    auto entry = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    sculpt_log_lvl(LogLevel::Info, "[Undo] Executing REDO: '%s'\n", entry->getDescription().c_str());
    applyEntry(entry.get(), scene, false);

    m_undoStack.push_back(std::move(entry));
}

