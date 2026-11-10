#pragma once

#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DyntopoSettings.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dynamic {

struct SymCopy {
    glm::mat3 m{1.0f};
    glm::vec3 pivot{0.0f};
    glm::vec3 point(const glm::vec3& p) const { return pivot + m * (p - pivot); }
    glm::vec3 dir(const glm::vec3& d) const { return m * d; }
    glm::vec3 inversePoint(const glm::vec3& p) const { return pivot + glm::transpose(m) * (p - pivot); }
    glm::vec3 inverseDir(const glm::vec3& d) const { return glm::transpose(m) * d; }
    float handedness() const { return glm::determinant(m) < 0.0f ? -1.0f : 1.0f; }
    bool identity() const { return m == glm::mat3(1.0f); }
};

std::vector<size_t> symmetryInverses(std::span<const SymCopy> copies);

inline std::string dyntopoBlocker(const Mesh& mesh) {
    if (mesh.hasLayers())
        return "Dynamic topology cannot remesh a mesh with active layers. Flatten layers first.";
    if (mesh.isArmature)
        return "Armature meshes cannot be remeshed directly.";
    if (mesh.nbFaces == 0)
        return "The mesh has no faces.";
    return {};
}

struct TopologyVertexValues {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> colors;
    std::vector<glm::vec3> materials;
    std::vector<uint8_t> visible;
    size_t bytes() const {
        return positions.size() * sizeof(glm::vec3) +
               colors.size() * sizeof(glm::vec3) +
               materials.size() * sizeof(glm::vec3) +
               visible.size() * sizeof(uint8_t);
    }
};

struct TopologyTriangleValues {
    std::vector<glm::uvec4> triangles;
    std::vector<uint32_t> faceGroups;
    std::vector<uint8_t> visible;
    size_t bytes() const {
        return triangles.size() * sizeof(glm::uvec4) +
               faceGroups.size() * sizeof(uint32_t) +
               visible.size() * sizeof(uint8_t);
    }
};

struct TopologyUndo {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    std::vector<uint32_t> vertexSlots;
    std::vector<uint32_t> triangleSlots;
    TopologyVertexValues vertices;
    TopologyTriangleValues triangles;

    size_t bytes() const {
        return vertices.bytes() + triangles.bytes() +
               vertexSlots.size() * sizeof(uint32_t) +
               triangleSlots.size() * sizeof(uint32_t);
    }

    void swapWith(Mesh& mesh);
};

struct DyntopoStats {
    size_t splits = 0;
    size_t collapses = 0;
    size_t flips = 0;
    size_t unpaired = 0;
};

struct DyntopoRemeshProfile {
    double initAdjacencyMs = 0.0;
    double gatherRegionMs = 0.0;
    double splitPassMs = 0.0;
    double refit1Ms = 0.0;
    double queryRegion2Ms = 0.0;
    double collapsePassMs = 0.0;
    double flipPassMs = 0.0;
    double relaxPassMs = 0.0;
    double compactMs = 0.0;
    double vertNormalsAndTouchMs = 0.0;
    double refit2Ms = 0.0;
    double fullFaceNormalsBoxesMs = 0.0;
    double syncCsrMs = 0.0;
    double totalMs = 0.0;
    size_t splits = 0;
    size_t collapses = 0;
    size_t flips = 0;
    size_t regionTris = 0;
    size_t changedVerts = 0;
    size_t changedTris = 0;
};

struct DyntopoFinishProfile {
    double initTopologyMs = 0.0;
    double vertProxyCopyMs = 0.0;
    double faceNormalsBoxesMs = 0.0;
    double vertNormalsMs = 0.0;
    double bvhBuildMs = 0.0;
    double cleanupMs = 0.0;
    double totalMs = 0.0;
};

class DynamicTopology {
public:
    DynamicTopology() = default;
    ~DynamicTopology() = default;

    bool remesh(Mesh& mesh, BVH& bvh, const glm::vec3& center, float radius, float edgeLength,
                std::span<const SymCopy> mirrors = {}, bool needCsrSync = true);

    std::span<const std::pair<uint32_t, uint32_t>> movedVertices() const { return moved_; }
    std::span<const uint32_t> recordedVertices() const { return recordedNow_; }
    bool active() const { return mesh_ != nullptr; }

    void recordStrokeStart(uint32_t v) {
        if (mesh_ && v < (uint32_t)mesh_->nbVerts) recordVertex(v);
    }

    bool recordedVertex(uint32_t v) const {
        return v < vertRec_.size() * 64 && ((vertRec_[v >> 6] >> (v & 63)) & 1);
    }

    std::unique_ptr<TopologyUndo> finish(Mesh& mesh, BVH& bvh);
    void syncDynamicCSR(Mesh& mesh);
    Mesh* strokeMesh() const { return mesh_; }
    BVH* strokeBvh() const { return bvh_; }

    DyntopoStats stats;
    double lastMs = 0.0;
    DyntopoRemeshProfile lastRemeshProfile;
    DyntopoFinishProfile lastFinishProfile;

private:
    struct Edge {
        float len2;
        uint32_t a, b;
    };

    // Mesh attribute helpers:
    glm::vec3 getPos(uint32_t v) const {
        const float* p = &mesh_->verts[v * 3];
        return glm::vec3(p[0], p[1], p[2]);
    }
    void setPos(uint32_t v, const glm::vec3& p) {
        float* dst = &mesh_->verts[v * 3];
        dst[0] = p.x; dst[1] = p.y; dst[2] = p.z;
        if (!mesh_->vertProxy.empty() && v * 3 + 2 < mesh_->vertProxy.size()) {
            float* pxy = &mesh_->vertProxy[v * 3];
            pxy[0] = p.x; pxy[1] = p.y; pxy[2] = p.z;
        }
    }
    glm::vec3 getNormal(uint32_t v) const {
        if (mesh_->normals.empty() || v * 3 + 2 >= mesh_->normals.size()) return glm::vec3(0.0f, 1.0f, 0.0f);
        const float* n = &mesh_->normals[v * 3];
        return glm::vec3(n[0], n[1], n[2]);
    }
    void setNormal(uint32_t v, const glm::vec3& n) {
        if (mesh_->normals.size() <= v * 3 + 2) mesh_->normals.resize((v + 1) * 3, 0.0f);
        float* dst = &mesh_->normals[v * 3];
        dst[0] = n.x; dst[1] = n.y; dst[2] = n.z;
    }
    glm::vec3 getColor(uint32_t v) const {
        if (mesh_->colors.empty() || v * 3 + 2 >= mesh_->colors.size()) return glm::vec3(1.0f);
        const float* c = &mesh_->colors[v * 3];
        return glm::vec3(c[0], c[1], c[2]);
    }
    void setColor(uint32_t v, const glm::vec3& c) {
        if (mesh_->colors.size() <= v * 3 + 2) mesh_->colors.resize((v + 1) * 3, 1.0f);
        float* dst = &mesh_->colors[v * 3];
        dst[0] = c.x; dst[1] = c.y; dst[2] = c.z;
    }
    glm::vec3 getMaterial(uint32_t v) const {
        if (mesh_->materials.empty() || v * 3 + 2 >= mesh_->materials.size()) return glm::vec3(0.5f, 0.0f, 1.0f);
        const float* m = &mesh_->materials[v * 3];
        return glm::vec3(m[0], m[1], m[2]);
    }
    void setMaterial(uint32_t v, const glm::vec3& m) {
        if (mesh_->materials.size() <= v * 3 + 2) mesh_->materials.resize((v + 1) * 3, 0.0f);
        float* dst = &mesh_->materials[v * 3];
        dst[0] = m.x; dst[1] = m.y; dst[2] = m.z;
    }
    glm::uvec3 getTri(uint32_t t) const {
        const uint32_t* f = &mesh_->faces[t * 4];
        return glm::uvec3(f[0], f[1], f[2]);
    }
    void setTri(uint32_t t, const glm::uvec3& tri) {
        uint32_t* f = &mesh_->faces[t * 4];
        f[0] = tri.x; f[1] = tri.y; f[2] = tri.z; f[3] = 0xFFFFFFFF;
    }
    bool isQuad(uint32_t t) const {
        return mesh_->faces[t * 4 + 3] != 0xFFFFFFFF;
    }
    glm::uvec4 getFace4(uint32_t t) const {
        const uint32_t* f = &mesh_->faces[t * 4];
        return glm::uvec4(f[0], f[1], f[2], f[3]);
    }
    void setFace4(uint32_t t, const glm::uvec4& f4) {
        uint32_t* f = &mesh_->faces[t * 4];
        f[0] = f4.x; f[1] = f4.y; f[2] = f4.z; f[3] = f4.w;
    }
    bool hasIncidentQuad(uint32_t v) const {
        if (v >= m_vertTris.size()) return false;
        for (uint32_t t : m_vertTris[v]) {
            if (!deadT_[t] && isQuad(t)) return true;
        }
        return false;
    }

    uint32_t triangulateQuad(uint32_t q);

    void recordVertex(uint32_t v);
    void recordTriangle(uint32_t t);
    void markVertex(uint32_t v);
    void markTriangle(uint32_t t);

    uint32_t newVertex();
    void interpolateVertex(uint32_t v, uint32_t a, uint32_t b, float t, const glm::vec3& position);
    void killVertex(uint32_t v);

    uint32_t newTriangle(const glm::uvec3& tri, uint32_t like);
    void setTriangle(uint32_t t, const glm::uvec3& tri);
    void killTriangle(uint32_t t);

    void vtInsert(uint32_t v, uint32_t t);
    void vtErase(uint32_t v, uint32_t t);
    void vtReplace(uint32_t v, uint32_t from, uint32_t to);

    void moveVertex(uint32_t from, uint32_t to);
    void moveTriangle(uint32_t from, uint32_t to);
    void compact();

    int edgeTriangles(uint32_t a, uint32_t b, uint32_t* out) const;
    void ring(uint32_t v, std::vector<uint32_t>& out) const;
    bool locked(uint32_t v) const;
    bool planeVertex(uint32_t v) const;
    bool featureEdge(uint32_t a, uint32_t b) const;
    int featureEdges(uint32_t v, uint32_t* other) const;
    bool boundaryVertex(uint32_t v) const;
    float len2(uint32_t a, uint32_t b) const;
    bool inside(uint32_t a, uint32_t b, bool mirrored = false) const;
    bool candidate(uint32_t a, uint32_t b) const;
    glm::vec3 faceNormalAt(uint32_t v) const;

    uint32_t mirrorOf(size_t mirror, uint32_t v) const;
    void setMirror(size_t mirror, uint32_t v, uint32_t w);
    void matchMirrors(const std::vector<uint32_t>& verts);
    void orbit(uint32_t a, uint32_t b, std::vector<std::pair<uint32_t, uint32_t>>& out) const;
    bool orbitSharesTriangle(const std::vector<std::pair<uint32_t, uint32_t>>& edges) const;

    void splitPass();
    void splitOrbit(uint32_t a, uint32_t b, int depth);
    uint32_t split(uint32_t a, uint32_t b);

    void collapsePass();
    bool collapseTarget(uint32_t a, uint32_t b, uint32_t& keep, uint32_t& drop, glm::vec3& p) const;
    bool canCollapse(uint32_t keep, uint32_t drop, const glm::vec3& p) const;
    void collapse(uint32_t keep, uint32_t drop, const glm::vec3& p);

    void flipPass();
    bool flipGain(uint32_t a, uint32_t b, uint32_t& c, uint32_t& d) const;
    void flip(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

    void relaxPass();
    void pushEdgesAround(uint32_t v, bool longer);
    void gatherRegion();
    void queryRegion();

    Mesh* mesh_ = nullptr;
    BVH* bvh_ = nullptr;
    uint32_t startVertices_ = 0, startTriangles_ = 0;
    std::unique_ptr<TopologyUndo> undo_;
    std::vector<uint64_t> vertRec_, triRec_;

    struct FlatVertTris {
        std::vector<uint32_t> offsets;
        std::vector<uint32_t> counts;
        std::vector<uint32_t> caps;
        std::vector<uint32_t> data;

        void clear() {
            offsets.clear();
            counts.clear();
            caps.clear();
            data.clear();
        }

        size_t size() const { return counts.size(); }

        void resize(size_t nv) {
            if (nv > counts.size()) {
                size_t old = counts.size();
                offsets.resize(nv, 0);
                counts.resize(nv, 0);
                caps.resize(nv, 0);
                for (size_t i = old; i < nv; ++i) {
                    offsets[i] = (uint32_t)data.size();
                    counts[i] = 0;
                    caps[i] = 0;
                }
            } else if (nv < counts.size()) {
                offsets.resize(nv);
                counts.resize(nv);
                caps.resize(nv);
            }
        }

        void init(const std::vector<uint32_t>& degrees) {
            const size_t nv = degrees.size();
            offsets.resize(nv);
            counts.assign(nv, 0);
            caps.resize(nv);
            uint32_t total = 0;
            for (size_t i = 0; i < nv; ++i) {
                offsets[i] = total;
                uint32_t c = degrees[i] + 4;
                caps[i] = c;
                total += c;
            }
            data.resize(total);
        }

        void appendInit(uint32_t v, uint32_t t) {
            data[offsets[v] + counts[v]++] = t;
        }

        void sortVert(uint32_t v) {
            if (v < counts.size() && counts[v] > 1) {
                std::sort(data.begin() + offsets[v], data.begin() + offsets[v] + counts[v]);
            }
        }

        std::span<const uint32_t> operator[](uint32_t v) const {
            if (v >= counts.size() || counts[v] == 0) return {};
            return { data.data() + offsets[v], counts[v] };
        }

        void set(uint32_t v, std::span<const uint32_t> vals) {
            if (v >= counts.size()) resize(v + 1);
            uint32_t n = (uint32_t)vals.size();
            if (n > caps[v]) {
                uint32_t newCap = std::max(n + 3, (n + 1) * 2);
                offsets[v] = (uint32_t)data.size();
                caps[v] = newCap;
                data.resize(data.size() + newCap);
            }
            if (n > 0) {
                std::copy(vals.begin(), vals.end(), data.begin() + offsets[v]);
            }
            counts[v] = n;
        }

        void clearVert(uint32_t v) {
            if (v < counts.size()) counts[v] = 0;
        }
    };
    FlatVertTris m_vertTris;

    glm::vec3 center_{0.0f};
    float radius_ = 0.0f, target_ = 0.0f, splitLen2_ = 0.0f, collapseLen2_ = 0.0f, eps_ = 0.0f;
    std::vector<SymCopy> mirrorSigns_;
    std::vector<size_t> mirrorInverse_;

    struct MirrorMap {
        std::vector<uint32_t> to, stamp;
    };
    std::vector<MirrorMap> mirrorMaps_;
    uint32_t mirrorStamp_ = 0;
    std::vector<uint64_t> gridKeys_;
    std::vector<uint32_t> gridHead_, gridNext_;
    std::vector<uint32_t> seen_, regionVerts_;
    std::vector<uint8_t> deadV_, deadT_;
    std::vector<uint32_t> freeV_, freeT_;
    std::vector<uint32_t> changedV_, changedT_, regionTris_, slivers_;
    std::vector<uint32_t> mirrorTris_;
    std::vector<uint8_t> changedVFlag_;
    std::vector<Edge> heap_;
    std::vector<std::pair<uint32_t, uint32_t>> moved_;
    std::vector<uint32_t> recordedNow_;
    mutable bool orbitIncomplete_ = false;
    mutable std::vector<std::pair<uint32_t, uint32_t>> orbitScratch_;
    size_t ops_ = 0, maxOps_ = 0;
    mutable std::vector<uint32_t> scratch_, scratch2_, ringA_, ringB_, ringC_, lockScratch_;
};

} // namespace dynamic

