#include "editing/SelectionManager.h"
#include "mesh/Mesh.h"
#include <algorithm>
#include <cmath>

SelectionManager::SelectionManager()
    : m_mode(ComponentMode::Edge)
{
}

bool SelectionManager::hasSelection() const {
    return !m_selectedVerts.empty() || !m_selectedEdges.empty() || !m_selectedFaces.empty();
}

bool SelectionManager::hasSelectionInMode(ComponentMode mode) const {
    switch (mode) {
        case ComponentMode::Vertex: return !m_selectedVerts.empty();
        case ComponentMode::Edge:   return !m_selectedEdges.empty();
        case ComponentMode::Face:   return !m_selectedFaces.empty();
    }
    return false;
}

bool SelectionManager::isVertexSelected(uint32_t vertIdx) const {
    return m_selectedVerts.count(vertIdx) > 0;
}

bool SelectionManager::isEdgeSelected(uint32_t edgeIdx) const {
    return m_selectedEdges.count(edgeIdx) > 0;
}

bool SelectionManager::isFaceSelected(uint32_t faceIdx) const {
    return m_selectedFaces.count(faceIdx) > 0;
}

void SelectionManager::selectVertex(uint32_t vertIdx, bool add) {
    if (!add) {
        m_selectedVerts.clear();
    }
    m_selectedVerts.insert(vertIdx);
}

void SelectionManager::deselectVertex(uint32_t vertIdx) {
    m_selectedVerts.erase(vertIdx);
}

void SelectionManager::toggleVertex(uint32_t vertIdx) {
    if (m_selectedVerts.count(vertIdx)) {
        m_selectedVerts.erase(vertIdx);
    } else {
        m_selectedVerts.insert(vertIdx);
    }
}

void SelectionManager::selectEdge(uint32_t edgeIdx, bool add) {
    if (!add) {
        m_selectedEdges.clear();
    }
    m_selectedEdges.insert(edgeIdx);
}

void SelectionManager::deselectEdge(uint32_t edgeIdx) {
    m_selectedEdges.erase(edgeIdx);
}

void SelectionManager::toggleEdge(uint32_t edgeIdx) {
    if (m_selectedEdges.count(edgeIdx)) {
        m_selectedEdges.erase(edgeIdx);
    } else {
        m_selectedEdges.insert(edgeIdx);
    }
}

void SelectionManager::selectFace(uint32_t faceIdx, bool add) {
    if (!add) {
        m_selectedFaces.clear();
    }
    m_selectedFaces.insert(faceIdx);
}

void SelectionManager::deselectFace(uint32_t faceIdx) {
    m_selectedFaces.erase(faceIdx);
}

void SelectionManager::toggleFace(uint32_t faceIdx) {
    if (m_selectedFaces.count(faceIdx)) {
        m_selectedFaces.erase(faceIdx);
    } else {
        m_selectedFaces.insert(faceIdx);
    }
}

void SelectionManager::clear() {
    m_selectedVerts.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();
}

void SelectionManager::clearMode(ComponentMode mode) {
    switch (mode) {
        case ComponentMode::Vertex: m_selectedVerts.clear(); break;
        case ComponentMode::Edge:   m_selectedEdges.clear(); break;
        case ComponentMode::Face:   m_selectedFaces.clear(); break;
    }
}

void SelectionManager::selectAll(const Mesh& mesh) {
    switch (m_mode) {
        case ComponentMode::Vertex: {
            m_selectedVerts.clear();
            for (int i = 0; i < mesh.nbVerts; ++i) {
                m_selectedVerts.insert((uint32_t)i);
            }
            break;
        }
        case ComponentMode::Edge: {
            m_selectedEdges.clear();
            for (int i = 0; i < mesh.nbEdges; ++i) {
                m_selectedEdges.insert((uint32_t)i);
            }
            break;
        }
        case ComponentMode::Face: {
            m_selectedFaces.clear();
            for (int i = 0; i < mesh.nbFaces; ++i) {
                m_selectedFaces.insert((uint32_t)i);
            }
            break;
        }
    }
}

void SelectionManager::invertSelection(const Mesh& mesh) {
    switch (m_mode) {
        case ComponentMode::Vertex: {
            std::unordered_set<uint32_t> newSel;
            for (int i = 0; i < mesh.nbVerts; ++i) {
                uint32_t u = (uint32_t)i;
                if (!m_selectedVerts.count(u)) newSel.insert(u);
            }
            m_selectedVerts = std::move(newSel);
            break;
        }
        case ComponentMode::Edge: {
            std::unordered_set<uint32_t> newSel;
            for (int i = 0; i < mesh.nbEdges; ++i) {
                uint32_t u = (uint32_t)i;
                if (!m_selectedEdges.count(u)) newSel.insert(u);
            }
            m_selectedEdges = std::move(newSel);
            break;
        }
        case ComponentMode::Face: {
            std::unordered_set<uint32_t> newSel;
            for (int i = 0; i < mesh.nbFaces; ++i) {
                uint32_t u = (uint32_t)i;
                if (!m_selectedFaces.count(u)) newSel.insert(u);
            }
            m_selectedFaces = std::move(newSel);
            break;
        }
    }
}

void SelectionManager::ensureEdgeTopology(const Mesh& mesh) const {
    if (m_edgeTopologyValid &&
        m_cachedMeshId == mesh.getID() &&
        m_cachedNbEdges == mesh.nbEdges &&
        m_cachedNbFaces == mesh.nbFaces &&
        m_edgeTopology.size() == (size_t)mesh.nbEdges) {
        return;
    }

    if (mesh.faceEdges.size() != (size_t)mesh.nbFaces * 4 || mesh.nbEdges <= 0) {
        const_cast<Mesh&>(mesh).initEdges();
    }

    m_edgeTopology.assign(mesh.nbEdges, EdgeTopology{});
    m_cachedMeshId = mesh.getID();
    m_cachedNbEdges = mesh.nbEdges;
    m_cachedNbFaces = mesh.nbFaces;

    int nbF = mesh.nbFaces;
    const auto& fAr = mesh.faces;
    const auto& feAr = mesh.faceEdges;

    for (int f = 0; f < nbF; ++f) {
        int id = f * 4;
        int numE = (fAr[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numE; ++k) {
            if ((size_t)(id + k) >= feAr.size()) continue;
            uint32_t e = feAr[id + k];
            if (e < (uint32_t)mesh.nbEdges) {
                uint32_t va = fAr[id + k];
                uint32_t vb = fAr[id + ((k + 1) % numE)];
                auto& top = m_edgeTopology[e];
                top.v0 = std::min(va, vb);
                top.v1 = std::max(va, vb);
                if (top.f0 == -1) {
                    top.f0 = f;
                    top.k0 = (int8_t)k;
                } else if (top.f1 == -1) {
                    top.f1 = f;
                    top.k1 = (int8_t)k;
                }
            }
        }
    }

    m_edgeTopologyValid = true;
}

void SelectionManager::selectEdgeLoop(const Mesh& mesh, uint32_t startEdgeIdx, bool add) {
    if (startEdgeIdx >= (uint32_t)mesh.nbEdges) return;
    ensureEdgeTopology(mesh);

    if (!add) {
        m_selectedEdges.clear();
    }

    std::unordered_set<uint32_t> visited;
    visited.insert(startEdgeIdx);
    m_selectedEdges.insert(startEdgeIdx);

    const auto& startTop = m_edgeTopology[startEdgeIdx];

    // Helper: walk connected loop end-to-end starting from edge 'e' out of vertex 'v'
    auto walkEnd = [&](uint32_t fromEdge, uint32_t vertex) {
        uint32_t curEdge = fromEdge;
        uint32_t curVert = vertex;

        while (true) {
            // Count faces sharing curVert
            if (curVert * 2 >= mesh.vrfStartCount.size()) break;
            uint32_t startRF = mesh.vrfStartCount[curVert * 2];
            uint32_t countRF = mesh.vrfStartCount[curVert * 2 + 1];

            // If curEdge is boundary edge, look for adjacent boundary edge at curVert
            const auto& topCur = m_edgeTopology[curEdge];
            bool isBoundary = (topCur.f1 == -1);

            if (isBoundary) {
                uint32_t nextBoundaryEdge = 0xffffffff;
                for (uint32_t i = 0; i < countRF; ++i) {
                    uint32_t f = mesh.vertRingFace[startRF + i];
                    int id = f * 4;
                    int numE = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
                    for (int k = 0; k < numE; ++k) {
                        uint32_t candidateE = mesh.faceEdges[id + k];
                        if (candidateE != curEdge && candidateE < m_edgeTopology.size()) {
                            const auto& candTop = m_edgeTopology[candidateE];
                            if ((candTop.v0 == curVert || candTop.v1 == curVert) && candTop.f1 == -1) {
                                nextBoundaryEdge = candidateE;
                                break;
                            }
                        }
                    }
                    if (nextBoundaryEdge != 0xffffffff) break;
                }

                if (nextBoundaryEdge != 0xffffffff && !visited.count(nextBoundaryEdge)) {
                    visited.insert(nextBoundaryEdge);
                    m_selectedEdges.insert(nextBoundaryEdge);
                    const auto& nTop = m_edgeTopology[nextBoundaryEdge];
                    curVert = (nTop.v0 == curVert) ? nTop.v1 : nTop.v0;
                    curEdge = nextBoundaryEdge;
                    continue;
                }
                break;
            }

            // In quad interior, a regular vertex has valence 4 (4 quad faces)
            if (countRF != 4) {
                // Non-quad pole: terminate loop
                break;
            }

            // Face A is topCur.f0
            int32_t faceA = topCur.f0;
            if (faceA < 0) break;
            int idA = faceA * 4;
            if (mesh.faces[idA + 3] == 0xffffffff) break; // Non-quad

            // In face A, find the other edge sharing curVert
            uint32_t sideEdge = 0xffffffff;
            int numEA = 4;
            for (int k = 0; k < numEA; ++k) {
                uint32_t eA = mesh.faceEdges[idA + k];
                if (eA != curEdge && eA < m_edgeTopology.size()) {
                    const auto& topA = m_edgeTopology[eA];
                    if (topA.v0 == curVert || topA.v1 == curVert) {
                        sideEdge = eA;
                        break;
                    }
                }
            }
            if (sideEdge == 0xffffffff) break;

            // Across sideEdge, find Face B
            const auto& topSide = m_edgeTopology[sideEdge];
            int32_t faceB = (topSide.f0 == faceA) ? topSide.f1 : topSide.f0;
            if (faceB < 0) break;
            int idB = faceB * 4;
            if (mesh.faces[idB + 3] == 0xffffffff) break; // Non-quad

            // In Face B, find the other edge sharing curVert that is not sideEdge
            uint32_t nextEdge = 0xffffffff;
            int numEB = 4;
            for (int k = 0; k < numEB; ++k) {
                uint32_t eB = mesh.faceEdges[idB + k];
                if (eB != sideEdge && eB < m_edgeTopology.size()) {
                    const auto& topB = m_edgeTopology[eB];
                    if (topB.v0 == curVert || topB.v1 == curVert) {
                        nextEdge = eB;
                        break;
                    }
                }
            }

            if (nextEdge == 0xffffffff || visited.count(nextEdge)) {
                break;
            }

            visited.insert(nextEdge);
            m_selectedEdges.insert(nextEdge);

            const auto& nTop = m_edgeTopology[nextEdge];
            curVert = (nTop.v0 == curVert) ? nTop.v1 : nTop.v0;
            curEdge = nextEdge;
        }
    };

    walkEnd(startEdgeIdx, startTop.v0);
    walkEnd(startEdgeIdx, startTop.v1);
}

void SelectionManager::selectEdgeRing(const Mesh& mesh, uint32_t startEdgeIdx, bool add) {
    if (startEdgeIdx >= (uint32_t)mesh.nbEdges) return;
    ensureEdgeTopology(mesh);

    if (!add) {
        m_selectedEdges.clear();
    }

    std::unordered_set<uint32_t> visited;
    visited.insert(startEdgeIdx);
    m_selectedEdges.insert(startEdgeIdx);

    auto walkRingDir = [&](int32_t startFace, int8_t startK) {
        int32_t curFace = startFace;
        int8_t curK = startK;

        while (curFace >= 0 && curFace < mesh.nbFaces) {
            int id = curFace * 4;
            if (mesh.faces[id + 3] == 0xffffffff) break; // Not a quad

            int8_t oppK = (curK + 2) % 4;
            uint32_t oppE = mesh.faceEdges[id + oppK];
            if (oppE >= (uint32_t)mesh.nbEdges || visited.count(oppE)) break;

            visited.insert(oppE);
            m_selectedEdges.insert(oppE);

            const auto& top = m_edgeTopology[oppE];
            if (top.f0 == curFace) {
                curFace = top.f1;
                curK = top.k1;
            } else {
                curFace = top.f0;
                curK = top.k0;
            }
        }
    };

    const auto& startTop = m_edgeTopology[startEdgeIdx];
    if (startTop.f0 >= 0) {
        walkRingDir(startTop.f0, startTop.k0);
    }
    if (startTop.f1 >= 0) {
        walkRingDir(startTop.f1, startTop.k1);
    }
}

void SelectionManager::expandCurrentEdgeLoops(const Mesh& mesh) {
    auto initialEdges = m_selectedEdges;
    for (uint32_t e : initialEdges) {
        selectEdgeLoop(mesh, e, true);
    }
}

void SelectionManager::expandCurrentEdgeRings(const Mesh& mesh) {
    auto initialEdges = m_selectedEdges;
    for (uint32_t e : initialEdges) {
        selectEdgeRing(mesh, e, true);
    }
}

std::vector<uint32_t> SelectionManager::getEffectiveVertexIndices(const Mesh& mesh) const {
    std::unordered_set<uint32_t> uniqueVerts;
    ensureEdgeTopology(mesh);

    if (m_mode == ComponentMode::Vertex) {
        for (uint32_t v : m_selectedVerts) {
            if (v < (uint32_t)mesh.nbVerts) uniqueVerts.insert(v);
        }
    } else if (m_mode == ComponentMode::Edge) {
        for (uint32_t e : m_selectedEdges) {
            if (e < m_edgeTopology.size()) {
                const auto& top = m_edgeTopology[e];
                if (top.v0 < (uint32_t)mesh.nbVerts) uniqueVerts.insert(top.v0);
                if (top.v1 < (uint32_t)mesh.nbVerts) uniqueVerts.insert(top.v1);
            }
        }
    } else if (m_mode == ComponentMode::Face) {
        for (uint32_t f : m_selectedFaces) {
            if (f < (uint32_t)mesh.nbFaces) {
                int id = f * 4;
                int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
                for (int k = 0; k < numV; ++k) {
                    uint32_t v = mesh.faces[id + k];
                    if (v < (uint32_t)mesh.nbVerts) uniqueVerts.insert(v);
                }
            }
        }
    }

    return std::vector<uint32_t>(uniqueVerts.begin(), uniqueVerts.end());
}

std::vector<uint32_t> SelectionManager::getEffectiveEdgeIndices(const Mesh& mesh) const {
    std::unordered_set<uint32_t> uniqueEdges;
    ensureEdgeTopology(mesh);

    if (m_mode == ComponentMode::Edge) {
        for (uint32_t e : m_selectedEdges) {
            if (e < (uint32_t)mesh.nbEdges) uniqueEdges.insert(e);
        }
    } else if (m_mode == ComponentMode::Face) {
        for (uint32_t f : m_selectedFaces) {
            if (f < (uint32_t)mesh.nbFaces) {
                int id = f * 4;
                int numE = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
                for (int k = 0; k < numE; ++k) {
                    uint32_t e = mesh.faceEdges[id + k];
                    if (e < (uint32_t)mesh.nbEdges) uniqueEdges.insert(e);
                }
            }
        }
    } else if (m_mode == ComponentMode::Vertex) {
        for (size_t e = 0; e < m_edgeTopology.size(); ++e) {
            const auto& top = m_edgeTopology[e];
            if (m_selectedVerts.count(top.v0) && m_selectedVerts.count(top.v1)) {
                uniqueEdges.insert((uint32_t)e);
            }
        }
    }

    return std::vector<uint32_t>(uniqueEdges.begin(), uniqueEdges.end());
}

glm::vec3 SelectionManager::computeCentroidLocal(const Mesh& mesh) const {
    auto verts = getEffectiveVertexIndices(mesh);
    if (verts.empty() || mesh.verts.empty()) {
        return mesh.center;
    }

    glm::vec3 sum(0.0f);
    for (uint32_t v : verts) {
        sum.x += mesh.verts[v * 3];
        sum.y += mesh.verts[v * 3 + 1];
        sum.z += mesh.verts[v * 3 + 2];
    }
    return sum / (float)verts.size();
}
