#include "editing/undo/UndoManager.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "common/Logger.h"
#include <algorithm>
#include <unordered_set>

UndoManager g_undoManager;

namespace {
    // Helper to keep track of recorded vertex indices per mesh during an active stroke using fast array stamps
    static std::unordered_map<uint32_t, std::vector<uint32_t>> s_recordedStamps;
    static uint32_t s_recordedCurrentStamp = 0;
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
        ++s_recordedCurrentStamp;
        if (s_recordedCurrentStamp == 0) {
            s_recordedStamps.clear();
            s_recordedCurrentStamp = 1;
        }
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

    auto& stamps = s_recordedStamps[meshId];
    if (stamps.size() < static_cast<size_t>(mesh->nbVerts)) {
        stamps.resize(mesh->nbVerts, 0);
    }
    uint32_t currentStamp = s_recordedCurrentStamp;

    Layer* activeLayer = mesh->isLayerActive() ? mesh->layerStack.getActive() : nullptr;
    int activeLayerIdx = mesh->isLayerActive() ? mesh->layerStack.getActiveIdx() : -1;
    if (activeLayer) {
        delta.hasLayerDeltas = true;
        delta.activeLayerIdx = activeLayerIdx;
    }

    for (uint32_t v : affectedVerts) {
        if (v >= (uint32_t)mesh->nbVerts) continue;
        if (stamps[v] != currentStamp) {
            stamps[v] = currentStamp;
            // First time this vertex is recorded in this stroke
            delta.indices.push_back(v);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 0]);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 1]);
            delta.prevVerts.push_back(mesh->verts[v * 3 + 2]);

            if (activeLayer) {
                if (v * 3 + 2 < activeLayer->deltaVerts.size()) {
                    delta.prevLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 0]);
                    delta.prevLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 1]);
                    delta.prevLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 2]);
                } else {
                    delta.prevLayerDeltas.push_back(0.0f);
                    delta.prevLayerDeltas.push_back(0.0f);
                    delta.prevLayerDeltas.push_back(0.0f);
                }
            }

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

        Layer* activeLayer = (delta.hasLayerDeltas && mesh->isLayerActive()) ? mesh->layerStack.getLayer(delta.activeLayerIdx) : nullptr;
        if (!activeLayer && delta.hasLayerDeltas && mesh->isLayerActive()) {
            activeLayer = mesh->layerStack.getActive();
        }
        if (activeLayer) {
            delta.nextLayerDeltas.clear();
            delta.nextLayerDeltas.reserve(delta.indices.size() * 3);
        }

        for (uint32_t v : delta.indices) {
            delta.nextVerts.push_back(mesh->verts[v * 3 + 0]);
            delta.nextVerts.push_back(mesh->verts[v * 3 + 1]);
            delta.nextVerts.push_back(mesh->verts[v * 3 + 2]);

            if (activeLayer) {
                if (v * 3 + 2 < activeLayer->deltaVerts.size()) {
                    delta.nextLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 0]);
                    delta.nextLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 1]);
                    delta.nextLayerDeltas.push_back(activeLayer->deltaVerts[v * 3 + 2]);
                } else {
                    delta.nextLayerDeltas.push_back(0.0f);
                    delta.nextLayerDeltas.push_back(0.0f);
                    delta.nextLayerDeltas.push_back(0.0f);
                }
            }

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
            (delta.hasLayerDeltas && delta.prevLayerDeltas != delta.nextLayerDeltas) ||
            (delta.hasColors && delta.prevColors != delta.nextColors) ||
            (delta.hasMaterials && delta.prevMaterials != delta.nextMaterials)) {
            hasAnyChange = true;
        }
    }

    if (hasAnyChange && !m_activeSculptEntry->deltas.empty()) {
        sculpt_log_lvl(LogLevel::Info, "[Undo] Sculpt stroke completed: '%s' (%zu verts recorded, size: %.2f KB)\n",
                       m_activeSculptEntry->description.c_str(), totalRecordedVerts, m_activeSculptEntry->getMemoryUsage() / 1024.0f);
        pushEntry(std::move(m_activeSculptEntry));
        scene.setModified(true);
    } else {
        sculpt_log_lvl(LogLevel::Debug, "[Undo] Sculpt stroke canceled or no changes detected\n");
        m_activeSculptEntry.reset();
    }

    m_activeMeshDeltaMap.clear();
    s_recordedStamps.clear();
}

void UndoManager::cancelSculptStroke() {
    sculpt_log_lvl(LogLevel::Debug, "[Undo] Sculpt stroke canceled\n");
    m_activeSculptEntry.reset();
    m_activeMeshDeltaMap.clear();
    s_recordedStamps.clear();
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
    entry->before = scene.saveCurrentState(true);
    
    if (operation) {
        operation();
    }

    entry->after = scene.saveCurrentState(true);
    sculpt_log_lvl(LogLevel::Info, "[Undo] Topology change pushed: '%s' (size: %.2f MB)\n",
                   description.c_str(), entry->getMemoryUsage() / (1024.0f * 1024.0f));
    pushEntry(std::move(entry));
    scene.setModified(true);
}

void UndoManager::pushTopologyChange(Scene& scene,
                                      const std::string& description,
                                      HistoryState beforeState,
                                      HistoryState afterState) {
    auto entry = std::make_unique<TopologyUndoEntry>();
    entry->description = description;
    entry->before = std::move(beforeState);
    entry->after = std::move(afterState);
    sculpt_log_lvl(LogLevel::Info, "[Undo] Topology change pushed: '%s' (size: %.2f MB)\n",
                   description.c_str(), entry->getMemoryUsage() / (1024.0f * 1024.0f));
    pushEntry(std::move(entry));
    scene.setModified(true);
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
    scene.setModified(true);
}

void UndoManager::pushLegacyState(Scene& scene, const std::string& description) {
    auto entry = std::make_unique<TopologyUndoEntry>();
    entry->description = description;
    entry->before = scene.saveCurrentState(false);
    entry->after = entry->before;
    sculpt_log_lvl(LogLevel::Info, "[Undo] Legacy/Transform change pushed: '%s' (size: %.2f MB)\n",
                   description.c_str(), entry->getMemoryUsage() / (1024.0f * 1024.0f));
    pushEntry(std::move(entry));
    scene.setModified(true);
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
    s_recordedStamps.clear();
}

void UndoManager::applyEntry(UndoEntry* entry, Scene& scene, bool isUndo) {
    if (!entry) return;

    auto tStart = std::chrono::high_resolution_clock::now();

    if (entry->getType() == UndoEntryType::Sculpt) {
        auto* e = static_cast<SculptUndoEntry*>(entry);
        for (const auto& delta : e->deltas) {
            Mesh* mesh = scene.getMeshById(delta.meshId);
            if (!mesh) {
                sculpt_log_lvl(LogLevel::Warning, "[Undo] Target mesh ID %u not found for sculpt delta\n", delta.meshId);
                continue;
            }

            const auto& srcVerts = isUndo ? delta.prevVerts : delta.nextVerts;
            const auto& srcColors = isUndo ? delta.prevColors : delta.nextColors;
            const auto& srcMats = isUndo ? delta.prevMaterials : delta.nextMaterials;
            const auto& srcLayerDeltas = isUndo ? delta.prevLayerDeltas : delta.nextLayerDeltas;

            bool restoreLayer = delta.hasLayerDeltas && mesh->isLayerActive();
            Layer* targetLayer = restoreLayer ? mesh->layerStack.getLayer(delta.activeLayerIdx) : nullptr;
            if (restoreLayer && !targetLayer) {
                targetLayer = mesh->layerStack.getActive();
            }

            auto tCopyStart = std::chrono::high_resolution_clock::now();
            size_t count = delta.indices.size();
            for (size_t i = 0; i < count; ++i) {
                uint32_t vi = delta.indices[i];
                if (vi >= (uint32_t)mesh->nbVerts) continue;
                if (i * 3 + 2 >= srcVerts.size()) continue;

                mesh->verts[vi * 3 + 0] = srcVerts[i * 3 + 0];
                mesh->verts[vi * 3 + 1] = srcVerts[i * 3 + 1];
                mesh->verts[vi * 3 + 2] = srcVerts[i * 3 + 2];

                if (targetLayer && i * 3 + 2 < srcLayerDeltas.size() && vi * 3 + 2 < targetLayer->deltaVerts.size()) {
                    targetLayer->deltaVerts[vi * 3 + 0] = srcLayerDeltas[i * 3 + 0];
                    targetLayer->deltaVerts[vi * 3 + 1] = srcLayerDeltas[i * 3 + 1];
                    targetLayer->deltaVerts[vi * 3 + 2] = srcLayerDeltas[i * 3 + 2];
                }

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
            auto tCopyEnd = std::chrono::high_resolution_clock::now();

            if (targetLayer) {
                sculpt_log_lvl(LogLevel::Debug, "[Undo] Restored layer '%s' deltas for %zu vertices (%s)\n",
                               targetLayer->name.c_str(), count, isUndo ? "UNDO" : "REDO");
            }

            if (!delta.indices.empty()) {
                auto [minIt, maxIt] = std::minmax_element(delta.indices.begin(), delta.indices.end());
                mesh->dirtyVertMin = *minIt;
                mesh->dirtyVertMax = *maxIt;
                mesh->isVertexDirty = true;
                if (delta.hasColors) mesh->isColorDirty = true;
                if (delta.hasMaterials) mesh->isMaterialDirty = true;
                mesh->isDirty = true;

                auto tFNormsStart = std::chrono::high_resolution_clock::now();
                updateFaceNormalsAndBoxes(
                    mesh->verts.data(), mesh->nbVerts,
                    mesh->faces.data(), mesh->nbFaces,
                    nullptr, -1,
                    mesh->faceNormals.data(),
                    mesh->faceBoxes.data(),
                    mesh->faceCenters.data()
                );
                auto tVNormsStart = std::chrono::high_resolution_clock::now();

                updateVertexNormals(
                    nullptr, -1, mesh->nbVerts,
                    mesh->vrfStartCount.data(),
                    mesh->vertRingFace.data(),
                    mesh->faceNormals.data(),
                    mesh->normals.data()
                );
                auto tOctreeStart = std::chrono::high_resolution_clock::now();

                mesh->octree.build(
                    mesh->nbVerts, mesh->nbFaces,
                    mesh->faceCenters.data(),
                    mesh->faceBoxes.data(),
                    mesh->verts.data(),
                    mesh->faces.data()
                );
                auto tDone = std::chrono::high_resolution_clock::now();

                double msCopy = std::chrono::duration<double, std::milli>(tCopyEnd - tCopyStart).count();
                double msFNorms = std::chrono::duration<double, std::milli>(tVNormsStart - tFNormsStart).count();
                double msVNorms = std::chrono::duration<double, std::milli>(tOctreeStart - tVNormsStart).count();
                double msOctree = std::chrono::duration<double, std::milli>(tDone - tOctreeStart).count();
                double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();

                sculpt_log_lvl(LogLevel::Info,
                    "[Undo Diagnostics] Sculpt apply (%s): %zu verts restored in %.2f ms (Copy: %.2fms, FaceNorms: %.2fms, VertNorms: %.2fms, OctreeBuild: %.2fms)\n",
                    isUndo ? "UNDO" : "REDO", count, msTotal, msCopy, msFNorms, msVNorms, msOctree);
            }
        }
    } else if (entry->getType() == UndoEntryType::Topology) {
        auto* e = static_cast<TopologyUndoEntry*>(entry);
        sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applying Topology %s: '%s'\n",
                       isUndo ? "UNDO" : "REDO", e->getDescription().c_str());
        scene.restoreState(isUndo ? e->before : e->after);
        auto tDone = std::chrono::high_resolution_clock::now();
        double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();
        sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Topology apply (%s) completed in %.2f ms\n",
                       isUndo ? "UNDO" : "REDO", msTotal);
    } else if (entry->getType() == UndoEntryType::SceneMeta) {
        if (auto* transformEntry = dynamic_cast<TransformUndoEntry*>(entry)) {
            sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applying Transform %s: '%s'\n",
                           isUndo ? "UNDO" : "REDO", transformEntry->getDescription().c_str());

            for (const auto& item : transformEntry->m_meshStates) {
                Mesh* mesh = scene.getMeshById(item.meshId);
                if (!mesh) {
                    sculpt_log_lvl(LogLevel::Warning, "[Undo Diagnostics] Target mesh ID %u not found for transform entry\n", item.meshId);
                    continue;
                }

                auto tStateStart = std::chrono::high_resolution_clock::now();
                const char* modeStr = "PureMatrix";

                if (item.hasVertexDeformation) {
                    modeStr = "VertexDeform";
                    mesh->verts = isUndo ? item.beforeVerts : item.afterVerts;
                    if (!item.beforeNormals.empty() && !item.afterNormals.empty()) {
                        mesh->normals = isUndo ? item.beforeNormals : item.afterNormals;
                    }
                    mesh->matrix = isUndo ? item.beforeMatrix : item.afterMatrix;
                    mesh->vertProxy = mesh->verts;
                    mesh->invalidateLocalRadius();
                    mesh->bumpVertsGeneration();

                    if (!mesh->faceNormals.empty() && !mesh->faceBoxes.empty() && !mesh->faceCenters.empty()) {
                        updateFaceNormalsAndBoxes(
                            mesh->verts.data(), mesh->nbVerts,
                            mesh->faces.data(), mesh->nbFaces,
                            nullptr, -1,
                            mesh->faceNormals.data(),
                            mesh->faceBoxes.data(),
                            mesh->faceCenters.data()
                        );
                        if (!mesh->vrfStartCount.empty() && !mesh->vertRingFace.empty()) {
                            updateVertexNormals(
                                nullptr, -1, mesh->nbVerts,
                                mesh->vrfStartCount.data(),
                                mesh->vertRingFace.data(),
                                mesh->faceNormals.data(),
                                mesh->normals.data()
                            );
                        }
                        mesh->octree.update(
                            mesh->verts.data(), mesh->nbVerts,
                            mesh->faces.data(), mesh->nbFaces,
                            mesh->faceBoxes.data(),
                            nullptr, -1
                        );
                    } else {
                        mesh->postInit();
                    }

                    mesh->dirtyVertMin = 0;
                    mesh->dirtyVertMax = std::max(0, mesh->nbVerts - 1);
                    mesh->isVertexDirty = true;
                } else if (item.bakedScale) {
                    modeStr = "BakedScale";
                    float sx = isUndo ? (1.0f / item.scaleX) : item.scaleX;
                    float sy = isUndo ? (1.0f / item.scaleY) : item.scaleY;
                    float sz = isUndo ? (1.0f / item.scaleZ) : item.scaleZ;

                    int n = mesh->nbVerts;
#pragma omp parallel for schedule(static) if(n > 2000)
                    for (int i = 0; i < n; ++i) {
                        mesh->verts[i * 3]     *= sx;
                        mesh->verts[i * 3 + 1] *= sy;
                        mesh->verts[i * 3 + 2] *= sz;
                    }
                    mesh->bumpVertsGeneration();
                    mesh->scaleFaceNormalsBoxesCentersAndOctree(sx, sy, sz);
                    mesh->matrix = isUndo ? item.beforeMatrix : item.afterMatrix;

                    mesh->dirtyVertMin = 0;
                    mesh->dirtyVertMax = std::max(0, mesh->nbVerts - 1);
                    mesh->isVertexDirty = true;
                } else {
                    modeStr = "PureMatrix";
                    mesh->matrix = isUndo ? item.beforeMatrix : item.afterMatrix;
                    // Matrix changed - no vertex VBO upload needed!
                }

                auto tStateDone = std::chrono::high_resolution_clock::now();
                double msState = std::chrono::duration<double, std::milli>(tStateDone - tStateStart).count();
                sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applied Transform (%s) | Mesh ID: %u | Mode: %s | Verts: %d | Time: %.2fms\n",
                               isUndo ? "UNDO" : "REDO", item.meshId, modeStr, mesh->nbVerts, msState);
            }

            auto tDone = std::chrono::high_resolution_clock::now();
            double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();
            sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Transform apply (%s) completed in %.2f ms\n",
                           isUndo ? "UNDO" : "REDO", msTotal);
        } else if (auto* e = dynamic_cast<SceneMetaUndoEntry*>(entry)) {
            sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applying SceneMeta %s: '%s'\n",
                           isUndo ? "UNDO" : "REDO", e->getDescription().c_str());
            scene.restoreState(isUndo ? e->before : e->after);
            auto tDone = std::chrono::high_resolution_clock::now();
            double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();
            sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] SceneMeta apply (%s) completed in %.2f ms\n",
                           isUndo ? "UNDO" : "REDO", msTotal);
        }
    }
}

void UndoManager::pushTransformChange(Scene& scene, TransformUndoEntry entry) {
    if (m_activeSculptEntry) {
        endSculptStroke(scene);
    }
    size_t memBytes = entry.getMemoryUsage();
    uint32_t targetMeshId = 0;
    bool hasDeform = false, hasScale = false;
    for (const auto& s : entry.m_meshStates) {
        if (s.hasVertexDeformation) hasDeform = true;
        if (s.bakedScale) hasScale = true;
        targetMeshId = s.meshId;
    }
    sculpt_log_lvl(LogLevel::Info, "[Undo] Transform change pushed: '%s' (MeshID: %u, Deform: %d, Scale: %d, size: %.2f KB)\n",
                   entry.getDescription().c_str(), targetMeshId, hasDeform ? 1 : 0, hasScale ? 1 : 0, memBytes / 1024.0f);
    pushEntry(std::make_unique<TransformUndoEntry>(std::move(entry)));
    scene.setModified(true);
}


void UndoManager::undo(Scene& scene) {
    if (m_activeSculptEntry) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Undo requested while active sculpt stroke in progress. Canceling active stroke.\n");
        cancelSculptStroke();
    }

    if (m_undoStack.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Nothing to undo! (Undo stack empty, Redo stack size: %zu)\n", m_redoStack.size());
        return;
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    auto entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    sculpt_log_lvl(LogLevel::Info, "[Undo] START UNDO: '%s' (Type: %s, UndoStack size left: %zu, Total Mem: %.2f MB)\n",
                   entry->getDescription().c_str(),
                   entry->getType() == UndoEntryType::Sculpt ? "Sculpt" :
                   (entry->getType() == UndoEntryType::Topology ? "Topology" : "SceneMeta"),
                   m_undoStack.size(), getTotalMemoryUsage() / (1024.0f * 1024.0f));
    applyEntry(entry.get(), scene, true);

    m_redoStack.push_back(std::move(entry));
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log_lvl(LogLevel::Info, "[Undo] FINISHED UNDO in %.2f ms\n", msTotal);
}

void UndoManager::redo(Scene& scene) {
    if (m_activeSculptEntry) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Redo requested while active sculpt stroke in progress. Canceling active stroke.\n");
        cancelSculptStroke();
    }

    if (m_redoStack.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[Undo] Nothing to redo! (Redo stack empty, Undo stack size: %zu)\n", m_undoStack.size());
        return;
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    auto entry = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    sculpt_log_lvl(LogLevel::Info, "[Undo] START REDO: '%s' (Type: %s, RedoStack size left: %zu, Total Mem: %.2f MB)\n",
                   entry->getDescription().c_str(),
                   entry->getType() == UndoEntryType::Sculpt ? "Sculpt" :
                   (entry->getType() == UndoEntryType::Topology ? "Topology" : "SceneMeta"),
                   m_redoStack.size(), getTotalMemoryUsage() / (1024.0f * 1024.0f));
    applyEntry(entry.get(), scene, false);

    m_undoStack.push_back(std::move(entry));
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log_lvl(LogLevel::Info, "[Undo] FINISHED REDO in %.2f ms\n", msTotal);
}

