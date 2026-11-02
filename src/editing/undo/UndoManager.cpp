#include "editing/undo/UndoManager.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "common/Logger.h"
#include "editing/SculptManager.h"
#include "sculpt/SculptEngine.h"

#if __has_include("mesh/BVH.h")
#define UNDO_HAS_BVH 1
#include "mesh/BVH.h"
#else
#define UNDO_HAS_BVH 0
#endif

#include <algorithm>
#include <unordered_set>
#include <cstring>
#include <omp.h>

UndoManager g_undoManager;

namespace {
    // Helper to keep track of recorded vertex indices per mesh during an active stroke using fast array stamps
    static std::unordered_map<uint32_t, std::vector<uint32_t>> s_recordedStamps;
    static uint32_t s_recordedCurrentStamp = 0;

    // Fast face lookup caches for incremental undo/redo normal and spatial structure updates
    static std::vector<uint32_t> s_undoIFaces;
    static std::vector<uint32_t> s_undoTagFlags;
    static uint32_t s_undoTagEpoch = 0;
}

UndoManager::UndoManager() {}

void UndoManager::beginSculptStroke(Scene& scene,
                                     uint32_t meshId,
                                     const std::vector<uint32_t>& affectedVerts,
                                     bool affectsPositions,
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
    recordAffectedVertices(scene, meshId, affectedVerts, affectsPositions, affectsColors, affectsMaterials);
}

void UndoManager::recordAffectedVertices(Scene& scene,
                                         uint32_t meshId,
                                         const std::vector<uint32_t>& affectedVerts,
                                         bool affectsPositions,
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
        newDelta.hasPositions = affectsPositions;
        newDelta.hasColors = affectsColors;
        newDelta.hasMaterials = affectsMaterials;
        m_activeSculptEntry->deltas.push_back(newDelta);
        deltaIdx = m_activeSculptEntry->deltas.size() - 1;
        m_activeMeshDeltaMap[meshId] = deltaIdx;
    } else {
        deltaIdx = mapIt->second;
    }

    VertexDelta& delta = m_activeSculptEntry->deltas[deltaIdx];
    if (affectsPositions) delta.hasPositions = true;
    if (affectsColors) delta.hasColors = true;
    if (affectsMaterials) delta.hasMaterials = true;

    auto& stamps = s_recordedStamps[meshId];
    if (stamps.size() < static_cast<size_t>(mesh->nbVerts)) {
        auto tAlloc0 = std::chrono::high_resolution_clock::now();
        stamps.resize(mesh->nbVerts, 0);
        auto tAlloc1 = std::chrono::high_resolution_clock::now();
        double allocMs = std::chrono::duration<double, std::milli>(tAlloc1 - tAlloc0).count();
        sculpt_log("[Undo Diagnostics] stamps.resize(%d) allocated %.2f MB in %.2f ms\n",
                   mesh->nbVerts, (mesh->nbVerts * sizeof(uint32_t)) / (1024.0 * 1024.0), allocMs);
    }
    uint32_t currentStamp = s_recordedCurrentStamp;

    Layer* activeLayer = mesh->isLayerRecording() ? mesh->layerStack.getActive() : nullptr;
    int activeLayerIdx = mesh->isLayerRecording() ? mesh->layerStack.getActiveIdx() : -1;
    if (activeLayer) {
        delta.hasLayerDeltas = true;
        delta.activeLayerIdx = activeLayerIdx;
    }

    size_t count = affectedVerts.size();
    if (count < 2048) {
        if (delta.indices.capacity() < delta.indices.size() + count) {
            delta.indices.reserve(delta.indices.size() + count);
        }
        if (delta.hasPositions && delta.prevVerts.capacity() < delta.prevVerts.size() + count * 3) {
            delta.prevVerts.reserve(delta.prevVerts.size() + count * 3);
        }
        if (activeLayer && delta.prevLayerDeltas.capacity() < delta.prevLayerDeltas.size() + count * 3) {
            delta.prevLayerDeltas.reserve(delta.prevLayerDeltas.size() + count * 3);
        }
        if (affectsColors && !mesh->colors.empty() && delta.prevColors.capacity() < delta.prevColors.size() + count * 3) {
            delta.prevColors.reserve(delta.prevColors.size() + count * 3);
        }
        if (affectsMaterials && !mesh->materials.empty() && delta.prevMaterials.capacity() < delta.prevMaterials.size() + count * 3) {
            delta.prevMaterials.reserve(delta.prevMaterials.size() + count * 3);
        }

        for (uint32_t v : affectedVerts) {
            if (v >= (uint32_t)mesh->nbVerts) continue;
            if (stamps[v] != currentStamp) {
                stamps[v] = currentStamp;
                // First time this vertex is recorded in this stroke
                delta.indices.push_back(v);
                if (delta.hasPositions) {
                    delta.prevVerts.push_back(mesh->verts[v * 3 + 0]);
                    delta.prevVerts.push_back(mesh->verts[v * 3 + 1]);
                    delta.prevVerts.push_back(mesh->verts[v * 3 + 2]);
                }

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
    } else {
        int maxThreads = omp_get_max_threads();
        static std::vector<std::vector<uint32_t>> s_threadNewVerts;
        if ((int)s_threadNewVerts.size() < maxThreads) {
            s_threadNewVerts.resize(maxThreads);
        }

        size_t estCap = (count / maxThreads) + 64;
        #pragma omp parallel for schedule(static)
        for (int t = 0; t < maxThreads; ++t) {
            if (s_threadNewVerts[t].size() < estCap) {
                s_threadNewVerts[t].resize(estCap);
            }
        }

        uint32_t* stampPtr = stamps.data();
        uint32_t curStamp = currentStamp;
        uint32_t maxLimit = (uint32_t)mesh->nbVerts;
        const uint32_t* affPtr = affectedVerts.data();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            uint32_t* thPtr = s_threadNewVerts[tid].data();
            size_t thCount = 0;

            #pragma omp for schedule(static)
            for (int64_t i = 0; i < (int64_t)count; ++i) {
                if (i + 8 < (int64_t)count) {
                    __builtin_prefetch(&stampPtr[affPtr[i + 8]], 1, 1);
                }
                uint32_t v = affPtr[i];
                if (v >= maxLimit) continue;
                if (stampPtr[v] != curStamp) {
                    if (__atomic_exchange_n(&stampPtr[v], curStamp, __ATOMIC_RELAXED) != curStamp) {
                        thPtr[thCount++] = v;
                    }
                }
            }
            s_threadNewVerts[tid].resize(thCount);
        }

        size_t totalNew = 0;
        for (int t = 0; t < maxThreads; ++t) {
            totalNew += s_threadNewVerts[t].size();
        }

        if (totalNew > 0) {
            size_t baseIdx = delta.indices.size();
            delta.indices.resize(baseIdx + totalNew);
            if (delta.hasPositions) delta.prevVerts.resize((baseIdx + totalNew) * 3);
            if (activeLayer) delta.prevLayerDeltas.resize((baseIdx + totalNew) * 3);
            if (affectsColors && !mesh->colors.empty()) delta.prevColors.resize((baseIdx + totalNew) * 3);
            if (affectsMaterials && !mesh->materials.empty()) delta.prevMaterials.resize((baseIdx + totalNew) * 3);

            std::vector<size_t> thOffsets(maxThreads + 1, baseIdx);
            for (int t = 0; t < maxThreads; ++t) {
                thOffsets[t + 1] = thOffsets[t] + s_threadNewVerts[t].size();
            }
            #pragma omp parallel for schedule(static)
            for (int t = 0; t < maxThreads; ++t) {
                size_t num = s_threadNewVerts[t].size();
                if (num > 0) {
                    std::memcpy(delta.indices.data() + thOffsets[t], s_threadNewVerts[t].data(), num * sizeof(uint32_t));
                }
            }

            const float* vertsData = mesh->verts.data();
            const float* colorsData = mesh->colors.data();
            const float* matsData = mesh->materials.data();
            const float* layerData = activeLayer ? activeLayer->deltaVerts.data() : nullptr;
            size_t layerSize = activeLayer ? activeLayer->deltaVerts.size() : 0;
            bool hasPos = delta.hasPositions;
            bool hasCol = affectsColors && !mesh->colors.empty();
            bool hasMat = affectsMaterials && !mesh->materials.empty();
            const uint32_t* indData = delta.indices.data();
            float* pVerts = delta.prevVerts.data();
            float* pColors = delta.prevColors.data();
            float* pMats = delta.prevMaterials.data();
            float* pLayer = delta.prevLayerDeltas.data();

            #pragma omp parallel for schedule(static) if(totalNew > 1000)
            for (int64_t k = (int64_t)baseIdx; k < (int64_t)(baseIdx + totalNew); ++k) {
                uint32_t v = indData[k];
                uint32_t v3 = v * 3;
                uint32_t k3 = (uint32_t)(k * 3);
                if (hasPos) {
                    pVerts[k3 + 0] = vertsData[v3 + 0];
                    pVerts[k3 + 1] = vertsData[v3 + 1];
                    pVerts[k3 + 2] = vertsData[v3 + 2];
                }
                if (activeLayer) {
                    if (v3 + 2 < layerSize) {
                        pLayer[k3 + 0] = layerData[v3 + 0];
                        pLayer[k3 + 1] = layerData[v3 + 1];
                        pLayer[k3 + 2] = layerData[v3 + 2];
                    } else {
                        pLayer[k3 + 0] = 0.0f;
                        pLayer[k3 + 1] = 0.0f;
                        pLayer[k3 + 2] = 0.0f;
                    }
                }
                if (hasCol) {
                    pColors[k3 + 0] = colorsData[v3 + 0];
                    pColors[k3 + 1] = colorsData[v3 + 1];
                    pColors[k3 + 2] = colorsData[v3 + 2];
                }
                if (hasMat) {
                    pMats[k3 + 0] = matsData[v3 + 0];
                    pMats[k3 + 1] = matsData[v3 + 1];
                    pMats[k3 + 2] = matsData[v3 + 2];
                }
            }
        }
    }
}

void UndoManager::warmupForMesh(uint32_t meshId, int nbVerts) {
    if (nbVerts <= 0) return;
    auto& stamps = s_recordedStamps[meshId];
    if (stamps.size() < static_cast<size_t>(nbVerts)) {
        auto tAlloc0 = std::chrono::high_resolution_clock::now();
        stamps.resize(nbVerts, 0);
        auto tAlloc1 = std::chrono::high_resolution_clock::now();
        double allocMs = std::chrono::duration<double, std::milli>(tAlloc1 - tAlloc0).count();
        sculpt_log("[Undo Diagnostics] warmupForMesh(%d) pre-allocated %.2f MB in %.2f ms\n",
                   nbVerts, (nbVerts * sizeof(uint32_t)) / (1024.0 * 1024.0), allocMs);
    }
}

void UndoManager::endSculptStroke(Scene& scene) {
    if (!m_activeSculptEntry) return;

    bool hasAnyChange = false;
    size_t totalRecordedVerts = 0;
    for (auto& delta : m_activeSculptEntry->deltas) {
        Mesh* mesh = scene.getMeshById(delta.meshId);
        if (!mesh) continue;

        size_t nIndices = delta.indices.size();
        totalRecordedVerts += nIndices;
        if (delta.hasPositions) {
            delta.nextVerts.resize(nIndices * 3);
        }
        if (delta.hasColors && !mesh->colors.empty()) {
            delta.nextColors.resize(nIndices * 3);
        }
        if (delta.hasMaterials && !mesh->materials.empty()) {
            delta.nextMaterials.resize(nIndices * 3);
        }

        Layer* activeLayer = (delta.hasLayerDeltas && mesh->hasLayers()) ? mesh->layerStack.getLayer(delta.activeLayerIdx) : nullptr;
        if (!activeLayer && delta.hasLayerDeltas && mesh->hasLayers()) {
            activeLayer = mesh->layerStack.getActive();
        }
        if (activeLayer) {
            delta.nextLayerDeltas.resize(nIndices * 3);
        }

        const uint32_t* indPtr = delta.indices.data();
        const float* vertsPtr = mesh->verts.data();
        const float* colorsPtr = mesh->colors.data();
        const float* matsPtr = mesh->materials.data();
        const float* layerPtr = activeLayer ? activeLayer->deltaVerts.data() : nullptr;
        size_t layerSize = activeLayer ? activeLayer->deltaVerts.size() : 0;
        bool hasPos = delta.hasPositions;
        bool hasCol = delta.hasColors && !mesh->colors.empty();
        bool hasMat = delta.hasMaterials && !mesh->materials.empty();
        float* nVerts = delta.nextVerts.data();
        float* nCols = delta.nextColors.data();
        float* nMats = delta.nextMaterials.data();
        float* nLayers = delta.nextLayerDeltas.data();

        const float* pVerts = delta.prevVerts.data();
        const float* pCols = delta.prevColors.data();
        const float* pMats = delta.prevMaterials.data();
        const float* pLayers = delta.prevLayerDeltas.data();
        bool deltaChanged = false;

        #pragma omp parallel for schedule(static) reduction(||:deltaChanged) if(nIndices > 1000)
        for (int64_t i = 0; i < (int64_t)nIndices; ++i) {
            uint32_t v = indPtr[i];
            uint32_t v3 = v * 3;
            uint32_t i3 = (uint32_t)(i * 3);
            if (hasPos) {
                float x = vertsPtr[v3 + 0], y = vertsPtr[v3 + 1], z = vertsPtr[v3 + 2];
                nVerts[i3 + 0] = x; nVerts[i3 + 1] = y; nVerts[i3 + 2] = z;
                if (!deltaChanged && (x != pVerts[i3 + 0] || y != pVerts[i3 + 1] || z != pVerts[i3 + 2])) {
                    deltaChanged = true;
                }
            }
            if (activeLayer) {
                float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                if (v3 + 2 < layerSize) {
                    lx = layerPtr[v3 + 0]; ly = layerPtr[v3 + 1]; lz = layerPtr[v3 + 2];
                }
                nLayers[i3 + 0] = lx; nLayers[i3 + 1] = ly; nLayers[i3 + 2] = lz;
                if (!deltaChanged && (lx != pLayers[i3 + 0] || ly != pLayers[i3 + 1] || lz != pLayers[i3 + 2])) {
                    deltaChanged = true;
                }
            }
            if (hasCol) {
                float cr = colorsPtr[v3 + 0], cg = colorsPtr[v3 + 1], cb = colorsPtr[v3 + 2];
                nCols[i3 + 0] = cr; nCols[i3 + 1] = cg; nCols[i3 + 2] = cb;
                if (!deltaChanged && (cr != pCols[i3 + 0] || cg != pCols[i3 + 1] || cb != pCols[i3 + 2])) {
                    deltaChanged = true;
                }
            }
            if (hasMat) {
                float mr = matsPtr[v3 + 0], mm = matsPtr[v3 + 1], ms = matsPtr[v3 + 2];
                nMats[i3 + 0] = mr; nMats[i3 + 1] = mm; nMats[i3 + 2] = ms;
                if (!deltaChanged && (mr != pMats[i3 + 0] || mm != pMats[i3 + 1] || ms != pMats[i3 + 2])) {
                    deltaChanged = true;
                }
            }
        }

        if (deltaChanged) {
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
}

void UndoManager::cancelSculptStroke() {
    sculpt_log_lvl(LogLevel::Debug, "[Undo] Sculpt stroke canceled\n");
    m_activeSculptEntry.reset();
    m_activeMeshDeltaMap.clear();
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

    beginSculptStroke(scene, meshId, allVerts, true, affectsColors, affectsMaterials, description);
    operation();
    endSculptStroke(scene);
}

void UndoManager::pushTopologyChange(Scene& scene,
                                      const std::string& description,
                                      std::function<void()> operation) {
    auto entry = std::make_unique<TopologyUndoEntry>();
    entry->description = description;
    HistoryState beforeState = scene.saveCurrentState(true);
    
    if (operation) {
        operation();
    }

    HistoryState afterState = scene.saveCurrentState(true);
    entry->setStates(beforeState, afterState);
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
    entry->setStates(beforeState, afterState);
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

void UndoManager::pushMeasurementChange(Scene& scene,
                                        const std::string& description,
                                        std::function<void()> operation) {
    SculptManager* sculpt = scene.getSculptManager();
    if (!sculpt) sculpt = m_sculptManager;
    if (!sculpt) {
        if (operation) operation();
        return;
    }

    auto entry = std::make_unique<MeasurementUndoEntry>();
    entry->description = description;
    entry->beforeMeasure = sculpt->getMeasureSegments();
    entry->beforeDivider = sculpt->getDividerSegments();

    if (operation) {
        operation();
    }

    entry->afterMeasure = sculpt->getMeasureSegments();
    entry->afterDivider = sculpt->getDividerSegments();

    sculpt_log_lvl(LogLevel::Info, "[Undo] Measurement change pushed: '%s'\n", description.c_str());
    pushEntry(std::move(entry));
    scene.setModified(true);
}


void UndoManager::pushLegacyState(Scene& scene, const std::string& description) {
    auto entry = std::make_unique<TopologyUndoEntry>();
    entry->description = description;
    HistoryState s = scene.saveCurrentState(false);
    entry->setStates(s, s);
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

            bool restoreLayer = delta.hasLayerDeltas && mesh->hasLayers();
            Layer* targetLayer = restoreLayer ? mesh->layerStack.getLayer(delta.activeLayerIdx) : nullptr;
            if (restoreLayer && !targetLayer) {
                targetLayer = mesh->layerStack.getActive();
            }

            auto tCopyStart = std::chrono::high_resolution_clock::now();
            size_t count = delta.indices.size();
            bool vertsMoved = false;
            for (size_t i = 0; i < count; ++i) {
                uint32_t vi = delta.indices[i];
                if (vi >= (uint32_t)mesh->nbVerts) continue;

                if (delta.hasPositions && i * 3 + 2 < srcVerts.size()) {
                    float nx = srcVerts[i * 3 + 0];
                    float ny = srcVerts[i * 3 + 1];
                    float nz = srcVerts[i * 3 + 2];
                    if (mesh->verts[vi * 3 + 0] != nx ||
                        mesh->verts[vi * 3 + 1] != ny ||
                        mesh->verts[vi * 3 + 2] != nz) {
                        vertsMoved = true;
                        mesh->verts[vi * 3 + 0] = nx;
                        mesh->verts[vi * 3 + 1] = ny;
                        mesh->verts[vi * 3 + 2] = nz;
                    }
                    if (vi * 3 + 2 < mesh->vertProxy.size()) {
                        mesh->vertProxy[vi * 3 + 0] = nx;
                        mesh->vertProxy[vi * 3 + 1] = ny;
                        mesh->vertProxy[vi * 3 + 2] = nz;
                    }
                }

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
                if (mesh->isVertexDirty || mesh->isColorDirty || mesh->isMaterialDirty) {
                    mesh->dirtyVertMin = std::min(mesh->dirtyVertMin, *minIt);
                    mesh->dirtyVertMax = std::max(mesh->dirtyVertMax, *maxIt);
                } else {
                    mesh->dirtyVertMin = *minIt;
                    mesh->dirtyVertMax = *maxIt;
                }
                if (vertsMoved) mesh->isVertexDirty = true;
                if (delta.hasColors) mesh->isColorDirty = true;
                if (delta.hasMaterials) mesh->isMaterialDirty = true;
                mesh->isDirty = true;

                if (!vertsMoved) {
                    auto tDone = std::chrono::high_resolution_clock::now();
                    double msCopy = std::chrono::duration<double, std::milli>(tCopyEnd - tCopyStart).count();
                    double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();
                    sculpt_log_lvl(LogLevel::Info,
                        "[Undo Diagnostics] Sculpt apply (%s): %zu verts restored in %.2f ms (Copy: %.2fms, FaceNorms: 0.00ms, VertNorms: 0.00ms, OctreeBuild: 0.00ms)\n",
                        isUndo ? "UNDO" : "REDO", count, msTotal, msCopy);
                    continue;
                }

                // Incremental update: find unique affected faces around restored vertices
                uint32_t numIFaces = 0;
                auto tFaceLookupStart = std::chrono::high_resolution_clock::now();
                if (!mesh->vrfStartCount.empty() && !mesh->vertRingFace.empty() && mesh->nbFaces > 0) {
                    if (s_undoTagFlags.size() < (size_t)mesh->nbFaces) {
                        s_undoTagFlags.assign(mesh->nbFaces, 0);
                        s_undoTagEpoch = 1;
                    }
                    if (s_undoIFaces.size() < (size_t)mesh->nbFaces) {
                        s_undoIFaces.resize(mesh->nbFaces);
                    }
                    numIFaces = getFacesFromVerticesFast(
                        delta.indices.data(),
                        (uint32_t)delta.indices.size(),
                        mesh->vrfStartCount.data(),
                        mesh->vertRingFace.data(),
                        s_undoIFaces.data(),
                        s_undoTagFlags.data(),
                        &s_undoTagEpoch,
                        mesh->nbFaces
                    );
                }
                auto tFaceLookupEnd = std::chrono::high_resolution_clock::now();

                auto tFNormsStart = std::chrono::high_resolution_clock::now();
                if (numIFaces > 0 && !mesh->faceNormals.empty() && !mesh->faceBoxes.empty() && !mesh->faceCenters.empty()) {
                    updateFaceNormalsAndBoxes(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        s_undoIFaces.data(), (int)numIFaces,
                        mesh->faceNormals.data(),
                        mesh->faceBoxes.data(),
                        mesh->faceCenters.data()
                    );
                } else if (numIFaces == 0 && mesh->nbFaces > 0 && !mesh->faceNormals.empty()) {
                    updateFaceNormalsAndBoxes(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        nullptr, -1,
                        mesh->faceNormals.data(),
                        mesh->faceBoxes.data(),
                        mesh->faceCenters.data()
                    );
                }
                auto tVNormsStart = std::chrono::high_resolution_clock::now();

                if (!mesh->normals.empty() && !mesh->vrfStartCount.empty() && !mesh->vertRingFace.empty()) {
                    updateVertexNormals(
                        delta.indices.data(), (int)delta.indices.size(), mesh->nbVerts,
                        mesh->vrfStartCount.data(),
                        mesh->vertRingFace.data(),
                        mesh->faceNormals.data(),
                        mesh->normals.data()
                    );
                } else if (!mesh->normals.empty()) {
                    updateVertexNormals(
                        nullptr, -1, mesh->nbVerts,
                        mesh->vrfStartCount.data(),
                        mesh->vertRingFace.data(),
                        mesh->faceNormals.data(),
                        mesh->normals.data()
                    );
                }
                auto tSpatialStart = std::chrono::high_resolution_clock::now();

#if UNDO_HAS_BVH
                if (numIFaces > 0) {
                    mesh->bvh.refit(*mesh, s_undoIFaces.data(), numIFaces);
                } else {
                    mesh->bvh.refitAll(*mesh);
                }
#else
                if (numIFaces > 0 && !mesh->faceBoxes.empty()) {
                    mesh->octree.update(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        mesh->faceBoxes.data(),
                        s_undoIFaces.data(), (int)numIFaces
                    );
                } else if (!mesh->faceBoxes.empty()) {
                    mesh->octree.update(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        mesh->faceBoxes.data(),
                        nullptr, -1
                    );
                }
#endif
                auto tDone = std::chrono::high_resolution_clock::now();

                double msCopy = std::chrono::duration<double, std::milli>(tCopyEnd - tCopyStart).count();
                double msFaceLookup = std::chrono::duration<double, std::milli>(tFaceLookupEnd - tFaceLookupStart).count();
                double msFNorms = std::chrono::duration<double, std::milli>(tVNormsStart - tFNormsStart).count();
                double msVNorms = std::chrono::duration<double, std::milli>(tSpatialStart - tVNormsStart).count();
                double msOctree = std::chrono::duration<double, std::milli>(tDone - tSpatialStart).count();
                double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();

                sculpt_log_lvl(LogLevel::Info,
                    "[Undo Diagnostics] Sculpt apply (%s): %zu verts restored in %.2f ms (Copy: %.2fms, FaceLookup: %.2fms, FaceNorms: %.2fms, VertNorms: %.2fms, OctreeBuild: %.2fms)\n",
                    isUndo ? "UNDO" : "REDO", count, msTotal, msCopy, msFaceLookup, msFNorms, msVNorms, msOctree);
            }
        }
    } else if (entry->getType() == UndoEntryType::Topology) {
        auto* e = static_cast<TopologyUndoEntry*>(entry);
        sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applying Topology %s: '%s'\n",
                       isUndo ? "UNDO" : "REDO", e->getDescription().c_str());
        scene.restoreState(isUndo ? e->getBeforeState() : e->getAfterState());
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
                double msFNorms = 0.0, msVNorms = 0.0, msOctree = 0.0;

                if (item.hasVertexDeformation) {
                    modeStr = item.isSparse ? "SparseVertexDeform" : "VertexDeform";
                    if (item.isSparse) {
                        const float* src = isUndo ? item.sparseBeforeCoords.data() : item.sparseAfterCoords.data();
                        int numSparse = (int)item.sparseVertIndices.size();
#pragma omp parallel for schedule(static) if(numSparse > 2000)
                        for (int i = 0; i < numSparse; ++i) {
                            uint32_t v = item.sparseVertIndices[i];
                            uint32_t dstIdx = v * 3;
                            uint32_t srcIdx = i * 3;
                            mesh->verts[dstIdx + 0] = src[srcIdx + 0];
                            mesh->verts[dstIdx + 1] = src[srcIdx + 1];
                            mesh->verts[dstIdx + 2] = src[srcIdx + 2];
                            mesh->vertProxy[dstIdx + 0] = src[srcIdx + 0];
                            mesh->vertProxy[dstIdx + 1] = src[srcIdx + 1];
                            mesh->vertProxy[dstIdx + 2] = src[srcIdx + 2];
                        }
                        mesh->matrix = isUndo ? item.beforeMatrix : item.afterMatrix;
                        mesh->invalidateLocalRadius();
                        mesh->bumpVertsGeneration();

                        if (!item.affectedFaces.empty() && !mesh->faceNormals.empty() && !mesh->faceBoxes.empty() && !mesh->faceCenters.empty()) {
                            auto tFN0 = std::chrono::high_resolution_clock::now();
                            updateFaceNormalsAndBoxes(
                                mesh->verts.data(), mesh->nbVerts,
                                mesh->faces.data(), mesh->nbFaces,
                                item.affectedFaces.data(), (int)item.affectedFaces.size(),
                                mesh->faceNormals.data(),
                                mesh->faceBoxes.data(),
                                mesh->faceCenters.data()
                            );
                            auto tVN0 = std::chrono::high_resolution_clock::now();
                            msFNorms = std::chrono::duration<double, std::milli>(tVN0 - tFN0).count();

                            if (!mesh->vrfStartCount.empty() && !mesh->vertRingFace.empty()) {
                                updateVertexNormals(
                                    item.sparseVertIndices.data(), (int)item.sparseVertIndices.size(), mesh->nbVerts,
                                    mesh->vrfStartCount.data(),
                                    mesh->vertRingFace.data(),
                                    mesh->faceNormals.data(),
                                    mesh->normals.data()
                                );
                            }
                            auto tOct0 = std::chrono::high_resolution_clock::now();
                            msVNorms = std::chrono::duration<double, std::milli>(tOct0 - tVN0).count();

                            mesh->octree.update(
                                mesh->verts.data(), mesh->nbVerts,
                                mesh->faces.data(), mesh->nbFaces,
                                mesh->faceBoxes.data(),
                                item.affectedFaces.data(), (int)item.affectedFaces.size()
                            );
#if UNDO_HAS_BVH
                            mesh->bvh.refit(*mesh, item.affectedFaces.data(), (int)item.affectedFaces.size());
#endif
                            auto tOct1 = std::chrono::high_resolution_clock::now();
                            msOctree = std::chrono::duration<double, std::milli>(tOct1 - tOct0).count();
                        }
                        if (!item.sparseVertIndices.empty()) {
                            mesh->dirtyVertMin = item.sparseVertIndices.front();
                            mesh->dirtyVertMax = item.sparseVertIndices.back();
                        }
                        mesh->isVertexDirty = true;
                    } else {
                        mesh->verts = isUndo ? item.beforeVerts : item.afterVerts;
                        if (!item.beforeNormals.empty() && !item.afterNormals.empty()) {
                            mesh->normals = isUndo ? item.beforeNormals : item.afterNormals;
                        }
                        mesh->matrix = isUndo ? item.beforeMatrix : item.afterMatrix;
                        mesh->vertProxy = mesh->verts;
                        mesh->invalidateLocalRadius();
                        mesh->bumpVertsGeneration();

                        if (!mesh->faceNormals.empty() && !mesh->faceBoxes.empty() && !mesh->faceCenters.empty()) {
                            auto tFN0 = std::chrono::high_resolution_clock::now();
                            updateFaceNormalsAndBoxes(
                                mesh->verts.data(), mesh->nbVerts,
                                mesh->faces.data(), mesh->nbFaces,
                                nullptr, -1,
                                mesh->faceNormals.data(),
                                mesh->faceBoxes.data(),
                                mesh->faceCenters.data()
                            );
                            auto tVN0 = std::chrono::high_resolution_clock::now();
                            msFNorms = std::chrono::duration<double, std::milli>(tVN0 - tFN0).count();
                            if (!mesh->vrfStartCount.empty() && !mesh->vertRingFace.empty()) {
                                updateVertexNormals(
                                    nullptr, -1, mesh->nbVerts,
                                    mesh->vrfStartCount.data(),
                                    mesh->vertRingFace.data(),
                                    mesh->faceNormals.data(),
                                    mesh->normals.data()
                                );
                            }
                            auto tOct0 = std::chrono::high_resolution_clock::now();
                            msVNorms = std::chrono::duration<double, std::milli>(tOct0 - tVN0).count();
                            mesh->octree.update(
                                mesh->verts.data(), mesh->nbVerts,
                                mesh->faces.data(), mesh->nbFaces,
                                mesh->faceBoxes.data(),
                                nullptr, -1
                            );
#if UNDO_HAS_BVH
                            mesh->bvh.refitAll(*mesh);
#endif
                            auto tOct1 = std::chrono::high_resolution_clock::now();
                            msOctree = std::chrono::duration<double, std::milli>(tOct1 - tOct0).count();
                        } else {
                            mesh->postInit();
                        }

                        mesh->dirtyVertMin = 0;
                        mesh->dirtyVertMax = std::max(0, mesh->nbVerts - 1);
                        mesh->isVertexDirty = true;
                    }
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

                if (item.isPrimitive && mesh->isPrimitive && mesh->primitiveData) {
                    mesh->primitiveData->localTransform = isUndo ? item.beforeLocalTransform : item.afterLocalTransform;
                    mesh->primitiveData->centerOffset = glm::vec3(mesh->primitiveData->localTransform[3]);
                }

                auto tStateDone = std::chrono::high_resolution_clock::now();
                double msState = std::chrono::duration<double, std::milli>(tStateDone - tStateStart).count();
                if (item.hasVertexDeformation) {
                    sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applied Transform (%s) | Mesh ID: %u | Mode: %s | Verts: %d | Time: %.2fms (FaceNorms: %.2fms, VertNorms: %.2fms, Octree: %.2fms)\n",
                                   isUndo ? "UNDO" : "REDO", item.meshId, modeStr, mesh->nbVerts, msState, msFNorms, msVNorms, msOctree);
                } else {
                    sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applied Transform (%s) | Mesh ID: %u | Mode: %s | Verts: %d | Time: %.2fms\n",
                                   isUndo ? "UNDO" : "REDO", item.meshId, modeStr, mesh->nbVerts, msState);
                }
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
    } else if (entry->getType() == UndoEntryType::Measurement) {
        auto* e = static_cast<MeasurementUndoEntry*>(entry);
        sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Applying Measurement %s: '%s'\n",
                       isUndo ? "UNDO" : "REDO", e->getDescription().c_str());

        SculptManager* sculpt = scene.getSculptManager();
        if (!sculpt) sculpt = g_undoManager.getSculptManager();
        if (sculpt) {
            sculpt->getMeasureSegments() = isUndo ? e->beforeMeasure : e->afterMeasure;
            sculpt->getDividerSegments() = isUndo ? e->beforeDivider : e->afterDivider;
            sculpt->validateSegments(scene);
        } else {
            sculpt_log_lvl(LogLevel::Warning, "[Undo] SculptManager not available for measurement undo\n");
        }

        auto tDone = std::chrono::high_resolution_clock::now();
        double msTotal = std::chrono::duration<double, std::milli>(tDone - tStart).count();
        sculpt_log_lvl(LogLevel::Info, "[Undo Diagnostics] Measurement apply (%s) completed in %.2f ms\n",
                       isUndo ? "UNDO" : "REDO", msTotal);
    }
}

void UndoManager::pushTransformChange(Scene& scene, TransformUndoEntry&& entry) {
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

void UndoManager::pushTransformChange(Scene& scene, const TransformUndoEntry& entry) {
    TransformUndoEntry copy = entry;
    pushTransformChange(scene, std::move(copy));
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

    const char* typeStr = "SceneMeta";
    if (entry->getType() == UndoEntryType::Sculpt) typeStr = "Sculpt";
    else if (entry->getType() == UndoEntryType::Topology) typeStr = "Topology";
    else if (entry->getType() == UndoEntryType::Measurement) typeStr = "Measurement";

    sculpt_log_lvl(LogLevel::Info, "[Undo] START UNDO: '%s' (Type: %s, UndoStack size left: %zu, Total Mem: %.2f MB)\n",
                   entry->getDescription().c_str(), typeStr,
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

    const char* typeStr = "SceneMeta";
    if (entry->getType() == UndoEntryType::Sculpt) typeStr = "Sculpt";
    else if (entry->getType() == UndoEntryType::Topology) typeStr = "Topology";
    else if (entry->getType() == UndoEntryType::Measurement) typeStr = "Measurement";

    sculpt_log_lvl(LogLevel::Info, "[Undo] START REDO: '%s' (Type: %s, RedoStack size left: %zu, Total Mem: %.2f MB)\n",
                   entry->getDescription().c_str(), typeStr,
                   m_redoStack.size(), getTotalMemoryUsage() / (1024.0f * 1024.0f));
    applyEntry(entry.get(), scene, false);

    m_undoStack.push_back(std::move(entry));
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log_lvl(LogLevel::Info, "[Undo] FINISHED REDO in %.2f ms\n", msTotal);
}


