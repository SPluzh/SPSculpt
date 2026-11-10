#include "mesh/dynamic/DynamicTopology.h"
#include "mesh/NormalCalc.h"
#include "mesh/Topology.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <omp.h>

namespace dynamic {

namespace {

constexpr uint32_t kNone = ~0u;

static bool setBit(std::vector<uint64_t>& bits, uint32_t i) {
    if (size_t(i >> 6) >= bits.size()) bits.resize(size_t(i >> 6) + 1 + bits.size() / 4, 0);
    const uint64_t b = uint64_t(1) << (i & 63);
    if (bits[i >> 6] & b) return false;
    bits[i >> 6] |= b;
    return true;
}

static float quality(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    // 1 for an equilateral triangle, 0 for a degenerate one.
    const float area2 = glm::length(glm::cross(b - a, c - a));
    const float sum = glm::dot(b - a, b - a) + glm::dot(c - b, c - b) + glm::dot(a - c, a - c);
    return sum > 0.0f ? 3.4641016f * area2 / sum : 0.0f;
}

} // namespace

std::vector<size_t> symmetryInverses(std::span<const SymCopy> copies) {
    std::vector<size_t> out(copies.size(), 0);
    for (size_t i = 0; i < copies.size(); ++i) {
        const glm::mat3 t = glm::transpose(copies[i].m);
        for (size_t j = 0; j < copies.size(); ++j) {
            if (copies[j].m == t && copies[j].pivot == copies[i].pivot) {
                out[i] = j;
                break;
            }
        }
    }
    return out;
}

void TopologyUndo::swapWith(Mesh& mesh) {
    const uint32_t nv = (uint32_t)mesh.nbVerts;
    const uint32_t nt = (uint32_t)mesh.nbFaces;

    // Swap vertex positions
    {
        std::vector<glm::vec3> current;
        current.reserve(vertexSlots.size());
        for (uint32_t s : vertexSlots) {
            if (s < nv) {
                current.push_back(glm::vec3(mesh.verts[s * 3], mesh.verts[s * 3 + 1], mesh.verts[s * 3 + 2]));
            }
        }
        mesh.verts.resize(vertexCount * 3, 0.0f);
        size_t k = 0;
        for (uint32_t s : vertexSlots) {
            if (s < vertexCount && k < vertices.positions.size()) {
                mesh.verts[s * 3 + 0] = vertices.positions[k].x;
                mesh.verts[s * 3 + 1] = vertices.positions[k].y;
                mesh.verts[s * 3 + 2] = vertices.positions[k].z;
                ++k;
            }
        }
        vertices.positions.swap(current);
    }

    // Swap colors
    if (!mesh.colors.empty() || !vertices.colors.empty()) {
        std::vector<glm::vec3> current;
        current.reserve(vertexSlots.size());
        for (uint32_t s : vertexSlots) {
            if (s < nv && !mesh.colors.empty()) {
                current.push_back(glm::vec3(mesh.colors[s * 3], mesh.colors[s * 3 + 1], mesh.colors[s * 3 + 2]));
            }
        }
        mesh.colors.resize(vertexCount * 3, 1.0f);
        size_t k = 0;
        for (uint32_t s : vertexSlots) {
            if (s < vertexCount && k < vertices.colors.size()) {
                mesh.colors[s * 3 + 0] = vertices.colors[k].x;
                mesh.colors[s * 3 + 1] = vertices.colors[k].y;
                mesh.colors[s * 3 + 2] = vertices.colors[k].z;
                ++k;
            }
        }
        vertices.colors.swap(current);
    }

    // Swap materials
    if (!mesh.materials.empty() || !vertices.materials.empty()) {
        std::vector<glm::vec3> current;
        current.reserve(vertexSlots.size());
        for (uint32_t s : vertexSlots) {
            if (s < nv && !mesh.materials.empty()) {
                current.push_back(glm::vec3(mesh.materials[s * 3], mesh.materials[s * 3 + 1], mesh.materials[s * 3 + 2]));
            }
        }
        mesh.materials.resize(vertexCount * 3, 0.0f);
        // Default unmasked
        for (size_t i = 0; i < vertexCount; ++i) {
            mesh.materials[i * 3 + 2] = 1.0f;
        }
        size_t k = 0;
        for (uint32_t s : vertexSlots) {
            if (s < vertexCount && k < vertices.materials.size()) {
                mesh.materials[s * 3 + 0] = vertices.materials[k].x;
                mesh.materials[s * 3 + 1] = vertices.materials[k].y;
                mesh.materials[s * 3 + 2] = vertices.materials[k].z;
                ++k;
            }
        }
        vertices.materials.swap(current);
    }

    // Swap vertVisible
    if (!mesh.vertVisible.empty() || !vertices.visible.empty()) {
        std::vector<uint8_t> current;
        current.reserve(vertexSlots.size());
        for (uint32_t s : vertexSlots) {
            if (s < nv && !mesh.vertVisible.empty()) {
                current.push_back(mesh.vertVisible[s]);
            }
        }
        mesh.vertVisible.resize(vertexCount, 1);
        size_t k = 0;
        for (uint32_t s : vertexSlots) {
            if (s < vertexCount && k < vertices.visible.size()) {
                mesh.vertVisible[s] = vertices.visible[k++];
            }
        }
        vertices.visible.swap(current);
    }

    // Swap triangles (faces)
    {
        std::vector<glm::uvec4> current;
        current.reserve(triangleSlots.size());
        for (uint32_t s : triangleSlots) {
            if (s < nt) {
                current.push_back(glm::uvec4(
                    mesh.faces[s * 4 + 0],
                    mesh.faces[s * 4 + 1],
                    mesh.faces[s * 4 + 2],
                    mesh.faces[s * 4 + 3]
                ));
            }
        }
        mesh.faces.resize(triangleCount * 4);
        size_t k = 0;
        for (uint32_t s : triangleSlots) {
            if (s < triangleCount && k < triangles.triangles.size()) {
                mesh.faces[s * 4 + 0] = triangles.triangles[k].x;
                mesh.faces[s * 4 + 1] = triangles.triangles[k].y;
                mesh.faces[s * 4 + 2] = triangles.triangles[k].z;
                mesh.faces[s * 4 + 3] = triangles.triangles[k].w;
                ++k;
            }
        }
        triangles.triangles.swap(current);
    }

    // Swap faceGroups
    if (!mesh.faceGroups.empty() || !triangles.faceGroups.empty()) {
        std::vector<uint32_t> current;
        current.reserve(triangleSlots.size());
        for (uint32_t s : triangleSlots) {
            if (s < nt && !mesh.faceGroups.empty()) {
                current.push_back(mesh.faceGroups[s]);
            }
        }
        mesh.faceGroups.resize(triangleCount, 0);
        size_t k = 0;
        for (uint32_t s : triangleSlots) {
            if (s < triangleCount && k < triangles.faceGroups.size()) {
                mesh.faceGroups[s] = triangles.faceGroups[k++];
            }
        }
        triangles.faceGroups.swap(current);
    }

    // Swap faceVisible
    if (!mesh.faceVisible.empty() || !triangles.visible.empty()) {
        std::vector<uint8_t> current;
        current.reserve(triangleSlots.size());
        for (uint32_t s : triangleSlots) {
            if (s < nt && !mesh.faceVisible.empty()) {
                current.push_back(mesh.faceVisible[s]);
            }
        }
        mesh.faceVisible.resize(triangleCount, 1);
        size_t k = 0;
        for (uint32_t s : triangleSlots) {
            if (s < triangleCount && k < triangles.visible.size()) {
                mesh.faceVisible[s] = triangles.visible[k++];
            }
        }
        triangles.visible.swap(current);
    }

    mesh.nbVerts = (int)vertexCount;
    mesh.nbFaces = (int)triangleCount;
    mesh.normals.resize(vertexCount * 3, 0.0f);
    mesh.faceNormals.resize(triangleCount * 3, 0.0f);
    mesh.faceBoxes.resize(triangleCount * 6, 0.0f);
    mesh.faceCenters.resize(triangleCount * 3, 0.0f);
    if (mesh.vertProxy.size() != mesh.verts.size()) {
        mesh.vertProxy.resize(vertexCount * 3, 0.0f);
    }

    vertexCount = nv;
    triangleCount = nt;
}

// ---------------------------------------------------------------------------
// Recording and elements
// ---------------------------------------------------------------------------

void DynamicTopology::recordVertex(uint32_t v) {
    if (!setBit(vertRec_, v)) return;
    undo_->vertexSlots.push_back(v);
    recordedNow_.push_back(v);
    if (v < startVertices_) {
        undo_->vertices.positions.push_back(getPos(v));
        if (!mesh_->colors.empty()) undo_->vertices.colors.push_back(getColor(v));
        if (!mesh_->materials.empty()) undo_->vertices.materials.push_back(getMaterial(v));
        if (!mesh_->vertVisible.empty() && v < mesh_->vertVisible.size())
            undo_->vertices.visible.push_back(mesh_->vertVisible[v]);
    }
}

void DynamicTopology::recordTriangle(uint32_t t) {
    if (!setBit(triRec_, t)) return;
    undo_->triangleSlots.push_back(t);
    if (t < startTriangles_) {
        undo_->triangles.triangles.push_back(getFace4(t));
        if (!mesh_->faceGroups.empty() && t < mesh_->faceGroups.size())
            undo_->triangles.faceGroups.push_back(mesh_->faceGroups[t]);
        if (!mesh_->faceVisible.empty() && t < mesh_->faceVisible.size())
            undo_->triangles.visible.push_back(mesh_->faceVisible[t]);
    }
}

void DynamicTopology::markVertex(uint32_t v) {
    if (v >= changedVFlag_.size()) changedVFlag_.resize(size_t(v) + 1, 0);
    if (changedVFlag_[v]) return;
    changedVFlag_[v] = 1;
    changedV_.push_back(v);
}

void DynamicTopology::markTriangle(uint32_t t) {
    changedT_.push_back(t);
}

uint32_t DynamicTopology::newVertex() {
    Mesh& m = *mesh_;
    if (!freeV_.empty()) {
        const uint32_t v = freeV_.back();
        freeV_.pop_back();
        deadV_[v] = 0;
        return v;
    }
    const uint32_t v = (uint32_t)m.nbVerts;
    recordVertex(v);
    m.verts.resize((v + 1) * 3, 0.0f);
    if (!m.normals.empty()) m.normals.resize((v + 1) * 3, 0.0f);
    if (!m.colors.empty()) m.colors.resize((v + 1) * 3, 1.0f);
    if (!m.materials.empty()) {
        m.materials.resize((v + 1) * 3, 0.0f);
        m.materials[v * 3 + 2] = 1.0f; // unmasked
    }
    if (!m.vertVisible.empty()) m.vertVisible.resize(v + 1, 1);
    if (!m.vertProxy.empty()) m.vertProxy.resize((v + 1) * 3, 0.0f);
    m_vertTris.clearVert(v);
    m.nbVerts = (int)(v + 1);
    deadV_.push_back(0);
    return v;
}

void DynamicTopology::interpolateVertex(uint32_t v, uint32_t a, uint32_t b, float t, const glm::vec3& position) {
    Mesh& m = *mesh_;
    recordVertex(v);
    setPos(v, position);
    if (!m.normals.empty()) {
        glm::vec3 na = getNormal(a), nb = getNormal(b);
        glm::vec3 n = glm::mix(na, nb, t);
        float len = glm::length(n);
        setNormal(v, len > 1e-20f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (!m.colors.empty()) {
        glm::vec3 ca = getColor(a), cb = getColor(b);
        setColor(v, glm::mix(ca, cb, t));
    }
    if (!m.materials.empty()) {
        glm::vec3 ma = getMaterial(a), mb = getMaterial(b);
        setMaterial(v, glm::mix(ma, mb, t));
    }
    if (!m.vertVisible.empty()) {
        m.vertVisible[v] = m.vertVisible[a] && m.vertVisible[b];
    }
    markVertex(v);
}

void DynamicTopology::killVertex(uint32_t v) {
    if (v >= deadV_.size() || deadV_[v]) return;
    recordVertex(v);
    deadV_[v] = 1;
    freeV_.push_back(v);
    Mesh& m = *mesh_;
    if (!m.vertVisible.empty() && v < m.vertVisible.size()) {
        m.vertVisible[v] = 0;
    }
    m_vertTris.clearVert(v);
    for (size_t k = 0; k < mirrorMaps_.size(); ++k) {
        const uint32_t w = mirrorOf(k, v);
        if (w == kNone) continue;
        const size_t back = k < mirrorInverse_.size() ? mirrorInverse_[k] : k;
        if (mirrorOf(back, w) == v) mirrorMaps_[back].stamp[w] = 0;
        mirrorMaps_[k].stamp[v] = 0;
    }
}

void DynamicTopology::vtInsert(uint32_t v, uint32_t t) {
    auto r = m_vertTris[v];
    scratch_.assign(r.begin(), r.end());
    auto it = std::upper_bound(scratch_.begin(), scratch_.end(), t);
    scratch_.insert(it, t);
    m_vertTris.set(v, scratch_);
}

void DynamicTopology::vtErase(uint32_t v, uint32_t t) {
    if (v >= m_vertTris.size()) return;
    auto r = m_vertTris[v];
    scratch_.assign(r.begin(), r.end());
    auto it = std::lower_bound(scratch_.begin(), scratch_.end(), t);
    if (it != scratch_.end() && *it == t) {
        scratch_.erase(it);
        m_vertTris.set(v, scratch_);
    }
}

void DynamicTopology::vtReplace(uint32_t v, uint32_t from, uint32_t to) {
    if (v >= m_vertTris.size()) return;
    auto r = m_vertTris[v];
    scratch_.assign(r.begin(), r.end());
    auto it = std::lower_bound(scratch_.begin(), scratch_.end(), from);
    if (it != scratch_.end() && *it == from) {
        scratch_.erase(it);
    }
    auto ins = std::upper_bound(scratch_.begin(), scratch_.end(), to);
    scratch_.insert(ins, to);
    m_vertTris.set(v, scratch_);
}

uint32_t DynamicTopology::newTriangle(const glm::uvec3& tri, uint32_t like) {
    Mesh& m = *mesh_;
    uint32_t t;
    if (!freeT_.empty()) {
        t = freeT_.back();
        freeT_.pop_back();
        deadT_[t] = 0;
    } else {
        t = (uint32_t)m.nbFaces;
        recordTriangle(t);
        m.faces.resize((t + 1) * 4);
        if (!m.faceGroups.empty()) m.faceGroups.resize(t + 1, 0);
        if (!m.faceVisible.empty()) m.faceVisible.resize(t + 1, 1);
        if (!m.faceNormals.empty()) m.faceNormals.resize((t + 1) * 3, 0.0f);
        if (!m.faceBoxes.empty()) m.faceBoxes.resize((t + 1) * 6, 0.0f);
        if (!m.faceCenters.empty()) m.faceCenters.resize((t + 1) * 3, 0.0f);
        m.nbFaces = (int)(t + 1);
        deadT_.push_back(0);
    }
    recordTriangle(t);
    setTri(t, tri);
    if (!m.faceGroups.empty() && like < m.faceGroups.size()) m.faceGroups[t] = m.faceGroups[like];
    if (!m.faceVisible.empty() && like < m.faceVisible.size()) m.faceVisible[t] = m.faceVisible[like];
    for (int c = 0; c < 3; ++c) {
        vtInsert(tri[c], t);
        markVertex(tri[c]);
    }
    bvh_->insertTriangle(m, t, like);
    markTriangle(t);
    return t;
}

void DynamicTopology::setTriangle(uint32_t t, const glm::uvec3& tri) {
    Mesh& m = *mesh_;
    recordTriangle(t);
    const glm::uvec3 old = getTri(t);
    auto has = [](const glm::uvec3& x, uint32_t v) { return x.x == v || x.y == v || x.z == v; };
    for (int c = 0; c < 3; ++c) {
        if (!has(tri, old[c])) vtErase(old[c], t);
        markVertex(old[c]);
    }
    for (int c = 0; c < 3; ++c) {
        if (!has(old, tri[c])) vtInsert(tri[c], t);
        markVertex(tri[c]);
    }
    setTri(t, tri);
    bvh_->touchTriangle(t);
    markTriangle(t);
}

void DynamicTopology::killTriangle(uint32_t t) {
    if (t >= deadT_.size() || deadT_[t]) return;
    Mesh& m = *mesh_;
    recordTriangle(t);
    const glm::uvec4 f4 = getFace4(t);
    const int nc = (f4.w != 0xFFFFFFFF) ? 4 : 3;
    for (int c = 0; c < nc; ++c) {
        vtErase(f4[c], t);
        markVertex(f4[c]);
    }
    bvh_->removeTriangle(t);
    deadT_[t] = 1;
    freeT_.push_back(t);
    if (!m.faceVisible.empty() && t < m.faceVisible.size()) {
        m.faceVisible[t] = 0;
    }
    markTriangle(t);
}

void DynamicTopology::moveTriangle(uint32_t from, uint32_t to) {
    Mesh& m = *mesh_;
    recordTriangle(to);
    recordTriangle(from);
    for (int k = 0; k < 4; ++k) m.faces[to * 4 + k] = m.faces[from * 4 + k];
    if (!m.faceGroups.empty()) m.faceGroups[to] = m.faceGroups[from];
    if (!m.faceVisible.empty()) m.faceVisible[to] = m.faceVisible[from];

    const glm::uvec4 f4 = getFace4(to);
    const int nc = (f4.w != 0xFFFFFFFF) ? 4 : 3;
    for (int c = 0; c < nc; ++c) vtReplace(f4[c], from, to);
    bvh_->renumberTriangle(from, to);
    markTriangle(to);
    markTriangle(from);
}

void DynamicTopology::moveVertex(uint32_t from, uint32_t to) {
    Mesh& m = *mesh_;
    recordVertex(to);
    recordVertex(from);
    for (int k = 0; k < 3; ++k) m.verts[to * 3 + k] = m.verts[from * 3 + k];
    if (!m.normals.empty()) for (int k = 0; k < 3; ++k) m.normals[to * 3 + k] = m.normals[from * 3 + k];
    if (!m.colors.empty()) for (int k = 0; k < 3; ++k) m.colors[to * 3 + k] = m.colors[from * 3 + k];
    if (!m.materials.empty()) for (int k = 0; k < 3; ++k) m.materials[to * 3 + k] = m.materials[from * 3 + k];
    if (!m.vertVisible.empty()) m.vertVisible[to] = m.vertVisible[from];
    if (!m.vertProxy.empty()) {
        if (m.vertProxy.size() <= (to + 1) * 3) m.vertProxy.resize((to + 1) * 3, 0.0f);
        if (from * 3 + 2 < m.vertProxy.size()) {
            for (int k = 0; k < 3; ++k) m.vertProxy[to * 3 + k] = m.vertProxy[from * 3 + k];
        }
    }

    auto r = m_vertTris[from];
    scratch2_.assign(r.begin(), r.end());
    m_vertTris.set(to, scratch2_);
    for (uint32_t t : scratch2_) {
        recordTriangle(t);
        glm::uvec4 f4 = getFace4(t);
        const int nc = (f4.w != 0xFFFFFFFF) ? 4 : 3;
        for (int c = 0; c < nc; ++c) {
            if (f4[c] == from) f4[c] = to;
            else markVertex(f4[c]);
        }
        setFace4(t, f4);
        markTriangle(t);
    }
    m_vertTris.clearVert(from);
    markVertex(to);
    moved_.emplace_back(from, to);
}

void DynamicTopology::compact() {
    Mesh& m = *mesh_;
    std::sort(freeT_.begin(), freeT_.end());
    freeT_.erase(std::unique(freeT_.begin(), freeT_.end()), freeT_.end());
    size_t nt = (size_t)m.nbFaces;
    for (uint32_t t : freeT_) {
        while (nt > 0 && deadT_[nt - 1]) --nt;
        if (t >= nt) break;
        moveTriangle(uint32_t(nt - 1), t);
        deadT_[t] = 0;
        --nt;
    }
    m.faces.resize(nt * 4);
    if (!m.faceGroups.empty()) m.faceGroups.resize(nt);
    if (!m.faceVisible.empty()) m.faceVisible.resize(nt);
    m.faceNormals.resize(nt * 3, 0.0f);
    m.faceBoxes.resize(nt * 6, 0.0f);
    m.faceCenters.resize(nt * 3, 0.0f);
    m.nbFaces = (int)nt;
    bvh_->resizeTriangles(nt);
    deadT_.resize(nt);
    freeT_.clear();

    std::sort(freeV_.begin(), freeV_.end());
    freeV_.erase(std::unique(freeV_.begin(), freeV_.end()), freeV_.end());
    size_t nv = (size_t)m.nbVerts;
    for (uint32_t v : freeV_) {
        while (nv > 0 && deadV_[nv - 1]) --nv;
        if (v >= nv) break;
        moveVertex(uint32_t(nv - 1), v);
        deadV_[v] = 0;
        --nv;
    }
    m.verts.resize(nv * 3);
    if (!m.normals.empty()) m.normals.resize(nv * 3);
    if (!m.colors.empty()) m.colors.resize(nv * 3);
    if (!m.materials.empty()) m.materials.resize(nv * 3);
    if (!m.vertVisible.empty()) m.vertVisible.resize(nv);
    if (m.vertProxy.size() != m.verts.size()) m.vertProxy.resize(nv * 3, 0.0f);
    m_vertTris.resize(nv);
    m.nbVerts = (int)nv;
    deadV_.resize(nv);
    freeV_.clear();

    // Defensive check: ensure every face vertex index is strictly < nv
    for (size_t f = 0; f < nt; ++f) {
        for (int k = 0; k < 3; ++k) {
            if (m.faces[f * 4 + k] >= nv) {
                m.faces[f * 4 + k] = 0;
            }
        }
        if (m.faces[f * 4 + 3] != 0xFFFFFFFF && m.faces[f * 4 + 3] >= nv) {
            m.faces[f * 4 + 3] = 0xFFFFFFFF;
        }
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int DynamicTopology::edgeTriangles(uint32_t a, uint32_t b, uint32_t* out) const {
    int n = 0;
    if (a >= m_vertTris.size()) return 0;
    for (uint32_t t : m_vertTris[a]) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        if (!isQuad(t)) {
            glm::uvec3 tri = getTri(t);
            if (tri.x == b || tri.y == b || tri.z == b) {
                if (n < 4) out[n] = t;
                ++n;
            }
        } else {
            glm::uvec4 f4 = getFace4(t);
            if ((f4.x == a && (f4.y == b || f4.w == b)) ||
                (f4.y == a && (f4.z == b || f4.x == b)) ||
                (f4.z == a && (f4.w == b || f4.y == b)) ||
                (f4.w == a && (f4.x == b || f4.z == b))) {
                if (n < 4) out[n] = t;
                ++n;
            }
        }
    }
    return n;
}

void DynamicTopology::ring(uint32_t v, std::vector<uint32_t>& out) const {
    out.clear();
    if (v >= m_vertTris.size()) return;
    for (uint32_t t : m_vertTris[v]) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        if (!isQuad(t)) {
            glm::uvec3 tri = getTri(t);
            for (int c = 0; c < 3; ++c) {
                if (tri[c] != v) out.push_back(tri[c]);
            }
        } else {
            glm::uvec4 f4 = getFace4(t);
            for (int k = 0; k < 4; ++k) {
                if (f4[k] == v) {
                    out.push_back(f4[(k + 1) % 4]);
                    out.push_back(f4[(k + 3) % 4]);
                    break;
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

float DynamicTopology::len2(uint32_t a, uint32_t b) const {
    const glm::vec3 d = getPos(a) - getPos(b);
    return glm::dot(d, d);
}

bool DynamicTopology::inside(uint32_t a, uint32_t b, bool mirrored) const {
    const glm::vec3 p = getPos(a), d = getPos(b) - p;
    const float dd = glm::dot(d, d);
    auto within = [&](const glm::vec3& c) {
        const float t = dd > 0.0f ? std::clamp(glm::dot(c - p, d) / dd, 0.0f, 1.0f) : 0.0f;
        const glm::vec3 q = p + d * t - c;
        return glm::dot(q, q) <= radius_ * radius_;
    };
    if (within(center_)) return true;
    if (!mirrored) return false;
    for (const SymCopy& s : mirrorSigns_) {
        if (within(s.point(center_))) return true;
    }
    return false;
}

bool DynamicTopology::candidate(uint32_t a, uint32_t b) const {
    if (inside(a, b)) return true;
    if (mirrorSigns_.empty() || !inside(a, b, true)) return false;
    orbit(a, b, orbitScratch_);
    return orbitIncomplete_;
}

uint32_t DynamicTopology::triangulateQuad(uint32_t q) {
    if (q >= (uint32_t)mesh_->nbFaces || deadT_[q] || !isQuad(q)) return kNone;
    glm::uvec4 f = getFace4(q);
    uint32_t v0 = f.x, v1 = f.y, v2 = f.z, v3 = f.w;

    float d02 = len2(v0, v2);
    float d13 = len2(v1, v3);
    bool use02 = false;

    float maxD = std::max(d02, d13);
    if (std::abs(d02 - d13) > 1e-4f * maxD) {
        use02 = (d02 < d13);
    } else {
        auto diagSymScore = [&](uint32_t a, uint32_t b) -> float {
            glm::vec3 pa = getPos(a), pb = getPos(b);
            glm::vec3 aa = glm::abs(pa), ab = glm::abs(pb);
            if (std::tie(pa.y, pa.z, aa.x) > std::tie(pb.y, pb.z, ab.x)) {
                std::swap(pa, pb);
                std::swap(aa, ab);
            }
            glm::vec3 pa2 = pa * pa, pb2 = pb * pb;
            return (pb2.x - pa2.x) * (pb.y - pa.y) +
                   (pb2.y - pa2.y) * (pb.z - pa.z) +
                   (pb2.z - pa2.z) * (ab.x - aa.x);
        };
        use02 = (diagSymScore(v0, v2) >= diagSymScore(v1, v3));
    }

    recordTriangle(q);
    if (use02) {
        vtErase(v3, q);
        setTri(q, glm::uvec3(v0, v1, v2));
        markVertex(v0); markVertex(v1); markVertex(v2); markVertex(v3);
        bvh_->touchTriangle(q);
        markTriangle(q);
        uint32_t t2 = newTriangle(glm::uvec3(v0, v2, v3), q);
        return t2;
    } else {
        vtErase(v2, q);
        setTri(q, glm::uvec3(v0, v1, v3));
        markVertex(v0); markVertex(v1); markVertex(v2); markVertex(v3);
        bvh_->touchTriangle(q);
        markTriangle(q);
        uint32_t t2 = newTriangle(glm::uvec3(v1, v2, v3), q);
        return t2;
    }
}

void DynamicTopology::queryRegion() {
    bvh_->queryTriangles(*mesh_, center_, radius_, regionTris_);
    mirrorTris_.clear();
    slivers_.clear();

    auto processQuadsInList = [&](std::vector<uint32_t>& list) {
        std::vector<uint32_t> kept;
        kept.reserve(list.size());
        for (uint32_t t : list) {
            if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
            if (isQuad(t)) {
                glm::uvec4 f = getFace4(t);
                bool inBrush = inside(f[0], f[1], true) ||
                               inside(f[1], f[2], true) ||
                               inside(f[2], f[3], true) ||
                               inside(f[3], f[0], true);
                if (inBrush) {
                    uint32_t t2 = triangulateQuad(t);
                    kept.push_back(t);
                    if (t2 != kNone) kept.push_back(t2);
                }
            } else {
                kept.push_back(t);
            }
        }
        list = std::move(kept);
    };

    processQuadsInList(regionTris_);

    auto addSlivers = [&](const std::vector<uint32_t>& tris) {
        for (uint32_t t : tris) {
            if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
            glm::uvec3 tri = getTri(t);
            if (quality(getPos(tri.x), getPos(tri.y), getPos(tri.z)) < 0.05f) {
                slivers_.push_back(t);
            }
        }
    };
    addSlivers(regionTris_);

    std::vector<uint32_t> more;
    for (const SymCopy& s : mirrorSigns_) {
        bvh_->queryTriangles(*mesh_, s.point(center_), radius_, more);
        processQuadsInList(more);
        addSlivers(more);
        mirrorTris_.insert(mirrorTris_.end(), more.begin(), more.end());
    }
    std::sort(mirrorTris_.begin(), mirrorTris_.end());
    mirrorTris_.erase(std::unique(mirrorTris_.begin(), mirrorTris_.end()), mirrorTris_.end());
    std::sort(slivers_.begin(), slivers_.end());
    slivers_.erase(std::unique(slivers_.begin(), slivers_.end()), slivers_.end());
}

bool DynamicTopology::locked(uint32_t v) const {
    const Mesh& m = *mesh_;
    if (v >= deadV_.size() || deadV_[v]) return true;
    if (!m.materials.empty() && v * 3 + 2 < m.materials.size()) {
        if (m.materials[v * 3 + 2] <= 0.001f) return true; // masked
    }
    if (v >= m_vertTris.size()) return true;
    const auto& tris = m_vertTris[v];
    if (tris.empty()) return true;
    if (!m.faceVisible.empty()) {
        for (uint32_t t : tris) {
            if (t < m.faceVisible.size() && !m.faceVisible[t]) return true;
        }
    }
    auto getFaceNeighbors = [&](uint32_t t, uint32_t* neighbors) {
        if (!isQuad(t)) {
            glm::uvec3 tri = getTri(t);
            int idx = 0;
            for (int c = 0; c < 3; ++c) {
                if (tri[c] != v && idx < 2) neighbors[idx++] = tri[c];
            }
        } else {
            glm::uvec4 f4 = getFace4(t);
            for (int k = 0; k < 4; ++k) {
                if (f4[k] == v) {
                    neighbors[0] = f4[(k + 1) % 4];
                    neighbors[1] = f4[(k + 3) % 4];
                    break;
                }
            }
        }
    };
    if (tris.size() <= 32) {
        uint32_t other[96], count[96];
        int n = 0;
        for (uint32_t t : tris) {
            if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
            uint32_t nb[2];
            getFaceNeighbors(t, nb);
            for (int c = 0; c < 2; ++c) {
                uint32_t u = nb[c];
                int i = 0;
                while (i < n && other[i] != u) ++i;
                if (i == n) {
                    other[n] = u;
                    count[n++] = 0;
                }
                if (++count[i] > 2) return true;
            }
        }
        return false;
    }
    std::vector<uint32_t>& others = lockScratch_;
    others.clear();
    for (uint32_t t : tris) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        uint32_t nb[2];
        getFaceNeighbors(t, nb);
        others.push_back(nb[0]);
        others.push_back(nb[1]);
    }
    std::sort(others.begin(), others.end());
    for (size_t i = 0; i + 2 < others.size(); ++i) {
        if (others[i] == others[i + 2]) return true;
    }
    return false;
}

bool DynamicTopology::planeVertex(uint32_t v) const {
    for (size_t k = 0; k < mirrorMaps_.size(); ++k) {
        if (mirrorOf(k, v) == v) return true;
    }
    return false;
}

bool DynamicTopology::featureEdge(uint32_t a, uint32_t b) const {
    uint32_t tris[4];
    const int n = edgeTriangles(a, b, tris);
    if (n != 2) return true;
    if (isQuad(tris[0]) || isQuad(tris[1])) return true;
    if (!mesh_->faceGroups.empty()) {
        uint32_t g0 = tris[0] < mesh_->faceGroups.size() ? mesh_->faceGroups[tris[0]] : 0;
        uint32_t g1 = tris[1] < mesh_->faceGroups.size() ? mesh_->faceGroups[tris[1]] : 0;
        if (g0 != g1) return true;
    }
    for (size_t k = 0; k < mirrorMaps_.size(); ++k) {
        if (mirrorOf(k, a) == a && mirrorOf(k, b) == b) return true;
    }
    return false;
}

int DynamicTopology::featureEdges(uint32_t v, uint32_t* other) const {
    ring(v, scratch_);
    int n = 0;
    for (uint32_t u : scratch_) {
        if (featureEdge(v, u)) {
            if (n < 2) other[n] = u;
            ++n;
        }
    }
    if (n == 0 && planeVertex(v)) return 3;
    return std::min(n, 3);
}

bool DynamicTopology::boundaryVertex(uint32_t v) const {
    ring(v, scratch2_);
    uint32_t tris[4];
    for (uint32_t u : scratch2_) {
        if (edgeTriangles(v, u, tris) == 1) return true;
    }
    return false;
}

glm::vec3 DynamicTopology::faceNormalAt(uint32_t v) const {
    glm::vec3 n(0.0f);
    if (v >= m_vertTris.size()) return glm::vec3(0.0f, 1.0f, 0.0f);
    for (uint32_t t : m_vertTris[v]) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        if (!isQuad(t)) {
            glm::uvec3 tri = getTri(t);
            glm::vec3 pa = getPos(tri.x), pb = getPos(tri.y), pc = getPos(tri.z);
            n += glm::cross(pb - pa, pc - pa);
        } else {
            glm::uvec4 f4 = getFace4(t);
            glm::vec3 p0 = getPos(f4.x), p1 = getPos(f4.y), p2 = getPos(f4.z), p3 = getPos(f4.w);
            n += glm::cross(p1 - p0, p2 - p0) + glm::cross(p2 - p0, p3 - p0);
        }
    }
    const float len = glm::length(n);
    return len > 1e-30f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Symmetry
// ---------------------------------------------------------------------------

uint32_t DynamicTopology::mirrorOf(size_t k, uint32_t v) const {
    const MirrorMap& map = mirrorMaps_[k];
    return v < map.to.size() && map.stamp[v] == mirrorStamp_ ? map.to[v] : kNone;
}

void DynamicTopology::setMirror(size_t k, uint32_t v, uint32_t w) {
    MirrorMap& map = mirrorMaps_[k];
    const size_t need = size_t(std::max(v, w)) + 1;
    if (map.to.size() < need) {
        map.to.resize(need + need / 4, kNone);
        map.stamp.resize(need + need / 4, 0);
    }
    map.to[v] = w;
    map.stamp[v] = mirrorStamp_;
    MirrorMap& back = mirrorMaps_[k < mirrorInverse_.size() ? mirrorInverse_[k] : k];
    if (back.to.size() < need) {
        back.to.resize(need + need / 4, kNone);
        back.stamp.resize(need + need / 4, 0);
    }
    back.to[w] = v;
    back.stamp[w] = mirrorStamp_;
}

void DynamicTopology::matchMirrors(const std::vector<uint32_t>& verts) {
    const float cell = std::max(eps_ * 2.0f, 1e-12f);
    auto cellOf = [cell](const glm::vec3& p) { return glm::ivec3(glm::floor(p / cell)); };
    auto keyOf = [](const glm::ivec3& q) {
        return (uint64_t(uint32_t(q.x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(q.y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(q.z) & 0x1FFFFF);
    };
    size_t slots = 16;
    while (slots < verts.size() * 2) slots *= 2;
    gridKeys_.assign(slots, ~uint64_t(0));
    gridHead_.resize(slots);
    gridNext_.resize(verts.size());
    auto slotOf = [&](uint64_t key) {
        size_t h = size_t((key * 0x9E3779B97F4A7C15ull) >> 20) & (slots - 1);
        while (gridKeys_[h] != ~uint64_t(0) && gridKeys_[h] != key) h = (h + 1) & (slots - 1);
        return h;
    };
    for (uint32_t i = 0; i < verts.size(); ++i) {
        const uint64_t key = keyOf(cellOf(getPos(verts[i])));
        const size_t h = slotOf(key);
        gridNext_[i] = gridKeys_[h] == key ? gridHead_[h] : kNone;
        gridKeys_[h] = key;
        gridHead_[h] = i;
    }
    auto nearest = [&](const glm::vec3& p) {
        uint32_t best = kNone;
        float bestD = eps_ * eps_;
        const glm::vec3 f = p / cell;
        const glm::ivec3 q = glm::ivec3(glm::floor(f));
        const glm::ivec3 step(f.x - float(q.x) < 0.5f ? -1 : 1, f.y - float(q.y) < 0.5f ? -1 : 1, f.z - float(q.z) < 0.5f ? -1 : 1);
        for (int c = 0; c < 8; ++c) {
            const glm::ivec3 at = q + glm::ivec3(c & 1 ? step.x : 0, c & 2 ? step.y : 0, c & 4 ? step.z : 0);
            const uint64_t key = keyOf(at);
            const size_t h = slotOf(key);
            if (gridKeys_[h] != key) continue;
            for (uint32_t i = gridHead_[h]; i != kNone; i = gridNext_[i]) {
                const uint32_t w = verts[i];
                const glm::vec3 d = getPos(w) - p;
                const float dd = glm::dot(d, d);
                if (dd < bestD || (dd == bestD && best != kNone && w < best)) {
                    bestD = dd;
                    best = w;
                }
            }
        }
        return best;
    };
    for (size_t k = 0; k < mirrorSigns_.size(); ++k) {
        const SymCopy& s = mirrorSigns_[k];
        for (uint32_t v : verts) {
            if (mirrorOf(k, v) != kNone) continue;
            const uint32_t w = nearest(s.point(getPos(v)));
            if (w == kNone || nearest(s.inversePoint(getPos(w))) != v) continue;
            setMirror(k, v, w);
        }
    }
}

void DynamicTopology::orbit(uint32_t a, uint32_t b, std::vector<std::pair<uint32_t, uint32_t>>& out) const {
    out.assign(1, {a, b});
    orbitIncomplete_ = false;
    uint32_t tris[4];
    for (size_t k = 0; k < mirrorMaps_.size(); ++k) {
        const uint32_t ma = mirrorOf(k, a), mb = mirrorOf(k, b);
        if (ma == kNone || mb == kNone || ma == mb || edgeTriangles(ma, mb, tris) == 0) {
            orbitIncomplete_ = true;
            continue;
        }
        bool seen = false;
        for (const auto& e : out) seen = seen || (e.first == ma && e.second == mb) || (e.first == mb && e.second == ma);
        if (!seen && edgeTriangles(ma, mb, tris) > 0) out.emplace_back(ma, mb);
    }
}

bool DynamicTopology::orbitSharesTriangle(const std::vector<std::pair<uint32_t, uint32_t>>& edges) const {
    uint32_t ti[4], tj[4];
    for (size_t i = 0; i < edges.size(); ++i) {
        const int ni = std::min(4, edgeTriangles(edges[i].first, edges[i].second, ti));
        for (size_t j = i + 1; j < edges.size(); ++j) {
            const int nj = std::min(4, edgeTriangles(edges[j].first, edges[j].second, tj));
            for (int x = 0; x < ni; ++x) {
                for (int y = 0; y < nj; ++y) {
                    if (ti[x] == tj[y]) return true;
                }
            }
            const auto& e = edges[i];
            const auto& f = edges[j];
            if (e.first == f.first || e.first == f.second || e.second == f.first || e.second == f.second) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

uint32_t DynamicTopology::split(uint32_t a, uint32_t b) {
    Mesh& m = *mesh_;
    if (a >= m_vertTris.size() || b >= m_vertTris.size()) return kNone;

    for (size_t i = 0; i < m_vertTris[a].size(); ++i) {
        uint32_t t = m_vertTris[a][i];
        if (t < (uint32_t)mesh_->nbFaces && !deadT_[t] && isQuad(t)) {
            glm::uvec4 f4 = getFace4(t);
            if ((f4.x == a && (f4.y == b || f4.w == b)) ||
                (f4.y == a && (f4.z == b || f4.x == b)) ||
                (f4.z == a && (f4.w == b || f4.y == b)) ||
                (f4.w == a && (f4.x == b || f4.z == b))) {
                triangulateQuad(t);
                i = (size_t)-1;
            }
        }
    }

    uint32_t tris[4];
    const int n = edgeTriangles(a, b, tris);
    if (n < 1 || n > 2) return kNone;

    const glm::vec3 pa = getPos(a), pb = getPos(b);
    glm::vec3 bend(0.0f);
    if (m.normals.size() == (size_t)m.nbVerts * 3) {
        const glm::vec3 na = getNormal(a), nb = getNormal(b), e = pb - pa;
        bend = (na * glm::dot(e, na) - nb * glm::dot(e, nb)) * -0.125f;
        const float len = glm::length(bend), limit = 0.25f * glm::length(e);
        if (!std::isfinite(len)) bend = glm::vec3(0.0f);
        else if (len > limit) bend *= limit / len;
    }
    const glm::vec3 mid = (pa + pb) * 0.5f + bend;

    const float minArea = 1e-6f * target_ * target_;
    for (int i = 0; i < n; ++i) {
        glm::uvec3 tri = getTri(tris[i]);
        uint32_t c = tri.x;
        for (int k = 0; k < 3; ++k)
            if (tri[k] != a && tri[k] != b) c = tri[k];
        const glm::vec3 pc = getPos(c);
        if (glm::length(glm::cross(mid - pa, pc - pa)) < minArea || glm::length(glm::cross(pb - mid, pc - mid)) < minArea)
            return kNone;
    }
    const uint32_t v = newVertex();
    interpolateVertex(v, a, b, 0.5f, mid);

    for (int i = 0; i < n; ++i) {
        const uint32_t t = tris[i];
        glm::uvec3 tri = getTri(t);
        int k = 0;
        while (k < 3 && !((tri[k] == a && tri[(k + 1) % 3] == b) || (tri[k] == b && tri[(k + 1) % 3] == a))) ++k;
        if (k == 3) continue;
        const uint32_t y = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
        glm::uvec3 first = tri;
        first[(k + 1) % 3] = v;
        setTriangle(t, first);
        newTriangle(glm::uvec3(v, y, c), t);
    }
    ++stats.splits;
    ++ops_;
    return v;
}

void DynamicTopology::pushEdgesAround(uint32_t v, bool longer) {
    ring(v, scratch2_);
    for (uint32_t u : scratch2_) {
        if (deadV_[u] || !candidate(v, u)) continue;
        const float l = len2(v, u);
        if (longer ? l > splitLen2_ : l < collapseLen2_) {
            heap_.push_back({longer ? l : -l, std::min(u, v), std::max(u, v)});
            std::push_heap(heap_.begin(), heap_.end(), [](const Edge& x, const Edge& y) {
                return x.len2 != y.len2 ? x.len2 < y.len2 : x.a != y.a ? x.a > y.a : x.b > y.b;
            });
        }
    }
}

void DynamicTopology::splitOrbit(uint32_t a, uint32_t b, int depth) {
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    orbit(a, b, edges);
    for (const auto& ed : edges) {
        uint32_t ea = ed.first, eb = ed.second;
        if (ea >= m_vertTris.size() || eb >= m_vertTris.size()) continue;
        for (size_t i = 0; i < m_vertTris[ea].size(); ++i) {
            uint32_t t = m_vertTris[ea][i];
            if (t < (uint32_t)mesh_->nbFaces && !deadT_[t] && isQuad(t)) {
                glm::uvec4 f4 = getFace4(t);
                if ((f4.x == ea && (f4.y == eb || f4.w == eb)) ||
                    (f4.y == ea && (f4.z == eb || f4.x == eb)) ||
                    (f4.z == ea && (f4.w == eb || f4.y == eb)) ||
                    (f4.w == ea && (f4.x == eb || f4.z == eb))) {
                    triangulateQuad(t);
                    i = (size_t)-1;
                }
            }
        }
    }
    if (edges.size() > 1 && depth < 3) {
        uint32_t ti[4], tj[4];
        bool again = false;
        for (size_t i = 0; i < edges.size() && !again; ++i) {
            const int ni = std::min(4, edgeTriangles(edges[i].first, edges[i].second, ti));
            for (size_t j = i + 1; j < edges.size() && !again; ++j) {
                const int nj = std::min(4, edgeTriangles(edges[j].first, edges[j].second, tj));
                for (int x = 0; x < ni && !again; ++x) {
                    for (int y = 0; y < nj && !again; ++y) {
                        if (ti[x] != tj[y]) continue;
                        const glm::uvec3 tri = getTri(ti[x]);
                        uint32_t shared = kNone;
                        for (uint32_t c : {edges[i].first, edges[i].second}) {
                            if (c == edges[j].first || c == edges[j].second) shared = c;
                        }
                        if (shared == kNone) continue;
                        uint32_t p = kNone, q = kNone;
                        for (int c = 0; c < 3; ++c) {
                            if (tri[c] != shared) (p == kNone ? p : q) = tri[c];
                        }
                        if (p != kNone && q != kNone && !locked(p) && !locked(q)) {
                            splitOrbit(p, q, depth + 1);
                            again = true;
                        }
                    }
                }
            }
        }
        if (again) orbit(a, b, edges);
    }
    if (orbitIncomplete_ && !mirrorMaps_.empty()) ++stats.unpaired;
    std::vector<uint32_t> made(edges.size(), kNone);
    uint32_t tris[4];
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto [x, y] = edges[i];
        const int n = edgeTriangles(x, y, tris);
        if (n < 1 || n > 2 || locked(x) || locked(y)) continue;
        made[i] = split(x, y);
    }
    for (size_t k = 0; k < mirrorMaps_.size(); ++k) {
        for (size_t i = 0; i < edges.size(); ++i) {
            if (made[i] == kNone) continue;
            const uint32_t ma = mirrorOf(k, edges[i].first), mb = mirrorOf(k, edges[i].second);
            for (size_t j = 0; j < edges.size(); ++j) {
                if (made[j] != kNone && ((edges[j].first == ma && edges[j].second == mb) ||
                                         (edges[j].first == mb && edges[j].second == ma))) {
                    setMirror(k, made[i], made[j]);
                }
            }
        }
    }
    for (uint32_t v : made) {
        if (v != kNone) pushEdgesAround(v, true);
    }
}

void DynamicTopology::splitPass() {
    heap_.clear();
    uint32_t tris[4];
    for (uint32_t t : regionTris_) {
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = tri[c], b = tri[(c + 1) % 3];
            if (a > b && edgeTriangles(a, b, tris) != 1) continue;
            const float l = len2(a, b);
            if (l > splitLen2_ && inside(a, b)) heap_.push_back({l, std::min(a, b), std::max(a, b)});
        }
    }
    for (uint32_t t : mirrorTris_) {
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = tri[c], b = tri[(c + 1) % 3];
            if (a > b && edgeTriangles(a, b, tris) != 1) continue;
            const float l = len2(a, b);
            if (l > splitLen2_ && !inside(a, b) && candidate(a, b)) heap_.push_back({l, std::min(a, b), std::max(a, b)});
        }
    }
    auto less = [](const Edge& x, const Edge& y) {
        return x.len2 != y.len2 ? x.len2 < y.len2 : x.a != y.a ? x.a > y.a : x.b > y.b;
    };
    std::make_heap(heap_.begin(), heap_.end(), less);
    while (!heap_.empty() && ops_ < maxOps_) {
        std::pop_heap(heap_.begin(), heap_.end(), less);
        const Edge e = heap_.back();
        heap_.pop_back();
        if (deadV_[e.a] || deadV_[e.b]) continue;
        const int n = edgeTriangles(e.a, e.b, tris);
        if (n < 1 || n > 2 || len2(e.a, e.b) <= splitLen2_) continue;
        if (locked(e.a) || locked(e.b)) continue;
        splitOrbit(e.a, e.b, 0);
    }
}

bool DynamicTopology::collapseTarget(uint32_t a, uint32_t b, uint32_t& keep, uint32_t& drop, glm::vec3& p) const {
    bool qa = hasIncidentQuad(a);
    bool qb = hasIncidentQuad(b);
    if (qa && qb) return false;
    if (qa && !qb) {
        keep = a; drop = b; p = getPos(a);
        return true;
    }
    if (qb && !qa) {
        keep = b; drop = a; p = getPos(b);
        return true;
    }

    uint32_t oa[2], ob[2];
    const int fa = featureEdges(a, oa), fb = featureEdges(b, ob);
    if (fa == 0 && fb == 0) {
        keep = std::min(a, b);
        drop = std::max(a, b);
        p = (getPos(a) + getPos(b)) * 0.5f;
        return true;
    }
    if (fa == 0) {
        keep = b; drop = a; p = getPos(b);
        return true;
    }
    if (fb == 0) {
        keep = a; drop = b; p = getPos(a);
        return true;
    }
    if (!featureEdge(a, b)) return false;
    if (fa == 2 && (oa[0] == b || oa[1] == b)) {
        keep = b; drop = a; p = getPos(b);
        return true;
    }
    if (fb == 2 && (ob[0] == a || ob[1] == a)) {
        keep = a; drop = b; p = getPos(a);
        return true;
    }
    return false;
}

bool DynamicTopology::canCollapse(uint32_t keep, uint32_t drop, const glm::vec3& p) const {
    if (hasIncidentQuad(drop)) return false;
    const Mesh& m = *mesh_;
    uint32_t tris[4];
    const int n = edgeTriangles(keep, drop, tris);
    if (n < 1 || n > 2) return false;
    if (isQuad(tris[0]) || (n == 2 && isQuad(tris[1]))) return false;
    uint32_t opposite[2] = {kNone, kNone};
    for (int i = 0; i < n; ++i) {
        glm::uvec3 tri = getTri(tris[i]);
        for (int c = 0; c < 3; ++c)
            if (tri[c] != keep && tri[c] != drop) opposite[i] = tri[c];
        if (opposite[i] == kNone) return false;
    }
    if (n == 2 && opposite[0] == opposite[1]) return false;
    std::vector<uint32_t>& rk = ringA_;
    std::vector<uint32_t>& rd = ringB_;
    ring(keep, rk);
    ring(drop, rd);
    // Link condition
    size_t common = 0;
    for (uint32_t u : rk) {
        if (std::binary_search(rd.begin(), rd.end(), u)) {
            if (u != opposite[0] && u != opposite[1]) return false;
            ++common;
        }
    }
    if (common != size_t(n)) return false;
    if (rk.size() + rd.size() - size_t(n) - 2 < 3) return false;
    std::vector<uint32_t>& ro = ringC_;
    for (int i = 0; i < n; ++i) {
        ring(opposite[i], ro);
        if (ro.size() <= 3) return false;
    }
    if (n == 1 && (rk.size() < 3 || rd.size() < 3)) return false;

    for (uint32_t end : {keep, drop}) {
        for (uint32_t u : (end == keep ? rk : rd)) {
            if (u != keep && u != drop) {
                const glm::vec3 d = getPos(u) - p;
                const float l = glm::dot(d, d);
                if (l >= splitLen2_ && l > len2(u, end) * 1.0001f) return false;
            }
        }
    }
    const float minArea = 1e-4f * target_ * target_;
    for (uint32_t v : {keep, drop}) {
        if (v >= m_vertTris.size()) continue;
        for (uint32_t t : m_vertTris[v]) {
            if (t == tris[0] || (n == 2 && t == tris[1])) continue;
            if (isQuad(t)) continue;
            glm::uvec3 tri = getTri(t);
            glm::vec3 q[3], o[3];
            for (int c = 0; c < 3; ++c) {
                o[c] = getPos(tri[c]);
                q[c] = tri[c] == keep || tri[c] == drop ? p : o[c];
            }
            const glm::vec3 nOld = glm::cross(o[1] - o[0], o[2] - o[0]);
            const glm::vec3 nNew = glm::cross(q[1] - q[0], q[2] - q[0]);
            const float lNew = glm::length(nNew), lOld = glm::length(nOld);
            if (lNew < minArea && lNew < lOld * 0.999f) return false;
            if (lOld >= minArea && glm::dot(nOld, nNew) <= 0.3f * lOld * lNew) return false;
            const float qNew = quality(q[0], q[1], q[2]);
            if (qNew < 0.1f && qNew < quality(o[0], o[1], o[2])) return false;
        }
    }
    return true;
}

void DynamicTopology::collapse(uint32_t keep, uint32_t drop, const glm::vec3& p) {
    uint32_t tris[4];
    const int n = edgeTriangles(keep, drop, tris);
    for (int i = 0; i < n && i < 2; ++i) killTriangle(tris[i]);
    const auto& around = m_vertTris[drop];
    std::vector<uint32_t>& list = ringA_;
    list.assign(around.begin(), around.end());
    for (uint32_t t : list) {
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            if (tri[c] == drop) tri[c] = keep;
        }
        setTriangle(t, tri);
    }
    const bool midpoint = (p != getPos(keep));
    if (midpoint) interpolateVertex(keep, keep, drop, 0.5f, p);
    markVertex(keep);
    killVertex(drop);
    ++stats.collapses;
    ++ops_;
}

void DynamicTopology::collapsePass() {
    heap_.clear();
    uint32_t tris[4];
    for (uint32_t t : regionTris_) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = tri[c], b = tri[(c + 1) % 3];
            if (a > b && edgeTriangles(a, b, tris) != 1) continue;
            const float l = len2(a, b);
            if (l < collapseLen2_ && inside(a, b)) heap_.push_back({-l, std::min(a, b), std::max(a, b)});
        }
    }
    for (uint32_t t : mirrorTris_) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = tri[c], b = tri[(c + 1) % 3];
            if (a > b && edgeTriangles(a, b, tris) != 1) continue;
            const float l = len2(a, b);
            if (l < collapseLen2_ && !inside(a, b) && candidate(a, b)) heap_.push_back({-l, std::min(a, b), std::max(a, b)});
        }
    }
    for (uint32_t t : slivers_) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        glm::uvec3 tri = getTri(t);
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = tri[c], b = tri[(c + 1) % 3];
            const float l = len2(a, b);
            if (l < collapseLen2_ && inside(a, b, true)) heap_.push_back({-l, std::min(a, b), std::max(a, b)});
        }
    }
    auto less = [](const Edge& x, const Edge& y) {
        return x.len2 != y.len2 ? x.len2 < y.len2 : x.a != y.a ? x.a > y.a : x.b > y.b;
    };
    std::make_heap(heap_.begin(), heap_.end(), less);
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    struct Plan {
        uint32_t keep, drop;
        glm::vec3 p;
    };
    std::vector<Plan> plans;
    while (!heap_.empty() && ops_ < maxOps_) {
        std::pop_heap(heap_.begin(), heap_.end(), less);
        const Edge e = heap_.back();
        heap_.pop_back();
        if (deadV_[e.a] || deadV_[e.b]) continue;
        const int n = edgeTriangles(e.a, e.b, tris);
        if (n < 1 || n > 2 || len2(e.a, e.b) >= collapseLen2_) continue;
        if (locked(e.a) || locked(e.b)) continue;
        orbit(e.a, e.b, edges);
        if (edges.size() > 1 && orbitSharesTriangle(edges)) continue;
        plans.clear();
        bool ok = true;
        for (const auto& [x, y] : edges) {
            Plan pl;
            if (locked(x) || locked(y) || !collapseTarget(x, y, pl.keep, pl.drop, pl.p) || !canCollapse(pl.keep, pl.drop, pl.p)) {
                ok = false;
                break;
            }
            plans.push_back(pl);
        }
        if (!ok) continue;
        if (orbitIncomplete_ && !mirrorMaps_.empty()) ++stats.unpaired;
        for (size_t i = 0; i < plans.size(); ++i) {
            const Plan& pl = plans[i];
            if (deadV_[pl.keep] || deadV_[pl.drop]) continue;
            if (i > 0 && !canCollapse(pl.keep, pl.drop, pl.p)) continue;
            collapse(pl.keep, pl.drop, pl.p);
            pushEdgesAround(pl.keep, false);
        }
    }
}

bool DynamicTopology::flipGain(uint32_t a, uint32_t b, uint32_t& c, uint32_t& d) const {
    uint32_t tris[4];
    if (edgeTriangles(a, b, tris) != 2 || featureEdge(a, b)) return false;
    c = d = kNone;
    for (int i = 0; i < 2; ++i) {
        glm::uvec3 tri = getTri(tris[i]);
        int k = 0;
        while (k < 3 && tri[k] != a) ++k;
        if (k == 3) return false;
        const uint32_t next = tri[(k + 1) % 3], third = tri[(k + 2) % 3];
        if (next == b) c = third;
        else d = next;
    }
    if (c == kNone || d == kNone || c == d) return false;
    uint32_t tmp[4];
    if (edgeTriangles(c, d, tmp) > 0) return false;

    const glm::vec3 pa = getPos(a), pb = getPos(b), pc = getPos(c), pd = getPos(d);
    const glm::vec3 n1 = glm::cross(pb - pa, pc - pa), n2 = glm::cross(pa - pb, pd - pb);
    const float l1 = glm::length(n1), l2 = glm::length(n2);
    const glm::vec3 m1 = glm::cross(pa - pc, pd - pc), m2 = glm::cross(pb - pd, pc - pd);
    const float k1 = glm::length(m1), k2 = glm::length(m2);
    const float minArea = 1e-4f * target_ * target_;
    if (k1 < minArea || k2 < minArea) return false;
    const float qOld = std::min(quality(pa, pb, pc), quality(pb, pa, pd));
    const float qNew = std::min(quality(pc, pa, pd), quality(pd, pb, pc));
    const glm::vec3 nSum = n1 + n2;
    if (glm::dot(m1, nSum) <= 0.0f || glm::dot(m2, nSum) <= 0.0f) return false;

    if (qOld < 0.05f) {
        ring(a, ringC_);
        if (ringC_.size() <= 3) return false;
        ring(b, ringC_);
        if (ringC_.size() <= 3) return false;
        return qNew > 2.0f * qOld + 0.02f && glm::dot(m1, m2) > 0.5f * k1 * k2;
    }
    if (glm::dot(n1, n2) <= 0.94f * l1 * l2) return false;
    if (glm::dot(m1, m2) <= 0.9f * k1 * k2) return false;

    std::vector<uint32_t>& r = ringC_;
    auto valence = [&](uint32_t v, int& target) {
        ring(v, r);
        target = boundaryVertex(v) ? 4 : 6;
        return int(r.size());
    };
    int ta, tb, tc, td;
    const int va = valence(a, ta), vb = valence(b, tb), vc = valence(c, tc), vd = valence(d, td);
    if (va <= 3 || vb <= 3) return false;
    auto sq = [](int x) { return x * x; };
    const int before = sq(va - ta) + sq(vb - tb) + sq(vc - tc) + sq(vd - td);
    const int after = sq(va - 1 - ta) + sq(vb - 1 - tb) + sq(vc + 1 - tc) + sq(vd + 1 - td);
    if (after >= before) return false;
    return qNew >= 0.5f * qOld;
}

void DynamicTopology::flip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t tris[4];
    if (edgeTriangles(a, b, tris) != 2) return;
    uint32_t t1 = tris[0], t2 = tris[1];
    {
        glm::uvec3 tri = getTri(t1);
        int k = 0;
        while (k < 3 && tri[k] != a) ++k;
        if (tri[(k + 1) % 3] != b) std::swap(t1, t2);
    }
    setTriangle(t1, glm::uvec3(c, a, d));
    setTriangle(t2, glm::uvec3(d, b, c));
    ++stats.flips;
    ++ops_;
}

void DynamicTopology::flipPass() {
    std::vector<std::pair<uint32_t, uint32_t>> candidates;
    for (uint32_t v : changedV_) {
        if (deadV_[v] || v >= m_vertTris.size()) continue;
        for (uint32_t t : m_vertTris[v]) {
            glm::uvec3 tri = getTri(t);
            for (int c = 0; c < 3; ++c) {
                const uint32_t a = tri[c], b = tri[(c + 1) % 3];
                if (a < b && candidate(a, b)) candidates.emplace_back(a, b);
            }
        }
    }
    for (uint32_t t : slivers_) {
        if (t >= (uint32_t)mesh_->nbFaces || deadT_[t]) continue;
        glm::uvec3 tri = getTri(t);
        if (quality(getPos(tri.x), getPos(tri.y), getPos(tri.z)) >= 0.05f) continue;
        int longest = 0;
        for (int c = 1; c < 3; ++c) {
            if (len2(tri[c], tri[(c + 1) % 3]) > len2(tri[longest], tri[(longest + 1) % 3])) longest = c;
        }
        const uint32_t a = tri[longest], b = tri[(longest + 1) % 3];
        candidates.emplace_back(std::min(a, b), std::max(a, b));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    struct Plan {
        uint32_t a, b, c, d;
    };
    std::vector<Plan> plans;
    for (const auto& [a0, b0] : candidates) {
        if (ops_ >= maxOps_) break;
        if (deadV_[a0] || deadV_[b0] || locked(a0) || locked(b0)) continue;
        uint32_t c, d;
        if (!flipGain(a0, b0, c, d)) continue;
        orbit(a0, b0, edges);
        if (edges.size() > 1 && orbitSharesTriangle(edges)) continue;
        plans.clear();
        bool ok = true;
        for (const auto& [x, y] : edges) {
            Plan pl{x, y, 0, 0};
            if (locked(x) || locked(y) || !flipGain(x, y, pl.c, pl.d)) {
                ok = false;
                break;
            }
            plans.push_back(pl);
        }
        if (!ok) continue;
        if (orbitIncomplete_ && !mirrorMaps_.empty()) ++stats.unpaired;
        for (const Plan& pl : plans) flip(pl.a, pl.b, pl.c, pl.d);
    }
}

void DynamicTopology::relaxPass() {
    std::vector<std::pair<uint32_t, glm::vec3>> moves;
    std::vector<uint32_t>& r = ringC_;
    uint32_t other[2];
    const float r2 = radius_ * radius_;
    for (uint32_t v : changedV_) {
        if (deadV_[v] || locked(v) || hasIncidentQuad(v)) continue;
        const glm::vec3 pv = getPos(v);
        bool in = glm::dot(pv - center_, pv - center_) <= r2;
        for (const SymCopy& s : mirrorSigns_) {
            const glm::vec3 d0 = pv - s.point(center_);
            in = in || glm::dot(d0, d0) <= r2;
        }
        if (!in) continue;
        if (featureEdges(v, other) != 0) continue;
        ring(v, r);
        if (r.size() < 3) continue;
        glm::vec3 avg(0.0f);
        for (uint32_t u : r) avg += getPos(u);
        avg /= float(r.size());
        const glm::vec3 n = faceNormalAt(v);
        glm::vec3 d = avg - pv;
        d -= n * glm::dot(d, n);
        moves.emplace_back(v, d * 0.5f);
    }
    const float minArea = 1e-4f * target_ * target_;
    for (const auto& [v, d] : moves) {
        const glm::vec3 to = getPos(v) + d;
        bool ok = true;
        if (v >= m_vertTris.size()) continue;
        for (uint32_t t : m_vertTris[v]) {
            glm::uvec3 tri = getTri(t);
            glm::vec3 o[3], q[3];
            for (int c = 0; c < 3; ++c) {
                o[c] = getPos(tri[c]);
                q[c] = (tri[c] == v ? to : o[c]);
            }
            const glm::vec3 nOld = glm::cross(o[1] - o[0], o[2] - o[0]), nNew = glm::cross(q[1] - q[0], q[2] - q[0]);
            const float lNew = glm::length(nNew);
            if (lNew < minArea || glm::dot(nOld, nNew) <= 0.5f * glm::length(nOld) * lNew) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        recordVertex(v);
        setPos(v, to);
    }
}

void DynamicTopology::gatherRegion() {
    queryRegion();
    if (mirrorSigns_.empty()) return;

    std::vector<uint32_t>& verts = regionVerts_;
    std::vector<uint32_t> tris;
    verts.clear();
    if (seen_.size() < (size_t)mesh_->nbVerts) seen_.resize(mesh_->nbVerts + mesh_->nbVerts / 4, 0);
    auto addCorners = [&](const std::vector<uint32_t>& ts) {
        for (uint32_t t : ts) {
            glm::uvec3 tri = getTri(t);
            for (int c = 0; c < 3; ++c) {
                const uint32_t v = tri[c];
                if (v >= seen_.size() || seen_[v] == mirrorStamp_) continue;
                seen_[v] = mirrorStamp_;
                verts.push_back(v);
            }
        }
    };

    const float reach = radius_ * 1.25f + target_ * 2.0f;
    bvh_->queryTriangles(*mesh_, center_, reach, tris);
    addCorners(tris);
    for (const SymCopy& s : mirrorSigns_) {
        bvh_->queryTriangles(*mesh_, s.point(center_), reach, tris);
        addCorners(tris);
    }

    std::vector<float> lengths;
    const size_t stride = std::max<size_t>(1, regionTris_.size() / 1024);
    for (size_t i = 0; i < regionTris_.size(); i += stride) {
        glm::uvec3 tri = getTri(regionTris_[i]);
        lengths.push_back(glm::distance(getPos(tri.x), getPos(tri.y)));
    }
    float typical = target_;
    if (!lengths.empty()) {
        std::nth_element(lengths.begin(), lengths.begin() + std::ptrdiff_t(lengths.size() / 2), lengths.end());
        typical = std::min(typical, lengths[lengths.size() / 2]);
    }
    eps_ = std::max(typical * 0.3f, 1e-7f);
    matchMirrors(verts);
}

bool DynamicTopology::remesh(Mesh& mesh, BVH& bvh, const glm::vec3& center, float radius, float edgeLength,
                             std::span<const SymCopy> mirrors) {
    const auto clock = std::chrono::steady_clock::now();
    lastRemeshProfile = DyntopoRemeshProfile{};
    const size_t splitsBefore = stats.splits;
    const size_t collapsesBefore = stats.collapses;
    const size_t flipsBefore = stats.flips;
    moved_.clear();
    recordedNow_.clear();
    if (!dyntopoBlocker(mesh).empty() || !(edgeLength > 0.0f) || !(radius > 0.0f)) return false;
    if (mesh_ && mesh_ != &mesh) return false;

    if (!mesh_) {
        const auto tInit0 = std::chrono::steady_clock::now();
        mesh_ = &mesh;
        bvh_ = &bvh;
        startVertices_ = (uint32_t)mesh.nbVerts;
        startTriangles_ = (uint32_t)mesh.nbFaces;
        undo_ = std::make_unique<TopologyUndo>();
        vertRec_.assign((mesh.nbVerts + 63) / 64, 0);
        triRec_.assign((mesh.nbFaces + 63) / 64, 0);
        deadV_.assign(mesh.nbVerts, 0);
        deadT_.assign(mesh.nbFaces, 0);
        changedVFlag_.assign(mesh.nbVerts, 0);

        std::vector<uint32_t> degrees(mesh.nbVerts, 0);
        for (uint32_t t = 0; t < (uint32_t)mesh.nbFaces; ++t) {
            const uint32_t* f = &mesh.faces[t * 4];
            for (int c = 0; c < 4; ++c) {
                if (f[c] != 0xFFFFFFFF && f[c] < (uint32_t)mesh.nbVerts) degrees[f[c]]++;
            }
        }
        m_vertTris.init(degrees);
        for (uint32_t t = 0; t < (uint32_t)mesh.nbFaces; ++t) {
            const uint32_t* f = &mesh.faces[t * 4];
            for (int c = 0; c < 4; ++c) {
                if (f[c] != 0xFFFFFFFF && f[c] < (uint32_t)mesh.nbVerts) m_vertTris.appendInit(f[c], t);
            }
        }
        for (size_t v = 0; v < m_vertTris.size(); ++v) {
            m_vertTris.sortVert((uint32_t)v);
        }
        lastRemeshProfile.initAdjacencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tInit0).count();
    }

    center_ = center;
    radius_ = radius;
    target_ = edgeLength;
    splitLen2_ = (edgeLength * 4.0f / 3.0f) * (edgeLength * 4.0f / 3.0f);
    collapseLen2_ = (edgeLength * 0.6f) * (edgeLength * 0.6f);

    std::vector<SymCopy> fullCopies;
    if (!mirrors.empty()) {
        if (!mirrors.front().identity()) {
            fullCopies.push_back(SymCopy{});
        }
        fullCopies.insert(fullCopies.end(), mirrors.begin(), mirrors.end());
    }
    mirrorSigns_.assign(fullCopies.size() > 1 ? fullCopies.begin() + 1 : fullCopies.end(), fullCopies.end());
    mirrorInverse_.clear();
    if (!mirrorSigns_.empty()) {
        const std::vector<size_t> inv = symmetryInverses(fullCopies);
        for (size_t k = 1; k < inv.size(); ++k) mirrorInverse_.push_back(inv[k] > 0 ? inv[k] - 1 : k - 1);
    }
    mirrorMaps_.resize(mirrorSigns_.size());
    if (++mirrorStamp_ == 0) {
        for (MirrorMap& map : mirrorMaps_) std::fill(map.stamp.begin(), map.stamp.end(), 0);
        mirrorStamp_ = 1;
    }
    ops_ = 0;
    maxOps_ = 6000;

    auto tStage = std::chrono::steady_clock::now();
    gatherRegion();
    lastRemeshProfile.gatherRegionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();
    lastRemeshProfile.regionTris = regionTris_.size();
    if (regionTris_.empty()) {
        if (mesh.vrfStartCount.size() < (size_t)mesh.nbVerts * 2 || mesh.vrvStartCount.size() < (size_t)mesh.nbVerts * 2) {
            syncDynamicCSR(mesh);
        }
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
        lastRemeshProfile.totalMs = lastMs;
        return false;
    }

    tStage = std::chrono::steady_clock::now();
    splitPass();
    lastRemeshProfile.splitPassMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    bvh.refitChanged(mesh);
    lastRemeshProfile.refit1Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    queryRegion();
    lastRemeshProfile.queryRegion2Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    collapsePass();
    lastRemeshProfile.collapsePassMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    flipPass();
    lastRemeshProfile.flipPassMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    relaxPass();
    lastRemeshProfile.relaxPassMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    const bool changed = !changedV_.empty() || !changedT_.empty();
    lastRemeshProfile.changedVerts = changedV_.size();
    lastRemeshProfile.changedTris = changedT_.size();

    tStage = std::chrono::steady_clock::now();
    compact();
    lastRemeshProfile.compactMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    const uint32_t nv = (uint32_t)mesh.nbVerts;
    for (uint32_t v : changedV_) {
        if (v < changedVFlag_.size()) changedVFlag_[v] = 0;
    }
    std::vector<std::pair<uint32_t, uint32_t>> moves(moved_.begin(), moved_.end());
    std::sort(moves.begin(), moves.end());
    std::vector<uint32_t> verts;
    verts.reserve(changedV_.size());
    for (uint32_t v : changedV_) {
        if (v < nv) {
            verts.push_back(v);
            continue;
        }
        const auto it = std::lower_bound(moves.begin(), moves.end(), std::make_pair(v, 0u));
        if (it != moves.end() && it->first == v) verts.push_back(it->second);
    }
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    changedV_.clear();
    changedVFlag_.resize(nv, 0);

    for (uint32_t v : verts) {
        if (v < nv) {
            glm::vec3 n = faceNormalAt(v);
            setNormal(v, n);
            for (uint32_t t : m_vertTris[v]) {
                bvh.touchTriangle(t);
                markTriangle(t);
            }
        }
    }
    lastRemeshProfile.vertNormalsAndTouchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    bvh.refitChanged(mesh);
    lastRemeshProfile.refit2Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    if (!changedT_.empty()) {
        tStage = std::chrono::steady_clock::now();
        mesh.faceNormals.resize(mesh.nbFaces * 3, 0.0f);
        mesh.faceBoxes.resize(mesh.nbFaces * 6, 0.0f);
        mesh.faceCenters.resize(mesh.nbFaces * 3, 0.0f);

        std::sort(changedT_.begin(), changedT_.end());
        changedT_.erase(std::unique(changedT_.begin(), changedT_.end()), changedT_.end());
        while (!changedT_.empty() && changedT_.back() >= (uint32_t)mesh.nbFaces) {
            changedT_.pop_back();
        }

        if (!changedT_.empty()) {
            updateFaceNormalsAndBoxes(
                mesh.verts.data(), mesh.nbVerts,
                mesh.faces.data(), mesh.nbFaces,
                changedT_.data(), (int)changedT_.size(),
                mesh.faceNormals.data(),
                mesh.faceBoxes.data(),
                mesh.faceCenters.data()
            );
        }
        lastRemeshProfile.fullFaceNormalsBoxesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();
    }
    changedT_.clear();

    tStage = std::chrono::steady_clock::now();
    // Keep dynamic CSR in sync so that any sculpting brush or test accessing vrf/vrv never hits OOB
    syncDynamicCSR(mesh);
    lastRemeshProfile.syncCsrMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    if (ops_ > 0) {
        if (!mesh.edgeIsCage.empty()) mesh.edgeIsCage.clear();
        if (!mesh.faceEdges.empty()) mesh.faceEdges.clear();
    }

    lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
    lastRemeshProfile.totalMs = lastMs;
    lastRemeshProfile.splits = stats.splits - splitsBefore;
    lastRemeshProfile.collapses = stats.collapses - collapsesBefore;
    lastRemeshProfile.flips = stats.flips - flipsBefore;
    return changed;
}

void DynamicTopology::syncDynamicCSR(Mesh& mesh) {
    if (mesh.nbVerts <= 0 || mesh.nbFaces <= 0) {
        mesh.vrfStartCount.clear();
        mesh.vertRingFace.clear();
        mesh.vrvStartCount.clear();
        mesh.vertRingVert.clear();
        mesh.vertOnEdge.clear();
        mesh.isTopologyDirty = false;
        return;
    }

    computeTopology(
        mesh.nbVerts, mesh.faces.data(), mesh.nbFaces,
        mesh.vrfStartCount, mesh.vertRingFace,
        mesh.vrvStartCount, mesh.vertRingVert,
        mesh.vertOnEdge
    );
    mesh.isTopologyDirty = false;
}

std::unique_ptr<TopologyUndo> DynamicTopology::finish(Mesh& mesh, BVH& bvh) {
    if (!mesh_) return nullptr;
    const auto tFinish0 = std::chrono::steady_clock::now();
    lastFinishProfile = DyntopoFinishProfile{};

    auto tStage = std::chrono::steady_clock::now();
    // Keep dynamic CSR up-to-date incrementally without heavy edge graph rebuild
    syncDynamicCSR(mesh);
    lastFinishProfile.initTopologyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();
    mesh.isTopologyDirty = false;
    lastFinishProfile.initTopologyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    mesh.vertProxy = mesh.verts;
    lastFinishProfile.vertProxyCopyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    lastFinishProfile.faceNormalsBoxesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    lastFinishProfile.vertNormalsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    tStage = std::chrono::steady_clock::now();
    if (bvh.isDegraded()) {
        bvh.build(mesh);
    }
    lastFinishProfile.bvhBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();

    mesh.isDirty = true;
    mesh.isVertexDirty = true;
    mesh.dirtyVertMin = 0;
    mesh.dirtyVertMax = mesh.nbVerts > 0 ? (uint32_t)mesh.nbVerts - 1 : 0;

    tStage = std::chrono::steady_clock::now();
    std::unique_ptr<TopologyUndo> undo = std::move(undo_);
    if (undo) {
        undo->vertexCount = startVertices_;
        undo->triangleCount = startTriangles_;
    }
    mesh_ = nullptr;
    bvh_ = nullptr;
    m_vertTris.clear();
    std::vector<uint64_t>().swap(vertRec_);
    std::vector<uint64_t>().swap(triRec_);
    std::vector<uint8_t>().swap(deadV_);
    std::vector<uint8_t>().swap(deadT_);
    std::vector<uint8_t>().swap(changedVFlag_);
    std::vector<uint64_t>().swap(gridKeys_);
    std::vector<uint32_t>().swap(gridHead_);
    std::vector<uint32_t>().swap(gridNext_);
    std::vector<uint32_t>().swap(regionVerts_);
    lastFinishProfile.cleanupMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStage).count();
    lastFinishProfile.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tFinish0).count();
    if (!undo || (undo->vertexSlots.empty() && undo->triangleSlots.empty())) return nullptr;
    return undo;
}

} // namespace dynamic
