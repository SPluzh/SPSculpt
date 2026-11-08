#include "editing/SculptManager.h"
#include "platform/HotkeyManager.h"
#include "common/IniFile.h"
#include "brushes/BrushPresetManager.h"
#include "sculpt/SculptEngine.h"
#include "mesh/NormalCalc.h"
#include "editing/ArmatureTool.h"
#include "editing/PoseTool.h"
#include "editing/TubeTool.h"
#include "editing/PrimitiveTool.h"
#include "editing/QuadDrawTool.h"
#include "editing/MaskExtractTool.h"
#include "editing/ComponentSelectTool.h"
#include "editing/EdgeCreaseTool.h"
#include "editing/CurveFoldsTool.h"
#include "editing/ClipCurveTool.h"
#include "editing/TrimTool.h"
#include "editing/SplitTool.h"
#include "editing/undo/UndoManager.h"
#include "common/Logger.h"
#include <glm/gtc/matrix_transform.hpp>
#include "platform/TabletInput.h"
#include "mesh/Topology.h"
#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include <vector>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cmath>
#include <omp.h>


static bool rayTriangleIntersect(
    const glm::vec3& rayOrigin, const glm::vec3& rayDir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t
) {
    const float EPSILON = 0.0000001f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(rayDir, edge2);
    float a = glm::dot(edge1, h);
    if (a > -EPSILON && a < EPSILON)
        return false;
    float f = 1.0f / a;
    glm::vec3 s = rayOrigin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f)
        return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(rayDir, q);
    if (v < 0.0f || u + v > 1.0f)
        return false;
    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

glm::vec3 reflectPointSymmetry(const glm::vec3& pt, const glm::vec3& sScale, const Mesh* mesh, SymmetryMode mode) {
    if (!mesh) return pt * sScale;
    glm::vec3 result = pt;
    for (int axis = 0; axis < 3; ++axis) {
        if (sScale[axis] < 0.0f) {
            glm::vec3 origin = mesh->getSymmetryOriginForAxis(axis, mode);
            glm::vec3 normal = mesh->getSymmetryNormalForAxis(axis, mode);
            float dist = glm::dot(result - origin, normal);
            result -= 2.0f * dist * normal;
        }
    }
    return result;
}

glm::vec3 reflectVectorSymmetry(const glm::vec3& vec, const glm::vec3& sScale, const Mesh* mesh, SymmetryMode mode) {
    if (!mesh) return vec * sScale;
    glm::vec3 result = vec;
    for (int axis = 0; axis < 3; ++axis) {
        if (sScale[axis] < 0.0f) {
            glm::vec3 normal = mesh->getSymmetryNormalForAxis(axis, mode);
            float dist = glm::dot(result, normal);
            result -= 2.0f * dist * normal;
        }
    }
    return result;
}


static inline void radixSortVertices(std::vector<uint32_t>& verts, std::vector<uint32_t>& temp, uint32_t maxV) {
    size_t n = verts.size();
    if (n <= 1) return;
    if (temp.size() < n) temp.resize(n);

    const int maxThreads = omp_get_max_threads();
    const bool inParallel = omp_in_parallel();

    // For maxV < 2^26 (up to 67.1M vertices, covering all meshes up to 50M):
    // 2 passes of 13 bits (8192 buckets = 32 KB, perfectly fits L1d on Ryzen 5800H / Zen 3)
    // Pass 1: bits 0..12 -> temp
    // Pass 2: bits 13..25 -> verts (writes directly into verts, 0 memcpy!)
    if (maxV < (1u << 26)) {
        const uint32_t mask = 8191; // 13 bits
        uint32_t* src = verts.data();
        uint32_t* dst = temp.data();

        if (n >= 32768 && !inParallel && maxThreads > 1) {
            int numT = std::min(maxThreads, 32);
            static std::vector<std::vector<uint32_t>> s_histograms;
            if ((int)s_histograms.size() < numT) {
                s_histograms.resize(numT, std::vector<uint32_t>(8192));
            }

            for (int pass = 0; pass < 2; ++pass) {
                uint32_t shift = pass * 13;

                #pragma omp parallel num_threads(numT)
                {
                    int tid = omp_get_thread_num();
                    auto& hist = s_histograms[tid];
                    std::fill(hist.begin(), hist.end(), 0);

                    #pragma omp for schedule(static)
                    for (int64_t i = 0; i < (int64_t)n; ++i) {
                        uint32_t bucket = (src[i] >> shift) & mask;
                        hist[bucket]++;
                    }
                }

                // Global prefix sum per bucket across threads to guarantee stability:
                std::vector<std::vector<uint32_t>> s_offsets(numT, std::vector<uint32_t>(8192));
                uint32_t runningSum = 0;
                for (uint32_t b = 0; b < 8192; ++b) {
                    for (int t = 0; t < numT; ++t) {
                        s_offsets[t][b] = runningSum;
                        runningSum += s_histograms[t][b];
                    }
                }

                #pragma omp parallel num_threads(numT)
                {
                    int tid = omp_get_thread_num();
                    auto& localOffsets = s_offsets[tid];

                    #pragma omp for schedule(static)
                    for (int64_t i = 0; i < (int64_t)n; ++i) {
                        uint32_t v = src[i];
                        uint32_t bucket = (v >> shift) & mask;
                        dst[localOffsets[bucket]++] = v;
                    }
                }

                std::swap(src, dst);
            }
            return;
        }

        // Sequential 2-pass 13-bit Radix Sort
        // Pass 1: bits 0..12
        {
            uint32_t count[8192] = {0};
            for (size_t i = 0; i < n; ++i) {
                count[src[i] & mask]++;
            }
            uint32_t offset[8192];
            offset[0] = 0;
            for (int i = 1; i < 8192; ++i) {
                offset[i] = offset[i - 1] + count[i - 1];
            }
            for (size_t i = 0; i < n; ++i) {
                uint32_t v = src[i];
                dst[offset[v & mask]++] = v;
            }
        }

        // Pass 2: bits 13..25 -> writes directly into verts (src)
        {
            uint32_t count[8192] = {0};
            for (size_t i = 0; i < n; ++i) {
                count[(dst[i] >> 13) & mask]++;
            }
            uint32_t offset[8192];
            offset[0] = 0;
            for (int i = 1; i < 8192; ++i) {
                offset[i] = offset[i - 1] + count[i - 1];
            }
            for (size_t i = 0; i < n; ++i) {
                uint32_t v = dst[i];
                src[offset[(v >> 13) & mask]++] = v;
            }
        }
        return;
    }

    // Fallback for massive > 67.1M meshes: 3 passes
    uint32_t* src = verts.data();
    uint32_t* dst = temp.data();
    {
        uint32_t count[2048] = {0};
        for (size_t i = 0; i < n; ++i) count[src[i] & 0x7FF]++;
        uint32_t offset[2048];
        offset[0] = 0;
        for (int i = 1; i < 2048; ++i) offset[i] = offset[i - 1] + count[i - 1];
        for (size_t i = 0; i < n; ++i) { uint32_t v = src[i]; dst[offset[v & 0x7FF]++] = v; }
    }
    {
        uint32_t count[2048] = {0};
        for (size_t i = 0; i < n; ++i) count[(dst[i] >> 11) & 0x7FF]++;
        uint32_t offset[2048];
        offset[0] = 0;
        for (int i = 1; i < 2048; ++i) offset[i] = offset[i - 1] + count[i - 1];
        for (size_t i = 0; i < n; ++i) { uint32_t v = dst[i]; src[offset[(v >> 11) & 0x7FF]++] = v; }
    }
    {
        uint32_t count[1024] = {0};
        for (size_t i = 0; i < n; ++i) count[(src[i] >> 22) & 0x3FF]++;
        uint32_t offset[1024];
        offset[0] = 0;
        for (int i = 1; i < 1024; ++i) offset[i] = offset[i - 1] + count[i - 1];
        for (size_t i = 0; i < n; ++i) { uint32_t v = src[i]; dst[offset[(v >> 22) & 0x3FF]++] = v; }
        std::memcpy(verts.data(), temp.data(), n * sizeof(uint32_t));
    }
}

static glm::vec3 vertexOnLine(const glm::vec3& vertex, const glm::vec3& vNear, const glm::vec3& vFar) {
    glm::vec3 ab = vFar - vNear;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-12f) return vNear;
    float dot = glm::dot(ab, vertex - vNear);
    return vNear + ab * (dot / len2);
}

#include <fstream>
#include <sstream>
#include <unordered_map>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static int safe_stoi(const std::string& str, int defaultVal = 0) {
    try {
        return std::stoi(str);
    } catch (...) {
        return defaultVal;
    }
}

static float safe_stof(const std::string& str, float defaultVal = 0.0f) {
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

static void pickVerticesInSphereTopological(
    Mesh* mesh, const glm::vec3& center, float radius2, uint32_t startFaceId,
    std::vector<uint8_t>& topoVisitedFlags, uint8_t& topoVisitedEpoch,
    std::vector<uint32_t>& picked
) {
    picked.clear();
    if (!mesh || startFaceId == 0xffffffff || startFaceId >= (uint32_t)mesh->nbFaces) return;

    uint32_t nbVerts = (uint32_t)mesh->nbVerts;
    if (topoVisitedFlags.size() < (size_t)nbVerts) {
        topoVisitedFlags.assign(nbVerts, uint8_t(0));
    }
    topoVisitedEpoch++;
    if (topoVisitedEpoch == 0) {
        std::fill(topoVisitedFlags.begin(), topoVisitedFlags.end(), uint8_t(0));
        topoVisitedEpoch = 1;
    }
    uint8_t curEpoch = topoVisitedEpoch;
    uint8_t* visited = topoVisitedFlags.data();

    const float* verts = mesh->verts.data();
    const uint8_t* vertVisible = mesh->vertVisible.data();
    const uint32_t* vrvStartCount = mesh->vrvStartCount.data();
    const uint32_t* vertRingVert = mesh->vertRingVert.data();

    if (picked.capacity() < 65536) {
        picked.reserve(131072);
    }

    const uint32_t* fv = mesh->faces.data() + (startFaceId << 2);
    for (int i = 0; i < 4; ++i) {
        uint32_t vid = fv[i];
        if (vid == 0xffffffff || vid >= nbVerts) continue;
        if (vertVisible && !vertVisible[vid]) continue;

        uint32_t vid3 = vid * 3;
        float dx = verts[vid3] - center.x;
        float dx2 = dx * dx;
        if (dx2 > radius2) continue;
        float dy = verts[vid3 + 1] - center.y;
        float dy2 = dy * dy;
        if (dx2 + dy2 > radius2) continue;
        float dz = verts[vid3 + 2] - center.z;
        if (dx2 + dy2 + dz * dz <= radius2) {
            picked.push_back(vid);
            visited[vid] = curEpoch;
        }
    }

    size_t head = 0;
    while (head < picked.size()) {
        uint32_t u = picked[head++];

        uint32_t start = vrvStartCount[u * 2];
        uint32_t count = vrvStartCount[u * 2 + 1];
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t v = vertRingVert[start + j];
            if (v >= nbVerts || visited[v] == curEpoch) continue;
            visited[v] = curEpoch;

            if (vertVisible && !vertVisible[v]) continue;

            if (j + 2 < count) {
                uint32_t nextV = vertRingVert[start + j + 2];
                __builtin_prefetch(&verts[nextV * 3], 0, 1);
            }

            uint32_t v3 = v * 3;
            float dx = verts[v3] - center.x;
            float dx2 = dx * dx;
            if (dx2 > radius2) continue;
            float dy = verts[v3 + 1] - center.y;
            float dy2 = dy * dy;
            if (dx2 + dy2 > radius2) continue;
            float dz = verts[v3 + 2] - center.z;
            if (dx2 + dy2 + dz * dz <= radius2) {
                picked.push_back(v);
            }
        }
    }
}

static void filterCullingVertices(
    std::vector<uint32_t>& pickedVertices, Mesh* mesh, const glm::vec3& eyeDirLocal
) {
    const size_t total = pickedVertices.size();
    if (total == 0) return;
    const int maxThreads = omp_get_max_threads();
    const bool inParallel = omp_in_parallel();
    if (total < 4096 || inParallel || maxThreads <= 1) {
        std::vector<uint32_t> front;
        front.reserve(total);
        const float* const norms = mesh->normals.data();
        for (uint32_t vid : pickedVertices) {
            float nx = norms[vid * 3];
            float ny = norms[vid * 3 + 1];
            float nz = norms[vid * 3 + 2];
            if (nx * eyeDirLocal.x + ny * eyeDirLocal.y + nz * eyeDirLocal.z <= 0.0f) {
                front.push_back(vid);
            }
        }
        pickedVertices = std::move(front);
        return;
    }

    std::vector<std::vector<uint32_t>> threadBuckets(maxThreads);
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& local = threadBuckets[tid];
        local.reserve((total / maxThreads) + 64);
        const float* const norms = mesh->normals.data();
        #pragma omp for schedule(static)
        for (int64_t i = 0; i < (int64_t)total; ++i) {
            uint32_t vid = pickedVertices[i];
            float nx = norms[vid * 3];
            float ny = norms[vid * 3 + 1];
            float nz = norms[vid * 3 + 2];
            if (nx * eyeDirLocal.x + ny * eyeDirLocal.y + nz * eyeDirLocal.z <= 0.0f) {
                local.push_back(vid);
            }
        }
    }
    size_t offsets[129] = {0};
    int numMergeThreads = std::min(maxThreads, 128);
    for (int t = 0; t < numMergeThreads; ++t) {
        offsets[t + 1] = offsets[t] + threadBuckets[t].size();
    }
    pickedVertices.resize(offsets[numMergeThreads]);
    #pragma omp parallel for schedule(static) if(offsets[numMergeThreads] > 8192)
    for (int t = 0; t < numMergeThreads; ++t) {
        size_t count = threadBuckets[t].size();
        if (count > 0) {
            std::memcpy(pickedVertices.data() + offsets[t], threadBuckets[t].data(), count * sizeof(uint32_t));
        }
    }
}

SculptManager::~SculptManager() = default;

SculptManager::SculptManager() {
    m_armatureTool = std::make_unique<ArmatureTool>(*this);
    m_poseTool = std::make_unique<PoseTool>(*this);
    m_tubeTool = std::make_unique<TubeTool>(*this);
    m_primitiveTool = std::make_unique<PrimitiveTool>(*this);
    m_quadDrawTool = std::make_unique<retopo::QuadDrawTool>(*this);
    m_maskExtractTool = std::make_unique<MaskExtractTool>(*this);
    m_componentSelectTool = std::make_unique<ComponentSelectTool>(*this);
    m_edgeCreaseTool = std::make_unique<EdgeCreaseTool>(*this);
    m_curveFoldsTool = std::make_unique<CurveFoldsTool>(*this);
    // Initialise all brushes with baseline defaults
    for (int i = 0; i < BRUSH_COUNT; ++i) {
        m_brushSettings[i].radius = 50.0f;
        m_brushSettings[i].intensity = 0.5f;
        m_brushSettings[i].focalShift = 0.0f;
        m_brushSettings[i].focalShiftFalloff = true;
        m_brushSettings[i].hardness = 0.5f;
        m_brushSettings[i].spacing = 0.15f;
        m_brushSettings[i].negative = false;
        m_brushSettings[i].culling = false;
        m_brushSettings[i].accumulate = true;
        m_brushSettings[i].lockPosition = false;
        m_brushSettings[i].clay = false;
        m_brushSettings[i].tangent = false;
        m_brushSettings[i].topoCheck = false;
        m_brushSettings[i].elasticity = 1.0f;
        m_brushSettings[i].paintColor = glm::vec3(0.72f, 0.52f, 0.45f);
        m_brushSettings[i].paintRoughness = 0.5f;
        m_brushSettings[i].paintMetallic = 0.0f;
        m_brushSettings[i].writeAlbedo = true;
        m_brushSettings[i].writeRoughness = true;
        m_brushSettings[i].writeMetalness = true;
        m_brushSettings[i].pickColor = false;
        m_brushSettings[i].maskSharpenBlurIterations = 4;
        m_brushSettings[i].maskSharpenFactor = 1.0f;
        m_brushSettings[i].maskExtractThickness = 0.05f;
    }

    // Set brush-specific defaults matching app_settings.cfg
    m_brushSettings[BRUSH_FLATTEN].radius = 146.0f;
    m_brushSettings[BRUSH_FLATTEN].intensity = 0.149644f;
    m_brushSettings[BRUSH_FLATTEN].focalShift = -0.30f;
    m_brushSettings[BRUSH_FLATTEN].spacing = 0.05f;
    m_brushSettings[BRUSH_FLATTEN].negative = true;
    m_brushSettings[BRUSH_FLATTEN].culling = false;

    m_brushSettings[BRUSH_SMOOTH].radius = 125.0f;
    m_brushSettings[BRUSH_SMOOTH].intensity = 1.002429f;
    m_brushSettings[BRUSH_SMOOTH].focalShift = 0.605884f;
    m_brushSettings[BRUSH_SMOOTH].culling = true;

    m_brushSettings[BRUSH_INFLATE].radius = 146.0f;
    m_brushSettings[BRUSH_INFLATE].intensity = 0.149646f;
    m_brushSettings[BRUSH_INFLATE].focalShift = -0.005f;
    m_brushSettings[BRUSH_INFLATE].culling = false;

    m_brushSettings[BRUSH_PINCH].radius = 125.0f;
    m_brushSettings[BRUSH_PINCH].intensity = 0.500683f;
    m_brushSettings[BRUSH_PINCH].focalShift = 0.645f;
    m_brushSettings[BRUSH_PINCH].culling = true;

    m_brushSettings[BRUSH_CREASE].radius = 122.0f;
    m_brushSettings[BRUSH_CREASE].intensity = 0.101145f;
    m_brushSettings[BRUSH_CREASE].negative = true;
    m_brushSettings[BRUSH_CREASE].culling = true;

    m_brushSettings[BRUSH_VTOOL].radius = 138.5f;
    m_brushSettings[BRUSH_VTOOL].intensity = 0.054995f;
    m_brushSettings[BRUSH_VTOOL].focalShift = -0.825f;
    m_brushSettings[BRUSH_VTOOL].spacing = 0.05f;
    m_brushSettings[BRUSH_VTOOL].negative = true;
    m_brushSettings[BRUSH_VTOOL].culling = false;

    m_brushSettings[BRUSH_MOVE].radius = 352.0f;
    m_brushSettings[BRUSH_MOVE].intensity = 0.999029f;
    m_brushSettings[BRUSH_MOVE].focalShift = 0.282281f;
    m_brushSettings[BRUSH_MOVE].topoCheck = true;
    m_brushSettings[BRUSH_MOVE].culling = false;

    m_brushSettings[BRUSH_DRAG].radius = 100.0f;
    m_brushSettings[BRUSH_DRAG].intensity = 0.5f;
    m_brushSettings[BRUSH_DRAG].culling = false;

    m_brushSettings[BRUSH_ELASTIC].radius = 614.5f;
    m_brushSettings[BRUSH_ELASTIC].intensity = 0.441305f;
    m_brushSettings[BRUSH_ELASTIC].focalShift = 0.695001f;
    m_brushSettings[BRUSH_ELASTIC].elasticity = 0.5f;
    m_brushSettings[BRUSH_ELASTIC].culling = false;

    m_brushSettings[BRUSH_MASK].radius = 50.0f;
    m_brushSettings[BRUSH_MASK].intensity = 0.744898f;
    m_brushSettings[BRUSH_MASK].focalShift = 0.857143f;
    m_brushSettings[BRUSH_MASK].culling = true;

    m_brushSettings[BRUSH_PAINT].radius = 108.5f;
    m_brushSettings[BRUSH_PAINT].intensity = 0.5f;
    m_brushSettings[BRUSH_PAINT].hardness = 0.7f;
    m_brushSettings[BRUSH_PAINT].culling = true;
    m_brushSettings[BRUSH_PAINT].paintColor = glm::vec3(0.971154f, 0.909559f, 0.513591f);

    m_brushSettings[BRUSH_TWIST].radius = 100.0f;
    m_brushSettings[BRUSH_TWIST].intensity = 0.5f;
    m_brushSettings[BRUSH_TWIST].culling = true;

    m_brushSettings[BRUSH_LOCALSCALE].radius = 100.0f;
    m_brushSettings[BRUSH_LOCALSCALE].intensity = 0.5f;
    m_brushSettings[BRUSH_LOCALSCALE].culling = true;

    m_brushSettings[BRUSH_CLAY].radius = 84.5f;
    m_brushSettings[BRUSH_CLAY].intensity = 0.5f;
    m_brushSettings[BRUSH_CLAY].clay = true;
    m_brushSettings[BRUSH_CLAY].culling = false;

    m_brushSettings[BRUSH_CLAYBUILDUP].radius = 134.0f;
    m_brushSettings[BRUSH_CLAYBUILDUP].intensity = 1.004649f;
    m_brushSettings[BRUSH_CLAYBUILDUP].focalShift = -0.285f;
    m_brushSettings[BRUSH_CLAYBUILDUP].spacing = 0.05f;
    m_brushSettings[BRUSH_CLAYBUILDUP].clay = true;
    m_brushSettings[BRUSH_CLAYBUILDUP].accumulate = false;
    m_brushSettings[BRUSH_CLAYBUILDUP].culling = false;

    m_brushSettings[BRUSH_DAMSTANDARD].radius = 158.0f;
    m_brushSettings[BRUSH_DAMSTANDARD].intensity = 0.149387f;
    m_brushSettings[BRUSH_DAMSTANDARD].focalShift = 0.175f;
    m_brushSettings[BRUSH_DAMSTANDARD].spacing = 0.05f;
    m_brushSettings[BRUSH_DAMSTANDARD].negative = true;
    m_brushSettings[BRUSH_DAMSTANDARD].culling = false;

    m_brushSettings[BRUSH_SQUAREBRUSH].radius = 78.5f;
    m_brushSettings[BRUSH_SQUAREBRUSH].intensity = 0.645477f;
    m_brushSettings[BRUSH_SQUAREBRUSH].focalShift = -0.54f;
    m_brushSettings[BRUSH_SQUAREBRUSH].clay = true;
    m_brushSettings[BRUSH_SQUAREBRUSH].culling = false;

    m_brushSettings[BRUSH_VISIBILITY].radius = 50.0f;
    m_brushSettings[BRUSH_VISIBILITY].culling = false;

    m_brushSettings[BRUSH_MASK_GRADIENT_BLUR].radius = 50.0f;
    m_brushSettings[BRUSH_MASK_GRADIENT_BLUR].maskSharpenBlurIterations = 40;
    m_brushSettings[BRUSH_MASK_GRADIENT_BLUR].culling = false;

    m_brushSettings[BRUSH_MEASURE].radius = 50.0f;
    m_brushSettings[BRUSH_MEASURE].culling = false;

    m_brushSettings[BRUSH_DIVIDER].radius = 50.0f;
    m_brushSettings[BRUSH_DIVIDER].culling = false;

    m_brushSettings[BRUSH_TRANSFORM].radius = 48.5f;
    m_brushSettings[BRUSH_TRANSFORM].intensity = 0.485038f;
    m_brushSettings[BRUSH_TRANSFORM].culling = false;

    m_brushSettings[BRUSH_ARMATURE_SPHERES].radius = 50.0f;
    m_brushSettings[BRUSH_ARMATURE_SPHERES].culling = false;

    m_brushSettings[BRUSH_BRUSH].radius = 89.0f;
    m_brushSettings[BRUSH_BRUSH].intensity = 0.045884f;
    m_brushSettings[BRUSH_BRUSH].focalShift = -0.46f;
    m_brushSettings[BRUSH_BRUSH].spacing = 0.05f;
    m_brushSettings[BRUSH_BRUSH].culling = true;
    m_brushSettings[BRUSH_BRUSH].clay = true;
    m_brushSettings[BRUSH_BRUSH].stampType = 4;
    m_brushSettings[BRUSH_BRUSH].stampInnerRatio = 0.2f;
    m_brushSettings[BRUSH_BRUSH].stampLockRotation = true;
    m_brushSettings[BRUSH_BRUSH].stampUseTilt = true;

    m_brushSettings[BRUSH_POLYGROUP].radius = 65.0f;
    m_brushSettings[BRUSH_POLYGROUP].intensity = 0.5f;
    m_brushSettings[BRUSH_POLYGROUP].focalShift = 0.82f;
    m_brushSettings[BRUSH_POLYGROUP].culling = true;

    m_brushSettings[BRUSH_CLIP_CURVE].radius = 177.5f;
    m_brushSettings[BRUSH_CLIP_CURVE].culling = false;

    m_brushSettings[BRUSH_TRIM].radius = 50.0f;
    m_brushSettings[BRUSH_TRIM].culling = false;

    m_brushSettings[BRUSH_SPLIT].radius = 134.0f;
    m_brushSettings[BRUSH_SPLIT].intensity = 0.056369f;
    m_brushSettings[BRUSH_SPLIT].focalShift = -0.555f;
    m_brushSettings[BRUSH_SPLIT].culling = true;
    m_brushSettings[BRUSH_SPLIT].splitMode = 0; // 0 = Lasso (like Trim)

    m_brushSettings[BRUSH_DELETE_LAYER].radius = 164.0f;
    m_brushSettings[BRUSH_DELETE_LAYER].intensity = 0.5f;
    m_brushSettings[BRUSH_DELETE_LAYER].culling = true;

    m_brushSettings[BRUSH_RELAX].radius = 50.0f;
    m_brushSettings[BRUSH_RELAX].intensity = 0.5f;
    m_brushSettings[BRUSH_RELAX].culling = true;

    m_brushSettings[BRUSH_POSE].radius = 50.0f;
    m_brushSettings[BRUSH_POSE].culling = false;

    m_brushSettings[BRUSH_SNAKEHOOK].radius = 82.5f;
    m_brushSettings[BRUSH_SNAKEHOOK].intensity = 1.0f;
    m_brushSettings[BRUSH_SNAKEHOOK].culling = false;
    m_brushSettings[BRUSH_SNAKEHOOK].spacing = 0.1f;

    m_brushSettings[BRUSH_TUBE].radius = 50.0f;
    m_brushSettings[BRUSH_TUBE].culling = false;

    m_brushSettings[BRUSH_QUAD_DRAW].radius = 66.5f;
    m_brushSettings[BRUSH_QUAD_DRAW].culling = false;

    m_brushSettings[BRUSH_EXTRACT].radius = 50.0f;
    m_brushSettings[BRUSH_EXTRACT].culling = false;

    m_brushSettings[BRUSH_CURVE_FOLDS].radius = 15.0f;
    m_brushSettings[BRUSH_CURVE_FOLDS].intensity = 1.0f;
    m_brushSettings[BRUSH_CURVE_FOLDS].culling = false;

    m_brushSettings[BRUSH_RAKE].radius = 80.0f;
    m_brushSettings[BRUSH_RAKE].intensity = 0.5f;
    m_brushSettings[BRUSH_RAKE].spacing = 0.06f;
    m_brushSettings[BRUSH_RAKE].culling = false;
    m_brushSettings[BRUSH_RAKE].rakeClayMode = true;
    m_brushSettings[BRUSH_RAKE].rakeSquareFootprint = false;
    m_brushSettings[BRUSH_RAKE].rakeRidgesOnly = false;
    m_brushSettings[BRUSH_RAKE].rakeWaveType = 0;
    m_brushSettings[BRUSH_RAKE].rakeTeethCount = 6;
    m_brushSettings[BRUSH_RAKE].rakeDepth = 0.85f;
    m_brushSettings[BRUSH_RAKE].rakeSharpness = 0.5f;
    m_brushSettings[BRUSH_RAKE].rakeTurnSmoothness = 0.65f;
    m_brushSettings[BRUSH_RAKE].rakeAngleOffset = 0.0f;
}

void SculptManager::notifyMeshDeletedTool(Mesh* mesh) {
    if (m_curveFoldsTool) {
        m_curveFoldsTool->notifyMeshDeleted(mesh);
    }
}

std::vector<glm::vec3> SculptManager::getActiveSymmetryScales() const {
    std::vector<glm::vec3> scales;
    if (!m_useSym || (!m_symX && !m_symY && !m_symZ)) return scales;

    std::vector<float> xVals = { 1.0f };
    std::vector<float> yVals = { 1.0f };
    std::vector<float> zVals = { 1.0f };

    if (m_symX) xVals.push_back(-1.0f);
    if (m_symY) yVals.push_back(-1.0f);
    if (m_symZ) zVals.push_back(-1.0f);

    for (float sx : xVals) {
        for (float sy : yVals) {
            for (float sz : zVals) {
                if (sx == 1.0f && sy == 1.0f && sz == 1.0f) continue;
                scales.push_back(glm::vec3(sx, sy, sz));
            }
        }
    }
    return scales;
}

int SculptManager::doStrokePass(
    Scene& scene,
    Mesh* mesh,
    BrushType activeBrush,
    bool negative,
    const uint32_t* pVerts,
    size_t nbVerts,
    const glm::vec3& currentIntersection,
    const glm::vec3& currentIntersectionNormal,
    const glm::vec3& initialIntersection,
    const glm::vec3& cachedAreaNormal,
    const glm::vec3& cachedAreaCenter,
    float localRadius,
    float intensity,
    float mouseX,
    bool isSymmetry,
    const glm::vec3& sScale,
    const std::vector<uint32_t>* pPickedVec
) {
    uint32_t* pData = const_cast<uint32_t*>(pVerts);
    std::vector<float> preVerts;
    bool isLayerDeform = mesh && mesh->isLayerRecording() && activeBrush != BRUSH_DELETE_LAYER &&
                         activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_POLYGROUP;
    if (isLayerDeform && nbVerts > 0) {
        preVerts.resize(nbVerts * 3);
        for (size_t i = 0; i < nbVerts; ++i) {
            uint32_t v = pVerts[i];
            preVerts[i * 3 + 0] = mesh->verts[v * 3 + 0];
            preVerts[i * 3 + 1] = mesh->verts[v * 3 + 1];
            preVerts[i * 3 + 2] = mesh->verts[v * 3 + 2];
        }
    }

    int deformedCount = 0;
    bool alphaXSym = isSymmetry && ((sScale.x * sScale.y * sScale.z) < 0.0f);

    switch (activeBrush) {
        case BRUSH_FLATTEN: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                float areaResults[7] = {0.0f};
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pData,
                    nbVerts,
                    areaResults
                )) {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                    }
                } else {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = currentIntersectionNormal;
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = currentIntersection;
                    }
                }
            }

            deformedCount = strokeFlatten(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_SMOOTH: {
            deformedCount = strokeSmooth(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                mesh->vrvStartCount.data(),
                mesh->vertRingVert.data(),
                mesh->vertOnEdge.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                getCurrentSettings().tangent,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_RELAX: {
            if (pPickedVec) {
                deformedCount = m_relaxEngine.relaxVertices(
                    mesh,
                    *const_cast<std::vector<uint32_t>*>(pPickedVec),
                    currentIntersection,
                    localRadius,
                    intensity,
                    getCurrentSettings()
                );
            } else {
                std::vector<uint32_t> tempVerts(pVerts, pVerts + nbVerts);
                deformedCount = m_relaxEngine.relaxVertices(
                    mesh,
                    tempVerts,
                    currentIntersection,
                    localRadius,
                    intensity,
                    getCurrentSettings()
                );
            }
            break;
        }
        case BRUSH_INFLATE: {
            deformedCount = strokeInflate(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                mesh->normals.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_PINCH: {
            deformedCount = strokePinch(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_CREASE: {
            deformedCount = strokeCrease(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_VTOOL: {
            deformedCount = strokeVTool(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                -0.4f, true,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_MOVE: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(m_currentIntersectionNormal) > 1e-6f) ? glm::normalize(m_currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = m_currentIntersection - m_initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                glm::vec3 primaryDragDir = norm * (localAmount * intensity);
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            } else {
                glm::vec3 primaryDragDir = (m_currentIntersection - m_initialIntersection) * intensity;
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            }
            deformedCount = strokeMove(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_DRAG: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(m_currentIntersectionNormal) > 1e-6f) ? glm::normalize(m_currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = m_currentIntersection - m_initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                glm::vec3 primaryDragDir = norm * (localAmount * intensity);
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            } else {
                glm::vec3 primaryDragDir = (m_currentIntersection - m_initialIntersection) * intensity;
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            }
            deformedCount = strokeDrag(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_ELASTIC: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(m_currentIntersectionNormal) > 1e-6f) ? glm::normalize(m_currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = m_currentIntersection - m_initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                glm::vec3 primaryDragDir = norm * (localAmount * intensity);
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            } else {
                glm::vec3 primaryDragDir = (m_currentIntersection - m_initialIntersection) * intensity;
                dragDirection = isSymmetry ? (primaryDragDir * sScale) : primaryDragDir;
            }
            deformedCount = strokeElastic(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius, getCurrentSettings().elasticity,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_SNAKEHOOK: {
            glm::vec3 tipStart = isSymmetry ? reflectPointSymmetry(m_snakePrevTipPos, sScale, mesh, m_symmetryMode)
                                            : m_snakePrevTipPos;
            glm::vec3 tipEnd = currentIntersection;

            deformedCount = strokeSnakeHook(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                static_cast<int>(nbVerts),
                tipStart.x, tipStart.y, tipStart.z,
                tipEnd.x, tipEnd.y, tipEnd.z,
                localRadius, intensity,
                getCurrentSettings().snakePinch,
                getCurrentSettings().snakeMagnify,
                getCurrentSettings().snakeTwist,
                getCurrentSettings().focalShift,
                getCurrentSettings().focalShiftFalloff,
                negative,
                false, nullptr
            );

            if (getCurrentSettings().snakeRelax > 1e-4f && deformedCount > 0 && !mesh->vertRingVert.empty()) {
                strokeSmooth(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    mesh->vrvStartCount.data(),
                    mesh->vertRingVert.data(),
                    mesh->vertOnEdge.data(),
                    pData,
                    deformedCount,
                    tipEnd.x, tipEnd.y, tipEnd.z,
                    localRadius,
                    getCurrentSettings().snakeRelax * 0.3f,
                    true,
                    getCurrentSettings().focalShift,
                    getCurrentSettings().focalShiftFalloff,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
                );
            }
            break;
        }
        case BRUSH_MASK: {
            deformedCount = strokeMask(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity, getSettings(activeBrush).hardness,
                negative,
                getSettings(activeBrush).focalShift, getSettings(activeBrush).focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_PAINT: {
            deformedCount = strokePaint(
                mesh->verts.data(),
                mesh->colors.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity, getCurrentSettings().hardness,
                getCurrentSettings().paintColor.r, getCurrentSettings().paintColor.g, getCurrentSettings().paintColor.b,
                getCurrentSettings().paintRoughness, getCurrentSettings().paintMetallic,
                getCurrentSettings().writeAlbedo, getCurrentSettings().writeRoughness, getCurrentSettings().writeMetalness,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_POLYGROUP: {
            for (size_t i = 0; i < nbVerts; ++i) {
                uint32_t v = pVerts[i];
                if (v * 2 + 1 < mesh->vrfStartCount.size()) {
                    uint32_t start = mesh->vrfStartCount[v * 2];
                    uint32_t count = mesh->vrfStartCount[v * 2 + 1];
                    for (uint32_t j = start; j < start + count; ++j) {
                        if (j < mesh->vertRingFace.size()) {
                            uint32_t f = mesh->vertRingFace[j];
                            mesh->setFaceGroup(f, m_activeGroupId);
                        }
                    }
                }
            }
            mesh->isFaceGroupDirty = true;
            deformedCount = (int)nbVerts;
            break;
        }
        case BRUSH_TWIST: {
            deformedCount = strokeTwist(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity * 3.14159f * 0.5f,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_LOCALSCALE: {
            deformedCount = strokeLocalScale(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_CLAY: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                bool moved = glm::distance(currentIntersection, m_prevClayIntersection) > localRadius * 0.15f;
                bool intervalElapsed = (m_strokeFrameCounter - m_clayAreaNormalFrame) >= 3;
                if (moved || intervalElapsed) {
                    auto tAreaStart = std::chrono::high_resolution_clock::now();
                    float areaResults[7] = {0.0f};
                    if (computeAreaNormalAndCenter(
                        mesh->verts.data(),
                        mesh->normals.data(),
                        mesh->materials.data(),
                        pData,
                        nbVerts,
                        areaResults
                    )) {
                        if (!getCurrentSettings().flattenLockNormal) {
                            m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                        }
                        if (!getCurrentSettings().flattenLockOrigin) {
                            m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                        }
                        m_clayAreaNormalFrame = m_strokeFrameCounter;
                        m_prevClayIntersection = currentIntersection;
                    }
                    m_lastFrameProfile.areaComputeMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tAreaStart).count();
                }
                areaNormal = m_cachedAreaNormal;
                areaCenter = m_cachedAreaCenter;
            }

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            deformedCount = strokeFlatten(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_CLAYBUILDUP: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!isSymmetry && !m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                bool moved = glm::distance(currentIntersection, m_prevClayIntersection) > localRadius * 0.15f;
                bool intervalElapsed = (m_strokeFrameCounter - m_clayAreaNormalFrame) >= 3;
                if (moved || intervalElapsed) {
                    auto tAreaStart = std::chrono::high_resolution_clock::now();
                    float areaResults[7] = {0.0f};
                    if (computeAreaNormalAndCenter(
                        mesh->verts.data(),
                        mesh->normals.data(),
                        mesh->materials.data(),
                        pData,
                        nbVerts,
                        areaResults
                    )) {
                        if (!getCurrentSettings().flattenLockNormal) {
                            m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                        }
                        if (!getCurrentSettings().flattenLockOrigin) {
                            m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                        }
                        m_clayAreaNormalFrame = m_strokeFrameCounter;
                        m_prevClayIntersection = currentIntersection;
                    }
                    m_lastFrameProfile.areaComputeMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tAreaStart).count();
                }
                areaNormal = m_cachedAreaNormal;
                areaCenter = m_cachedAreaCenter;
            }

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? reflectPointSymmetry(m_alphaOrigin, sScale, mesh, m_symmetryMode) : m_alphaOrigin;

            if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            deformedCount = strokeSquareBrush(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity * 0.1f,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                1.0f, 1.0f, localRadius * 0.70710678f,
                alphaLookAt, alphaXSym
            );
            break;
        }
        case BRUSH_DAMSTANDARD: {
            deformedCount = strokeDamStandard(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_SQUAREBRUSH: {
            glm::vec3 areaCenter = currentIntersection;
            glm::vec3 areaNormal = currentIntersectionNormal;
            float areaResults[7] = {0.0f};
            if (computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                areaResults
            )) {
                areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
            }

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? reflectPointSymmetry(m_alphaOrigin, sScale, mesh, m_symmetryMode) : m_alphaOrigin;

            if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            deformedCount = strokeSquareBrush(
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                1.0f, 1.0f, localRadius * 0.70710678f,
                alphaLookAt, alphaXSym
            );
            break;
        }
        case BRUSH_BRUSH: {
            glm::vec3 areaCenter = currentIntersection;
            glm::vec3 areaNormal = currentIntersectionNormal;
            float areaResults[7] = {0.0f};
            if (computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                areaResults
            )) {
                if (areaResults[6] > 0.0f) {
                    areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                    areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                }
            }

            if (getCurrentSettings().clay) {
                float off = localRadius * 0.1f;
                areaCenter += areaNormal * (negative ? -off : off);
            }

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? reflectPointSymmetry(m_alphaOrigin, sScale, mesh, m_symmetryMode) : m_alphaOrigin;

            if (getCurrentSettings().stampLockRotation) {
                const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            }
            else if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            float finalAngle = getCurrentSettings().stampAngle;
            if (getCurrentSettings().stampUseTilt && g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
                float tx = g_tablet.getTiltX();
                float ty = g_tablet.getTiltY();
                if (tx * tx + ty * ty > 1.0f) {
                    float tiltAngle = std::atan2(ty, tx) * (180.0f / 3.1415926535f);
                    finalAngle += tiltAngle;
                }
            }

            deformedCount = strokeBrush(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().clay,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                getCurrentSettings().stampType,
                getCurrentSettings().stampSides,
                getCurrentSettings().stampInnerRatio,
                finalAngle,
                getCurrentSettings().stampBlur,
                alphaLookAt, alphaXSym
            );
            break;
        }
        case BRUSH_RAKE: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!isSymmetry && !m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                bool moved = glm::distance(currentIntersection, m_prevClayIntersection) > localRadius * 0.15f;
                bool intervalElapsed = (m_strokeFrameCounter - m_clayAreaNormalFrame) >= 3;
                if (moved || intervalElapsed) {
                    auto tAreaStart = std::chrono::high_resolution_clock::now();
                    float areaResults[7] = {0.0f};
                    if (computeAreaNormalAndCenter(
                        mesh->verts.data(),
                        mesh->normals.data(),
                        mesh->materials.data(),
                        pData,
                        nbVerts,
                        areaResults
                    )) {
                        if (!getCurrentSettings().flattenLockNormal) {
                            m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                        }
                        if (!getCurrentSettings().flattenLockOrigin) {
                            m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                        }
                        m_clayAreaNormalFrame = m_strokeFrameCounter;
                        m_prevClayIntersection = currentIntersection;
                    }
                    m_lastFrameProfile.areaComputeMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tAreaStart).count();
                }
                areaNormal = m_cachedAreaNormal;
                areaCenter = m_cachedAreaCenter;
            }

            glm::vec3 strokeDir(0.0f);

            if (!isSymmetry) {
                if (m_firstStrokeFrame || !m_hasRakeDir) {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    glm::vec3 projCam = camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal);
                    if (glm::length(projCam) > 1e-5f) {
                        strokeDir = glm::normalize(projCam);
                    } else {
                        glm::vec3 camUpLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[1]), 0.0f)));
                        strokeDir = glm::normalize(camUpLocal - areaNormal * glm::dot(camUpLocal, areaNormal));
                    }
                    m_rakeSmoothDir = strokeDir;
                    m_rakePrevPos = currentIntersection;
                    m_hasRakeDir = true;
                } else {
                    glm::vec3 deltaP = currentIntersection - m_rakePrevPos;
                    glm::vec3 dRaw = deltaP - areaNormal * glm::dot(deltaP, areaNormal);
                    float dRawLen = glm::length(dRaw);
                    if (dRawLen > 1e-5f) {
                        glm::vec3 rawDir = dRaw / dRawLen;
                        float smoothFactor = std::clamp(getCurrentSettings().rakeTurnSmoothness, 0.0f, 0.95f);
                        glm::vec3 prevProj = m_rakeSmoothDir - areaNormal * glm::dot(m_rakeSmoothDir, areaNormal);
                        if (glm::length(prevProj) > 1e-5f) {
                            prevProj = glm::normalize(prevProj);
                        } else {
                            prevProj = rawDir;
                        }
                        m_rakeSmoothDir = glm::normalize(glm::mix(rawDir, prevProj, smoothFactor));
                        m_rakePrevPos = currentIntersection;
                    }
                    strokeDir = m_rakeSmoothDir;
                }
            } else {
                // Symmetry pass: reflect the primary stroke direction
                strokeDir = reflectVectorSymmetry(m_rakeSmoothDir, sScale, mesh, m_symmetryMode);
                glm::vec3 proj = strokeDir - areaNormal * glm::dot(strokeDir, areaNormal);
                if (glm::length(proj) > 1e-5f) {
                    strokeDir = glm::normalize(proj);
                } else {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    camRightLocal = reflectVectorSymmetry(camRightLocal, sScale, mesh, m_symmetryMode);
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            float finalAngle = getCurrentSettings().rakeAngleOffset;
            if (getCurrentSettings().stampUseTilt && g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
                float tx = g_tablet.getTiltX();
                float ty = g_tablet.getTiltY();
                if (tx * tx + ty * ty > 1.0f) {
                    float tiltAngle = std::atan2(ty, tx) * (180.0f / 3.1415926535f);
                    finalAngle += tiltAngle;
                }
            }

            if (std::abs(finalAngle) > 1e-4f) {
                float angleRad = glm::radians(finalAngle);
                if (alphaXSym) angleRad = -angleRad;
                glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angleRad, areaNormal);
                strokeDir = glm::normalize(glm::vec3(rot * glm::vec4(strokeDir, 0.0f)));
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            deformedCount = strokeRake(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().rakeClayMode,
                getCurrentSettings().accumulate,
                getCurrentSettings().focalShift,
                getCurrentSettings().focalShiftFalloff,
                getCurrentSettings().rakeWaveType,
                getCurrentSettings().rakeTeethCount,
                getCurrentSettings().rakeDepth,
                getCurrentSettings().rakeSharpness,
                getCurrentSettings().rakeSquareFootprint,
                getCurrentSettings().rakeRidgesOnly,
                alphaLookAt, alphaXSym
            );
            break;
        }
        case BRUSH_DELETE_LAYER: {
            if (!mesh->isLayerRecording()) break;
            Layer* layer = mesh->layerStack.getActive();
            if (!layer || !layer->visible) break;
            if (layer->deltaVerts.size() != mesh->verts.size()) {
                layer->deltaVerts.resize(mesh->verts.size(), 0.0f);
            }
            deformedCount = strokeDeleteLayer(
                mesh->layerStack.getBase().data(),
                layer->deltaVerts.data(),
                mesh->verts.data(),
                mesh->materials.data(),
                pData,
                nbVerts,
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                layer->intensity,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        default:
            break;
    }

    if (isLayerDeform && deformedCount > 0 && mesh) {
        Layer* layer = mesh->layerStack.getActive();
        if (layer) {
            if (layer->deltaVerts.size() != mesh->verts.size()) {
                sculpt_log_lvl(LogLevel::Warning, "[Layer WARNING] Layer '%s' deltaVerts size mismatch (%zu vs %zu). Resizing.\n",
                               layer->name.c_str(), layer->deltaVerts.size(), mesh->verts.size());
                layer->deltaVerts.resize(mesh->verts.size(), 0.0f);
            }
            if (m_firstStrokeFrame || m_strokeStartLayerDeltas.size() != layer->deltaVerts.size()) {
                m_strokeStartLayerDeltas = layer->deltaVerts;
            }

            float layerIntensity = layer->intensity;
            float invIntensity = 0.0f;
            if (std::abs(layerIntensity) > 1e-4f) {
                invIntensity = std::clamp(1.0f / layerIntensity, -100.0f, 100.0f);
            }

            float radius = getCurrentSettings().radius;
            float maxStep = std::max(1.0f, radius * 2.0f);

            bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC);

            for (int i = 0; i < deformedCount; ++i) {
                uint32_t v = pVerts[i];
                if (v * 3 + 2 < mesh->verts.size() && v * 3 + 2 < layer->deltaVerts.size()) {
                    bool isSnakeBrush = (activeBrush == BRUSH_SNAKEHOOK);
                    float dx = isSnakeBrush ? ((mesh->verts[v * 3 + 0] - preVerts[i * 3 + 0]) * invIntensity)
                                            : ((mesh->verts[v * 3 + 0] - mesh->vertProxy[v * 3 + 0]) * invIntensity);
                    float dy = isSnakeBrush ? ((mesh->verts[v * 3 + 1] - preVerts[i * 3 + 1]) * invIntensity)
                                            : ((mesh->verts[v * 3 + 1] - mesh->vertProxy[v * 3 + 1]) * invIntensity);
                    float dz = isSnakeBrush ? ((mesh->verts[v * 3 + 2] - preVerts[i * 3 + 2]) * invIntensity)
                                            : ((mesh->verts[v * 3 + 2] - mesh->vertProxy[v * 3 + 2]) * invIntensity);

                    if (std::isnan(dx) || std::isnan(dy) || std::isnan(dz) ||
                        std::isinf(dx) || std::isinf(dy) || std::isinf(dz)) {
                        continue;
                    }

                    dx = std::clamp(dx, -maxStep, maxStep);
                    dy = std::clamp(dy, -maxStep, maxStep);
                    dz = std::clamp(dz, -maxStep, maxStep);

                    if (isGrabBrush) {
                        layer->deltaVerts[v * 3 + 0] = m_strokeStartLayerDeltas[v * 3 + 0] + dx;
                        layer->deltaVerts[v * 3 + 1] = m_strokeStartLayerDeltas[v * 3 + 1] + dy;
                        layer->deltaVerts[v * 3 + 2] = m_strokeStartLayerDeltas[v * 3 + 2] + dz;
                    } else {
                        layer->deltaVerts[v * 3 + 0] += dx;
                        layer->deltaVerts[v * 3 + 1] += dy;
                        layer->deltaVerts[v * 3 + 2] += dz;
                    }

                    if (isSnakeBrush) {
                        mesh->verts[v * 3 + 0] = preVerts[i * 3 + 0] + dx * layerIntensity;
                        mesh->verts[v * 3 + 1] = preVerts[i * 3 + 1] + dy * layerIntensity;
                        mesh->verts[v * 3 + 2] = preVerts[i * 3 + 2] + dz * layerIntensity;
                    } else {
                        mesh->verts[v * 3 + 0] = mesh->vertProxy[v * 3 + 0] + dx * layerIntensity;
                        mesh->verts[v * 3 + 1] = mesh->vertProxy[v * 3 + 1] + dy * layerIntensity;
                        mesh->verts[v * 3 + 2] = mesh->vertProxy[v * 3 + 2] + dz * layerIntensity;
                    }
                }
            }
        }
    }

    return deformedCount;
}

void SculptManager::cancelStroke() {
    m_isSculpting = false;
    m_pendingBVHUpdate = false;
    m_grabbedNumIFaces = 0;
    m_grabUndoRecorded = false;
    m_grabbedMinV = 0xFFFFFFFF;
    m_grabbedMaxV = 0;
    m_relaxEngine.endStroke();
    m_grabbedVertices.clear();
    m_grabbedVerticesSyms.clear();
    m_grabbedAffectedVerts.clear();
    m_snakeActiveVerts.clear();
    m_snakeActiveVertsSyms.clear();
    m_snakePrevTipPosSyms.clear();
    m_initialSymIntersections.clear();
    m_lassoPoints.clear();
    m_isLassoActive = false;
    m_gradIsDrawing = false;
    m_draggedSegment = nullptr;
    m_hasAlphaOrigin = false;
    m_hasRakeDir = false;
    if (m_isTubeMoveStroke) {
        m_isTubeMoveStroke = false;
        m_tubeMoveHitMesh = nullptr;
        m_tubeMoveHitPointIdx = -1;
        m_tubeMoveTargets.clear();
        m_tubeMoveOrigPoints.clear();
    }
}

void SculptManager::warmupMeshCaches(const Mesh* mesh) {
    if (!mesh) return;
    size_t strokeFacesCap = std::min((size_t)mesh->nbFaces, (size_t)4000000);
    if (m_iFacesCache.size() < strokeFacesCap) {
        m_iFacesCache.resize(strokeFacesCap);
    }
    size_t strokeVertsCap = std::min((size_t)mesh->nbVerts, (size_t)2000000);
    if (m_allAffectedVertsCache.size() < strokeVertsCap) {
        m_allAffectedVertsCache.resize(strokeVertsCap);
    }
    if (mesh->vertProxy.size() != mesh->verts.size()) {
        const_cast<Mesh*>(mesh)->vertProxy = mesh->verts;
    }
    const_cast<Mesh*>(mesh)->ensureTopologyBuilt();
    const_cast<Mesh*>(mesh)->ensureBVHBuilt();
    const_cast<Mesh*>(mesh)->bvh.warmup(mesh->nbVerts);
    mesh->computeLocalRadius();
    g_undoManager.warmupForMesh(mesh->m_id, mesh->nbVerts, mesh->nbFaces);
}

void SculptManager::executeStroke(Scene& scene, Mesh* mesh, Camera& camera, float mouseX, float mouseY, float currentPressure) {
    m_lastFrameProfile.reset();
    
    auto tFrameStart = std::chrono::high_resolution_clock::now();

    if (mesh) {
        mesh->ensureTopologyBuilt();
        mesh->ensureBVHBuilt();
    }

    if (m_firstStrokeFrame) {
        m_cachedInvMatrix = glm::inverse(mesh ? mesh->matrix : glm::mat4(1.0f));
        m_cachedCamWorldMatrix = glm::inverse(camera.getViewMatrix());
        if (mesh && mesh->isLayerRecording()) {
            Layer* layer = mesh->layerStack.getActive();
            if (layer) {
                m_strokeStartLayerDeltas = layer->deltaVerts;
            } else {
                m_strokeStartLayerDeltas.clear();
            }
        } else {
            m_strokeStartLayerDeltas.clear();
        }
    }
    const glm::mat4& invMatrix = m_cachedInvMatrix;

    Ray ray = camera.getRay(mouseX, mouseY);
    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

    BrushType activeBrush = m_currentBrush;
    bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
    bool isCtrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    bool isAlt = (SDL_GetModState() & KMOD_ALT) != 0;
    bool isShift = (SDL_GetModState() & KMOD_SHIFT) != 0;

    if (isShift) {
        activeBrush = BRUSH_SMOOTH;
    } else if (isMayaNav ? (isCtrl && !isAlt) : isCtrl) {
        if (m_currentBrush != BRUSH_POLYGROUP) {
            activeBrush = BRUSH_MASK;
        }
    }
    if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK)) {
        m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
    }

    if (m_firstStrokeFrame && activeBrush == BRUSH_RELAX && mesh) {
        const BrushSettings& rSettings = getSettings(BRUSH_RELAX);
        m_relaxEngine.beginStroke(mesh, rSettings.relaxAlgorithm, rSettings.relaxPreserveCreases, rSettings.relaxCreaseAngle);
    }

    bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC);
    bool isSnakeBrush = (activeBrush == BRUSH_SNAKEHOOK);

    bool isDeformBrush = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_POLYGROUP);
    if (mesh && mesh->hasLayers() && isDeformBrush && !mesh->isLayerRecording()) {
        return;
    }

    uint32_t intersectFaceId = 0xffffffff;

    // --- Stage 1: Raycast ---
    auto tStage = std::chrono::high_resolution_clock::now();
    if (m_isTubeMoveStroke && m_tubeTool) {
        if (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC) {
            glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
            float denom = glm::dot(camFrontLocal, localRayDir);
            if (std::abs(denom) > 1e-12f) {
                float t = glm::dot(camFrontLocal, m_initialIntersection - localRayOrigin) / denom;
                m_currentIntersection = localRayOrigin + localRayDir * t;
            } else {
                m_currentIntersection = m_initialIntersection;
            }
        } else {
            // Smooth brush (or other brushes affecting tube curve):
            // Project ray towards closest point on curves or camera plane
            float bestT = -1.0f;
            float bestDist2 = std::numeric_limits<float>::max();
            for (const auto& target : m_tubeMoveTargets) {
                if (!target.mesh || !target.mesh->tubeData) continue;
                const CurveSpline& spl = (target.mesh == m_tubeTool->getActiveTubeMesh()) ? m_tubeTool->getSpline() : target.mesh->tubeData->spline;
                for (size_t pi = 0; pi < spl.getPointCount(); ++pi) {
                    glm::vec3 ptPos = spl.getPoint(pi).position;
                    float t = glm::dot(ptPos - ray.origin, ray.dir);
                    if (t > 0.0f) {
                        glm::vec3 pOnRay = ray.origin + t * ray.dir;
                        glm::vec3 diff = ptPos - pOnRay;
                        float d2 = glm::dot(diff, diff);
                        if (d2 < bestDist2) {
                            bestDist2 = d2;
                            bestT = t;
                        }
                    }
                }
            }

            glm::vec3 worldHit;
            if (bestT > 0.0f && std::sqrt(bestDist2) < 50.0f) {
                worldHit = ray.origin + bestT * ray.dir;
            } else {
                glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
                float denom = glm::dot(camFrontLocal, localRayDir);
                if (std::abs(denom) > 1e-12f) {
                    float t = glm::dot(camFrontLocal, m_initialIntersection - localRayOrigin) / denom;
                    worldHit = glm::vec3(mesh ? mesh->matrix * glm::vec4(localRayOrigin + localRayDir * t, 1.0f) : glm::vec4(localRayOrigin + localRayDir * t, 1.0f));
                } else {
                    worldHit = glm::vec3(mesh ? mesh->matrix * glm::vec4(m_initialIntersection, 1.0f) : glm::vec4(m_initialIntersection, 1.0f));
                }
            }
            m_currentIntersection = glm::vec3(m_cachedInvMatrix * glm::vec4(worldHit, 1.0f));
        }

        m_currentIntersectionNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        m_currentIntersectionValid = true;
        m_lastValidIntersection = m_currentIntersection;
        m_lastValidIntersectionNormal = m_currentIntersectionNormal;
        m_hasAnyValidIntersection = true;
    } else if ((isGrabBrush || isSnakeBrush) && !m_firstStrokeFrame) {
        // Bypass raycast for grab/snake brushes on subsequent frames
        glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
        float denom = glm::dot(camFrontLocal, localRayDir);
        if (std::abs(denom) > 1e-12f) {
            float t = glm::dot(camFrontLocal, m_initialIntersection - localRayOrigin) / denom;
            m_currentIntersection = localRayOrigin + localRayDir * t;
        } else {
            m_currentIntersection = isSnakeBrush ? m_snakePrevTipPos : m_initialIntersection;
        }
        m_currentIntersectionValid = true;

        m_lastValidIntersection = m_currentIntersection;
        m_hasAnyValidIntersection = true;
    } else {
        if (!mesh) {
            m_currentIntersectionValid = false;
            m_lastFrameProfile.raycastMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
            return;
        }
        mesh->ensureBVHBuilt();
        RayHit hit;
        if (!mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
            m_currentIntersectionValid = false;
            m_lastFrameProfile.raycastMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
            return;
        }

        intersectFaceId = hit.faceId;
        m_currentIntersection = hit.position;
        if (intersectFaceId < (uint32_t)mesh->nbFaces && intersectFaceId * 3 + 2 < mesh->faceNormals.size()) {
            m_currentIntersectionNormal = glm::vec3(
                mesh->faceNormals[intersectFaceId * 3],
                mesh->faceNormals[intersectFaceId * 3 + 1],
                mesh->faceNormals[intersectFaceId * 3 + 2]
            );
        } else {
            m_currentIntersectionNormal = hit.normal;
        }

        m_currentIntersectionValid = true;
        m_lastValidIntersection = m_currentIntersection;
        m_lastValidIntersectionNormal = m_currentIntersectionNormal;
        m_hasAnyValidIntersection = true;

        if (m_firstStrokeFrame && intersectFaceId != 0xffffffff && mesh && !mesh->faceGroups.empty() && intersectFaceId < (uint32_t)mesh->nbFaces) {
            m_strokeTargetPolyGroup = mesh->faceGroups[intersectFaceId];
        }
        if (m_firstStrokeFrame && isSnakeBrush) {
            m_snakePrevTipPos = m_currentIntersection;
        }
    }
    m_lastFrameProfile.raycastMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();

    if (activeBrush == BRUSH_POLYGROUP) {
        bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
        bool isCtrl = (SDL_GetModState() & KMOD_CTRL) != 0;
        bool isAlt = (SDL_GetModState() & KMOD_ALT) != 0;
        bool eyedropperMod = isMayaNav ? (isCtrl && isAlt) : isAlt;
        if (eyedropperMod) {
            if (m_currentIntersectionValid && intersectFaceId != 0xffffffff) {
                m_activeGroupId = m_polyGroupTool.getGroupAtFace(mesh, intersectFaceId);
                std::cout << "[PolyGroup] Eyedropper picked Group ID: " << m_activeGroupId << std::endl;
            }
            return;
        } else if (isCtrl && !eyedropperMod) {
            if (m_currentIntersectionValid && intersectFaceId != 0xffffffff && m_firstStrokeFrame) {
                m_polyGroupTool.floodFillGroup(mesh, intersectFaceId, m_activeGroupId);
            }
            return;
        }
    }

    if (activeBrush == BRUSH_PAINT && getCurrentSettings().pickColor) {
        if (m_currentIntersectionValid && intersectFaceId != 0xffffffff) {
            pickColor(mesh, intersectFaceId, m_currentIntersection);
        }
        return;
    }

    glm::vec3 cameraPos = camera.computePosition();
    glm::mat4 baseMatrix = mesh ? mesh->matrix : glm::mat4(1.0f);
    glm::vec3 worldIntersection = glm::vec3(baseMatrix * glm::vec4(m_currentIntersection, 1.0f));
    float hitDepth = glm::distance(cameraPos, worldIntersection);

    float worldRadius = 0.0f;
    float brushRadius = getSettings(activeBrush).radius;
    if (camera.isOrthographic()) {
        worldRadius = brushRadius * 2.0f * camera.getOrthoZoom();
    } else {
        float fov_rad = camera.getFovDegrees() * (float)M_PI / 180.0f;
        float screenHeight = (float)camera.getHeight();
        if (screenHeight <= 0.0f) screenHeight = 1.0f;
        worldRadius = brushRadius * hitDepth * std::tan(fov_rad * 0.5f) * 2.0f / screenHeight;
    }

    glm::vec3 col0(baseMatrix[0][0], baseMatrix[0][1], baseMatrix[0][2]);
    float scale = glm::length(col0);
    if (scale < 1e-12f) scale = 1.0f;

    float sizeMultiplier = 1.0f;
    if (g_tablet.isPressureSizeEnabled() && g_tablet.isPressureEnabled()) {
        sizeMultiplier = currentPressure;
    }
    float localRadius = (worldRadius / scale) * sizeMultiplier;
    float intensity = getSettings(activeBrush).intensity;
    if (g_tablet.isPressureEnabled() && activeBrush != BRUSH_MOVE && activeBrush != BRUSH_ELASTIC) {
        intensity *= currentPressure;
    }
    float radius2 = localRadius * localRadius;

    if (m_isTubeMoveStroke && m_tubeTool) {
        if (m_lastTubeActiveBrush != activeBrush) {
            m_initialIntersection = m_currentIntersection;
            m_lastTubeActiveBrush = activeBrush;
            for (auto& target : m_tubeMoveTargets) {
                if (!target.mesh || !target.mesh->tubeData) continue;
                bool isPrimary = (target.mesh == m_tubeTool->getActiveTubeMesh());
                CurveSpline& spl = isPrimary ? m_tubeTool->getSpline() : target.mesh->tubeData->spline;
                target.origPoints.resize(spl.getPointCount());
                for (size_t i = 0; i < spl.getPointCount(); ++i) {
                    target.origPoints[i] = spl.getPoint(i).position;
                }
                if (target.mesh == m_tubeMoveHitMesh) {
                    m_tubeMoveOrigPoints = target.origPoints;
                }
            }
        }

        float worldRad = localRadius * scale;
        float focalShift = getCurrentSettings().focalShift;
        float exponent = std::pow(2.0f, (focalShift - 0.6f) * 2.0f) * 3.0f;

        if (activeBrush == BRUSH_SMOOTH) {
            glm::vec3 worldCurrentCenter = glm::vec3(baseMatrix * glm::vec4(m_currentIntersection, 1.0f));

            if (!m_tubeMoveTargets.empty()) {
                for (auto& target : m_tubeMoveTargets) {
                    if (!target.mesh || !target.mesh->tubeData) continue;
                    bool isPrimary = (target.mesh == m_tubeTool->getActiveTubeMesh());
                    CurveSpline& spline = isPrimary ? m_tubeTool->getSpline() : target.mesh->tubeData->spline;
                    size_t ptCount = spline.getPointCount();
                    if (ptCount < 3) continue;

                    std::vector<glm::vec3> newPositions(ptCount);
                    std::vector<float> newRadii(ptCount);
                    for (size_t i = 0; i < ptCount; ++i) {
                        newPositions[i] = spline.getPoint(i).position;
                        newRadii[i] = spline.getPoint(i).radius;
                    }

                    bool anyChanged = false;
                    for (size_t i = 1; i + 1 < ptCount; ++i) {
                        float d = glm::distance(spline.getPoint(i).position, worldCurrentCenter);
                        if (d < worldRad && worldRad > 1e-5f) {
                            float dist = d / worldRad;
                            float base = 1.0f - dist * dist;
                            float falloff = std::pow(std::max(0.0f, base), exponent);
                            float factor = falloff * intensity * 0.35f;
                            if (factor > 1e-5f) {
                                glm::vec3 pTarget = 0.5f * (spline.getPoint(i - 1).position + spline.getPoint(i + 1).position);
                                newPositions[i] = glm::mix(spline.getPoint(i).position, pTarget, factor);

                                float rTarget = 0.5f * (spline.getPoint(i - 1).radius + spline.getPoint(i + 1).radius);
                                newRadii[i] = glm::mix(spline.getPoint(i).radius, rTarget, factor);
                                anyChanged = true;
                            }
                        }
                    }

                    if (anyChanged) {
                        for (size_t i = 1; i + 1 < ptCount; ++i) {
                            spline.getPoint(i).position = newPositions[i];
                            spline.getPoint(i).radius = newRadii[i];
                        }
                        if (target.origPoints.size() == ptCount) {
                            for (size_t i = 0; i < ptCount; ++i) {
                                target.origPoints[i] = spline.getPoint(i).position;
                            }
                        }
                        if (target.mesh == m_tubeMoveHitMesh) {
                            m_tubeMoveOrigPoints = target.origPoints;
                        }
                        if (isPrimary) {
                            m_tubeTool->syncToActiveMesh();
                        }
                        m_tubeTool->rebuildTubeMesh(scene, target.mesh);
                    }
                }
            } else {
                auto& spline = m_tubeTool->getSpline();
                size_t ptCount = spline.getPointCount();
                if (ptCount >= 3) {
                    std::vector<glm::vec3> newPositions(ptCount);
                    std::vector<float> newRadii(ptCount);
                    for (size_t i = 0; i < ptCount; ++i) {
                        newPositions[i] = spline.getPoint(i).position;
                        newRadii[i] = spline.getPoint(i).radius;
                    }

                    bool anyChanged = false;
                    for (size_t i = 1; i + 1 < ptCount; ++i) {
                        float d = glm::distance(spline.getPoint(i).position, worldCurrentCenter);
                        if (d < worldRad && worldRad > 1e-5f) {
                            float dist = d / worldRad;
                            float base = 1.0f - dist * dist;
                            float falloff = std::pow(std::max(0.0f, base), exponent);
                            float factor = falloff * intensity * 0.35f;
                            if (factor > 1e-5f) {
                                glm::vec3 pTarget = 0.5f * (spline.getPoint(i - 1).position + spline.getPoint(i + 1).position);
                                newPositions[i] = glm::mix(spline.getPoint(i).position, pTarget, factor);

                                float rTarget = 0.5f * (spline.getPoint(i - 1).radius + spline.getPoint(i + 1).radius);
                                newRadii[i] = glm::mix(spline.getPoint(i).radius, rTarget, factor);
                                anyChanged = true;
                            }
                        }
                    }

                    if (anyChanged) {
                        for (size_t i = 1; i + 1 < ptCount; ++i) {
                            spline.getPoint(i).position = newPositions[i];
                            spline.getPoint(i).radius = newRadii[i];
                        }
                        if (m_tubeMoveOrigPoints.size() == ptCount) {
                            for (size_t i = 0; i < ptCount; ++i) {
                                m_tubeMoveOrigPoints[i] = spline.getPoint(i).position;
                            }
                        }
                        m_tubeTool->rebuildMeshGeometry(scene);
                    }
                }
            }
        } else {
            bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
            bool isCtrl = (SDL_GetModState() & KMOD_CTRL) != 0;
            bool isAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            bool invertModifier = isMayaNav ? (isCtrl && isAlt) : isAlt;
            bool negative = getSettings(activeBrush).negative ^ invertModifier;

            glm::vec3 localDrag;
            if (negative) {
                glm::vec3 norm = (glm::length(m_currentIntersectionNormal) > 1e-6f) ? glm::normalize(m_currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * baseMatrix;
                glm::vec3 dragVec = m_currentIntersection - m_initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                float localAmount = amount / scale;
                localDrag = norm * (localAmount * intensity);
            } else {
                localDrag = (m_currentIntersection - m_initialIntersection) * intensity;
            }

            glm::vec3 worldDrag = glm::vec3(baseMatrix * glm::vec4(localDrag, 0.0f));
            glm::vec3 worldInitialCenter = glm::vec3(baseMatrix * glm::vec4(m_initialIntersection, 1.0f));

            if (!m_tubeMoveTargets.empty()) {
                float minD = std::numeric_limits<float>::max();
                for (const auto& target : m_tubeMoveTargets) {
                    for (const auto& p : target.origPoints) {
                        float d = glm::distance(p, worldInitialCenter);
                        if (d < minD) minD = d;
                    }
                }

                float effectiveRad = worldRad;
                if (m_tubeMoveHitPointIdx < 0 && minD >= effectiveRad && minD < 1e8f) {
                    effectiveRad = minD * 1.5f;
                }

                for (auto& target : m_tubeMoveTargets) {
                    if (!target.mesh || !target.mesh->tubeData) continue;
                    bool isPrimary = (target.mesh == m_tubeTool->getActiveTubeMesh());
                    CurveSpline& spline = isPrimary ? m_tubeTool->getSpline() : target.mesh->tubeData->spline;
                    size_t ptCount = spline.getPointCount();
                    if (target.origPoints.size() != ptCount) continue;

                    bool isHitMesh = (target.mesh == m_tubeMoveHitMesh);

                    for (size_t i = 0; i < ptCount; ++i) {
                        float falloff = 0.0f;
                        if (isHitMesh && m_tubeMoveHitPointIdx >= 0 && static_cast<int>(i) == m_tubeMoveHitPointIdx) {
                            falloff = 1.0f;
                        } else if (effectiveRad > 1e-5f) {
                            float d = glm::distance(target.origPoints[i], worldInitialCenter);
                            float dist = d / effectiveRad;
                            if (dist < 1.0f) {
                                float base = 1.0f - dist * dist;
                                falloff = std::pow(std::max(0.0f, base), exponent);
                            }
                        }

                        if (falloff > 0.0f) {
                            spline.getPoint(i).position = target.origPoints[i] + worldDrag * falloff;
                        } else {
                            spline.getPoint(i).position = target.origPoints[i];
                        }
                    }

                    if (isPrimary) {
                        m_tubeTool->syncToActiveMesh();
                    }
                    m_tubeTool->rebuildTubeMesh(scene, target.mesh);
                }
            } else {
                auto& spline = m_tubeTool->getSpline();
                size_t ptCount = spline.getPointCount();
                if (m_tubeMoveOrigPoints.size() == ptCount && ptCount > 0) {
                    float minD = std::numeric_limits<float>::max();
                    for (size_t i = 0; i < ptCount; ++i) {
                        float d = glm::distance(m_tubeMoveOrigPoints[i], worldInitialCenter);
                        if (d < minD) minD = d;
                    }

                    float effectiveRad = worldRad;
                    if (m_tubeMoveHitPointIdx < 0 && minD >= effectiveRad && minD < 1e8f) {
                        effectiveRad = minD * 1.5f;
                    }

                    for (size_t i = 0; i < ptCount; ++i) {
                        float falloff = 0.0f;
                        if (m_tubeMoveHitPointIdx >= 0 && static_cast<int>(i) == m_tubeMoveHitPointIdx) {
                            falloff = 1.0f;
                        } else if (effectiveRad > 1e-5f) {
                            float d = glm::distance(m_tubeMoveOrigPoints[i], worldInitialCenter);
                            float dist = d / effectiveRad;
                            if (dist < 1.0f) {
                                float base = 1.0f - dist * dist;
                                falloff = std::pow(std::max(0.0f, base), exponent);
                            }
                        }

                        if (falloff > 0.0f) {
                            spline.getPoint(i).position = m_tubeMoveOrigPoints[i] + worldDrag * falloff;
                        } else {
                            spline.getPoint(i).position = m_tubeMoveOrigPoints[i];
                        }
                    }

                    m_tubeTool->rebuildMeshGeometry(scene);
                }
            }
        }
        m_firstStrokeFrame = false;
        return;
    }

    // --- Stage 2: Pick Vertices ---
    tStage = std::chrono::high_resolution_clock::now();
    m_pickedVerticesCache.clear();
    std::vector<uint32_t>& pickedVertices = m_pickedVerticesCache;
    std::vector<glm::vec3> symScales;
    if (m_useSym) {
        symScales = getActiveSymmetryScales();
        if (m_firstStrokeFrame) {
            m_initialSymIntersections.resize(symScales.size());
            for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                m_initialSymIntersections[sIdx] = reflectPointSymmetry(m_initialIntersection, symScales[sIdx], mesh, m_symmetryMode);
            }
            m_grabbedVerticesSyms.resize(symScales.size());
            if (isSnakeBrush) {
                m_snakeActiveVertsSyms.resize(symScales.size());
                m_snakePrevTipPosSyms.resize(symScales.size());
                for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                    m_snakePrevTipPosSyms[sIdx] = m_initialSymIntersections[sIdx];
                }
            }
        }
        if (m_prePickedSymVerts.size() < symScales.size()) {
            m_prePickedSymVerts.resize(symScales.size());
        }
        for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
            m_prePickedSymVerts[sIdx].clear();
        }
    }

    bool culledAndFiltered = false;
    bool canDualPick = m_useSym && (symScales.size() == 1) && (!isSnakeBrush || m_firstStrokeFrame) && (!isGrabBrush || m_firstStrokeFrame);
    bool primaryUsedBVH = false;
    bool symUsedBVHInDual = false;
    bool primaryUsedTopo = false;
    double dualSec0Ms = 0.0, dualSec1Ms = 0.0, dualSec0CullMs = 0.0, dualSec1CullMs = 0.0;

    if (canDualPick) {
        glm::vec3 symCenter0 = reflectPointSymmetry(m_currentIntersection, symScales[0], mesh, m_symmetryMode);
        culledAndFiltered = true;
        m_lastFrameProfile.dualPickActive = true;

        // 1. Primary Pick & Cull with full multithreading
        {
            auto ts0 = std::chrono::high_resolution_clock::now();
            if (getSettings(activeBrush).topoCheck) {
                primaryUsedTopo = true;
                pickVerticesInSphereTopological(mesh, m_currentIntersection, radius2, intersectFaceId,
                                                m_vertTagFlags, m_vertTagEpoch, pickedVertices);
            } else {
                primaryUsedBVH = true;
                mesh->bvh.pickVerticesInSphere(
                    *mesh, m_currentIntersection,
                    radius2, pickedVertices, mesh->vertVisible.data(), 0
                );
            }
            auto tc0 = std::chrono::high_resolution_clock::now();
            if (getSettings(activeBrush).culling) {
                filterCullingVertices(pickedVertices, mesh, localRayDir);
            }
            if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty()) {
                filterPolyGroupVertices(pickedVertices, mesh, m_strokeTargetPolyGroup);
            }
            auto tc1 = std::chrono::high_resolution_clock::now();
            dualSec0CullMs = std::chrono::duration<double, std::milli>(tc1 - tc0).count();
            if (isSnakeBrush) {
                m_snakeActiveVerts = pickedVertices;
            } else if (isGrabBrush) {
                m_grabbedVertices = pickedVertices;
            }
            dualSec0Ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - ts0).count();
        }

        // 2. Symmetry Pick & Cull with full multithreading
        {
            auto ts1 = std::chrono::high_resolution_clock::now();
            symUsedBVHInDual = true;
            mesh->bvh.pickVerticesInSphere(
                *mesh, symCenter0,
                radius2, m_prePickedSymVerts[0], mesh->vertVisible.data(), 1
            );
            auto tc0 = std::chrono::high_resolution_clock::now();
            if (getSettings(activeBrush).culling && !m_prePickedSymVerts[0].empty()) {
                glm::vec3 symRayDir = reflectVectorSymmetry(localRayDir, symScales[0], mesh, m_symmetryMode);
                filterCullingVertices(m_prePickedSymVerts[0], mesh, symRayDir);
            }
            if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty() && !m_prePickedSymVerts[0].empty()) {
                filterPolyGroupVertices(m_prePickedSymVerts[0], mesh, m_strokeTargetPolyGroup);
            }
            auto tc1 = std::chrono::high_resolution_clock::now();
            dualSec1CullMs = std::chrono::duration<double, std::milli>(tc1 - tc0).count();
            if (isSnakeBrush) {
                if (m_snakeActiveVertsSyms.size() < 1) m_snakeActiveVertsSyms.resize(1);
                m_snakeActiveVertsSyms[0] = m_prePickedSymVerts[0];
            } else if (isGrabBrush) {
                if (m_grabbedVerticesSyms.size() < 1) m_grabbedVerticesSyms.resize(1);
                m_grabbedVerticesSyms[0] = m_prePickedSymVerts[0];
            }
            dualSec1Ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - ts1).count();
        }
        m_lastFrameProfile.dualPickSec0Ms = dualSec0Ms;
        m_lastFrameProfile.dualPickSec1Ms = dualSec1Ms;
        m_lastFrameProfile.dualPickCullMs = dualSec0CullMs + dualSec1CullMs;
    } else if (isSnakeBrush) {
        if (m_firstStrokeFrame) {
            if (getSettings(activeBrush).topoCheck) {
                primaryUsedTopo = true;
                pickVerticesInSphereTopological(mesh, m_currentIntersection, radius2, intersectFaceId,
                                                m_vertTagFlags, m_vertTagEpoch, pickedVertices);
            } else {
                primaryUsedBVH = true;
                mesh->bvh.pickVerticesInSphere(
                    *mesh, m_currentIntersection, radius2, pickedVertices, mesh->vertVisible.data(), 0
                );
            }
            if (getSettings(activeBrush).culling) {
                filterCullingVertices(pickedVertices, mesh, localRayDir);
            }
            if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty()) {
                filterPolyGroupVertices(pickedVertices, mesh, m_strokeTargetPolyGroup);
            }
            m_snakeActiveVerts = pickedVertices;
            culledAndFiltered = true;
        } else {
            pickedVertices = m_snakeActiveVerts;
            culledAndFiltered = true;
        }
    } else if (isGrabBrush && !m_firstStrokeFrame) {
        // Zero-copy: do not copy m_grabbedVertices into pickedVerticesCache
        culledAndFiltered = true;
    } else {
        if (getSettings(activeBrush).topoCheck) {
            primaryUsedTopo = true;
            pickVerticesInSphereTopological(mesh, m_currentIntersection, radius2, intersectFaceId,
                                            m_vertTagFlags, m_vertTagEpoch, pickedVertices);
        } else {
            primaryUsedBVH = true;
            mesh->bvh.pickVerticesInSphere(
                *mesh, m_currentIntersection, radius2, pickedVertices, mesh->vertVisible.data(), 0
            );
        }
    }
    m_lastFrameProfile.pickVertsMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
    m_lastFrameProfile.pickUsedTopo = primaryUsedTopo;
    if (primaryUsedBVH) {
        const BVHPickStats& ps0 = mesh->bvh.getLastPickStats(0);
        m_lastFrameProfile.pickCellsMs = ps0.collectCellsMs;
        m_lastFrameProfile.pickVisitedInitMs = ps0.visitedInitMs;
        m_lastFrameProfile.pickDedupMs = ps0.visitedInitMs;
        m_lastFrameProfile.pickFilterMs = ps0.filterVertsMs;
        m_lastFrameProfile.pickMergeMs = ps0.mergeMs;
        m_lastFrameProfile.pickCandLeaves = ps0.candLeaves;
        m_lastFrameProfile.pickCandFaces = ps0.candFaces;
        m_lastFrameProfile.pickUsedMT = ps0.usedMT;
        m_lastFrameProfile.pickThreads = ps0.numThreads;
    }
    if (symUsedBVHInDual) {
        const BVHPickStats& ps1 = mesh->bvh.getLastPickStats(1);
        m_lastFrameProfile.symPickCellsMs = ps1.collectCellsMs;
        m_lastFrameProfile.symPickVisitedInitMs = ps1.visitedInitMs;
        m_lastFrameProfile.symPickFilterMs = ps1.filterVertsMs;
        m_lastFrameProfile.symPickMergeMs = ps1.mergeMs;
        m_lastFrameProfile.symPickCandLeaves = ps1.candLeaves;
        m_lastFrameProfile.symPickCandFaces = ps1.candFaces;
        m_lastFrameProfile.symPickUsedMT = ps1.usedMT;
        m_lastFrameProfile.symPickThreads = ps1.numThreads;
    }
    const uint32_t* primaryVerts = (isGrabBrush && !m_firstStrokeFrame) ? m_grabbedVertices.data() : pickedVertices.data();
    const size_t primaryVertCount = (isGrabBrush && !m_firstStrokeFrame) ? m_grabbedVertices.size() : pickedVertices.size();
    m_lastFrameProfile.pickedVertCount = (int)primaryVertCount;

    // --- Stage 3: Culling & PolyGroup Filtering ---
    tStage = std::chrono::high_resolution_clock::now();
    if (!culledAndFiltered && (!isGrabBrush || m_firstStrokeFrame)) {
        if (getSettings(activeBrush).culling) {
            filterCullingVertices(pickedVertices, mesh, localRayDir);
        }
        if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty()) {
            filterPolyGroupVertices(pickedVertices, mesh, m_strokeTargetPolyGroup);
        }
        if (isGrabBrush) {
            m_grabbedVertices = pickedVertices;
        }
    }
    m_lastFrameProfile.cullingMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();

    std::vector<uint32_t> allAffectedVerts;
    if (primaryVertCount > 0) {
        if (!isGrabBrush || m_firstStrokeFrame) {
            allAffectedVerts.reserve(primaryVertCount * (m_useSym ? 2 : 1));
        }
        bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
        bool isCtrl = (SDL_GetModState() & KMOD_CTRL) != 0;
        bool isAlt = (SDL_GetModState() & KMOD_ALT) != 0;
        bool invertModifier = isMayaNav ? (isCtrl && isAlt) : isAlt;
        bool negative = getSettings(activeBrush).negative ^ invertModifier;

        if (isGrabBrush) {
            tStage = std::chrono::high_resolution_clock::now();
            if (!m_firstStrokeFrame) {
                const float* vertProxyData = mesh->vertProxy.data();
                float* vertsData = mesh->verts.data();
                if (!m_grabbedAffectedVerts.empty()) {
                    const uint32_t* affVerts = m_grabbedAffectedVerts.data();
                    int nbAff = (int)m_grabbedAffectedVerts.size();
                    #pragma omp parallel for schedule(static) if(nbAff > 1000)
                    for (int i = 0; i < nbAff; ++i) {
                        uint32_t v = affVerts[i];
                        uint32_t idx = v * 3;
                        vertsData[idx]     = vertProxyData[idx];
                        vertsData[idx + 1] = vertProxyData[idx + 1];
                        vertsData[idx + 2] = vertProxyData[idx + 2];
                    }
                } else {
                    int nbPicked = (int)primaryVertCount;
                    #pragma omp parallel for schedule(static) if(nbPicked > 1000)
                    for (int i = 0; i < nbPicked; ++i) {
                        uint32_t v = primaryVerts[i];
                        uint32_t idx = v * 3;
                        vertsData[idx]     = vertProxyData[idx];
                        vertsData[idx + 1] = vertProxyData[idx + 1];
                        vertsData[idx + 2] = vertProxyData[idx + 2];
                    }
                    if (m_useSym) {
                        for (const auto& symVertsList : m_grabbedVerticesSyms) {
                            int nbSym = (int)symVertsList.size();
                            #pragma omp parallel for schedule(static) if(nbSym > 1000)
                            for (int i = 0; i < nbSym; ++i) {
                                uint32_t v = symVertsList[i];
                                uint32_t idx = v * 3;
                                vertsData[idx]     = vertProxyData[idx];
                                vertsData[idx + 1] = vertProxyData[idx + 1];
                                vertsData[idx + 2] = vertProxyData[idx + 2];
                            }
                        }
                    }
                }
            }
            m_lastFrameProfile.vertProxyResetMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
        }

        // Cache area normal and center for Clay/Flatten/Brush tools on first frame
        if (m_firstStrokeFrame && (activeBrush == BRUSH_CLAY || activeBrush == BRUSH_CLAYBUILDUP || activeBrush == BRUSH_FLATTEN || activeBrush == BRUSH_SQUAREBRUSH || activeBrush == BRUSH_BRUSH || activeBrush == BRUSH_RAKE)) {
            auto tAreaStart = std::chrono::high_resolution_clock::now();
            float areaResults[7] = {0.0f};
            if (computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                areaResults
            )) {
                m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
            } else {
                m_cachedAreaNormal = m_currentIntersectionNormal;
                m_cachedAreaCenter = m_currentIntersection;
            }
            m_lastFrameProfile.areaComputeMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tAreaStart).count();
            m_clayAreaNormalFrame = 0;
            m_strokeFrameCounter = 0;
            m_prevClayIntersection = m_currentIntersection;
        } else {
            m_strokeFrameCounter++;
        }

        bool strokeAffectsPositions = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_MASK_GRADIENT_BLUR);
        bool strokeAffectsColors = (activeBrush == BRUSH_PAINT && getCurrentSettings().writeAlbedo);
        bool strokeAffectsMaterials = (activeBrush == BRUSH_PAINT && (getCurrentSettings().writeRoughness || getCurrentSettings().writeMetalness)) ||
                                       (activeBrush == BRUSH_MASK || activeBrush == BRUSH_MASK_GRADIENT_BLUR);

        // --- Stage 4: Undo Record ---
        bool skipGrabUndo = isGrabBrush && !m_firstStrokeFrame && m_grabUndoRecorded;
        if (!skipGrabUndo) {
            tStage = std::chrono::high_resolution_clock::now();
            if (isGrabBrush && !m_firstStrokeFrame) {
                g_undoManager.recordAffectedVertices(scene, mesh->m_id, m_grabbedVertices, strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials);
            } else {
                g_undoManager.recordAffectedVertices(scene, mesh->m_id, pickedVertices, strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials);
            }
            if (m_useSym) {
                for (const auto& symList : m_prePickedSymVerts) {
                    if (!symList.empty()) {
                        g_undoManager.recordAffectedVertices(scene, mesh->m_id, symList, strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials);
                    }
                }
            }
            m_lastFrameProfile.undoRecordMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
        } else {
            m_lastFrameProfile.undoRecordMs = 0.0;
        }

        // --- Stage 5: Primary Deform Pass ---
        tStage = std::chrono::high_resolution_clock::now();
        int primaryDeformed = doStrokePass(
            scene,
            mesh,
            activeBrush,
            negative,
            primaryVerts,
            primaryVertCount,
            m_currentIntersection,
            m_currentIntersectionNormal,
            m_initialIntersection,
            m_cachedAreaNormal,
            m_cachedAreaCenter,
            localRadius,
            intensity,
            mouseX,
            false,
            glm::vec3(1.0f),
            (isGrabBrush && !m_firstStrokeFrame) ? nullptr : &pickedVertices
        );
        m_lastFrameProfile.primaryDeformMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();

        if (primaryDeformed > 0 && (!isGrabBrush || m_firstStrokeFrame)) {
            allAffectedVerts.insert(allAffectedVerts.end(), primaryVerts, primaryVerts + primaryDeformed);
        }

        // --- Stage 6: Symmetry Pass ---
        tStage = std::chrono::high_resolution_clock::now();
        if (m_useSym) {
            if (m_firstStrokeFrame && m_initialSymIntersections.empty()) {
                m_initialSymIntersections.resize(symScales.size());
                for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                    m_initialSymIntersections[sIdx] = reflectPointSymmetry(m_initialIntersection, symScales[sIdx], mesh, m_symmetryMode);
                }
                m_grabbedVerticesSyms.resize(symScales.size());
                if (isSnakeBrush) {
                    m_snakeActiveVertsSyms.resize(symScales.size());
                    m_snakePrevTipPosSyms.resize(symScales.size());
                    for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                        m_snakePrevTipPosSyms[sIdx] = m_initialSymIntersections[sIdx];
                    }
                }
            }

            for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                glm::vec3 sScale = symScales[sIdx];
                glm::vec3 symCenter;
                std::vector<uint32_t> symVerts;
                bool fromPrePicked = false;

                auto tSymPick0 = std::chrono::high_resolution_clock::now();
                double thisSymCullMs = 0.0;

                if (isSnakeBrush) {
                    symCenter = reflectPointSymmetry(m_currentIntersection, sScale, mesh, m_symmetryMode);
                    if (m_firstStrokeFrame) {
                        if (sIdx < m_prePickedSymVerts.size() && !m_prePickedSymVerts[sIdx].empty()) {
                            symVerts = std::move(m_prePickedSymVerts[sIdx]);
                            fromPrePicked = true;
                        } else {
                            symVerts = mesh->bvh.pickVerticesInSphere(
                                *mesh, symCenter,
                                radius2, mesh->vertVisible.data(), 1
                            );
                            const BVHPickStats& ps1 = mesh->bvh.getLastPickStats(1);
                            m_lastFrameProfile.symPickCellsMs += ps1.collectCellsMs;
                            m_lastFrameProfile.symPickVisitedInitMs += ps1.visitedInitMs;
                            m_lastFrameProfile.symPickFilterMs += ps1.filterVertsMs;
                            m_lastFrameProfile.symPickMergeMs += ps1.mergeMs;
                            m_lastFrameProfile.symPickCandLeaves += ps1.candLeaves;
                            m_lastFrameProfile.symPickCandFaces += ps1.candFaces;
                            m_lastFrameProfile.symPickUsedMT = ps1.usedMT;
                            m_lastFrameProfile.symPickThreads = ps1.numThreads;

                            auto tSc0 = std::chrono::high_resolution_clock::now();
                            if (getSettings(activeBrush).culling && !symVerts.empty()) {
                                glm::vec3 symRayDir = reflectVectorSymmetry(localRayDir, sScale, mesh, m_symmetryMode);
                                filterCullingVertices(symVerts, mesh, symRayDir);
                            }
                            if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty() && !symVerts.empty()) {
                                filterPolyGroupVertices(symVerts, mesh, m_strokeTargetPolyGroup);
                            }
                            thisSymCullMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSc0).count();
                        }
                        if (sIdx >= m_snakeActiveVertsSyms.size()) m_snakeActiveVertsSyms.resize(sIdx + 1);
                        m_snakeActiveVertsSyms[sIdx] = symVerts;
                    } else {
                        if (sIdx < m_snakeActiveVertsSyms.size()) {
                            symVerts = m_snakeActiveVertsSyms[sIdx];
                        }
                    }
                } else if (isGrabBrush && !m_firstStrokeFrame) {
                    glm::vec3 symRayOrigin = reflectPointSymmetry(localRayOrigin, sScale, mesh, m_symmetryMode);
                    glm::vec3 symRayDir = reflectVectorSymmetry(localRayDir, sScale, mesh, m_symmetryMode);
                    const glm::vec3& initSymInter = (sIdx < m_initialSymIntersections.size()) ? m_initialSymIntersections[sIdx] : reflectPointSymmetry(m_initialIntersection, sScale, mesh, m_symmetryMode);
                    glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
                    glm::vec3 symCamFrontLocal = reflectVectorSymmetry(camFrontLocal, sScale, mesh, m_symmetryMode);
                    float denom = glm::dot(symCamFrontLocal, symRayDir);
                    if (std::abs(denom) > 1e-12f) {
                        float t = glm::dot(symCamFrontLocal, initSymInter - symRayOrigin) / denom;
                        symCenter = symRayOrigin + symRayDir * t;
                    } else {
                        symCenter = initSymInter;
                    }
                } else {
                    symCenter = reflectPointSymmetry(m_currentIntersection, sScale, mesh, m_symmetryMode);
                    if (sIdx < m_prePickedSymVerts.size() && !m_prePickedSymVerts[sIdx].empty()) {
                        symVerts = std::move(m_prePickedSymVerts[sIdx]);
                        fromPrePicked = true;
                    } else {
                        mesh->bvh.pickVerticesInSphere(
                            *mesh, symCenter,
                            radius2, symVerts, mesh->vertVisible.data(), 1
                        );
                        const BVHPickStats& ps1 = mesh->bvh.getLastPickStats(1);
                        m_lastFrameProfile.symPickCellsMs += ps1.collectCellsMs;
                        m_lastFrameProfile.symPickVisitedInitMs += ps1.visitedInitMs;
                        m_lastFrameProfile.symPickFilterMs += ps1.filterVertsMs;
                        m_lastFrameProfile.symPickMergeMs += ps1.mergeMs;
                        m_lastFrameProfile.symPickCandLeaves += ps1.candLeaves;
                        m_lastFrameProfile.symPickCandFaces += ps1.candFaces;
                        m_lastFrameProfile.symPickUsedMT = ps1.usedMT;
                        m_lastFrameProfile.symPickThreads = ps1.numThreads;

                        auto tSc0 = std::chrono::high_resolution_clock::now();
                        if (getSettings(activeBrush).culling && !symVerts.empty()) {
                            glm::vec3 symRayDir = reflectVectorSymmetry(localRayDir, sScale, mesh, m_symmetryMode);
                            filterCullingVertices(symVerts, mesh, symRayDir);
                        }

                        if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty() && !symVerts.empty()) {
                            filterPolyGroupVertices(symVerts, mesh, m_strokeTargetPolyGroup);
                        }
                        thisSymCullMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSc0).count();
                    }

                    if (isGrabBrush) {
                        if (sIdx >= m_grabbedVerticesSyms.size()) m_grabbedVerticesSyms.resize(sIdx + 1);
                        m_grabbedVerticesSyms[sIdx] = symVerts;
                    }
                }
                const uint32_t* pSymData = (isGrabBrush && !m_firstStrokeFrame && sIdx < m_grabbedVerticesSyms.size())
                    ? m_grabbedVerticesSyms[sIdx].data()
                    : symVerts.data();
                size_t numSymData = (isGrabBrush && !m_firstStrokeFrame && sIdx < m_grabbedVerticesSyms.size())
                    ? m_grabbedVerticesSyms[sIdx].size()
                    : symVerts.size();
                double thisSymPickMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSymPick0).count() - thisSymCullMs;
                m_lastFrameProfile.symPickMs += std::max(0.0, thisSymPickMs);
                m_lastFrameProfile.symCullMs += thisSymCullMs;

                if (numSymData > 0) {
                    glm::vec3 symAreaNormal = reflectVectorSymmetry(m_cachedAreaNormal, sScale, mesh, m_symmetryMode);
                    glm::vec3 symAreaCenter = reflectPointSymmetry(m_cachedAreaCenter, sScale, mesh, m_symmetryMode);
                    glm::vec3 symIntersectionNormal = reflectVectorSymmetry(m_currentIntersectionNormal, sScale, mesh, m_symmetryMode);
                    const glm::vec3& initSymInter = (sIdx < m_initialSymIntersections.size()) ? m_initialSymIntersections[sIdx] : reflectPointSymmetry(m_initialIntersection, sScale, mesh, m_symmetryMode);

                    if (!fromPrePicked && !skipGrabUndo) {
                        auto tUndoSym = std::chrono::high_resolution_clock::now();
                        if (isGrabBrush && !m_firstStrokeFrame) {
                            g_undoManager.recordAffectedVertices(scene, mesh->m_id, m_grabbedVerticesSyms[sIdx], strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials);
                        } else {
                            g_undoManager.recordAffectedVertices(scene, mesh->m_id, symVerts, strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials);
                        }
                        double uSymMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tUndoSym).count();
                        m_lastFrameProfile.undoRecordMs += uSymMs;
                        m_lastFrameProfile.symUndoMs += uSymMs;
                    }

                    auto tSymDef0 = std::chrono::high_resolution_clock::now();
                    int symDeformed = doStrokePass(
                        scene,
                        mesh,
                        activeBrush,
                        negative,
                        pSymData,
                        numSymData,
                        symCenter,
                        symIntersectionNormal,
                        initSymInter,
                        symAreaNormal,
                        symAreaCenter,
                        localRadius,
                        intensity,
                        mouseX,
                        true,
                        sScale,
                        (isGrabBrush && !m_firstStrokeFrame) ? nullptr : &symVerts
                    );
                    m_lastFrameProfile.symDeformMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSymDef0).count();

                    if (symDeformed > 0 && (!isGrabBrush || m_firstStrokeFrame)) {
                        allAffectedVerts.insert(allAffectedVerts.end(), pSymData, pSymData + symDeformed);
                    }
                }
            }
        }
        m_lastFrameProfile.symmetryMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
        if (isGrabBrush && m_firstStrokeFrame) {
            m_grabUndoRecorded = true;
        }
    }

    if (activeBrush == BRUSH_SNAKEHOOK) {
        m_snakePrevTipPos = m_currentIntersection;
    }

    const uint32_t* pAff = nullptr;
    size_t numAff = 0;

    double tagAllocMs = 0.0;
    if (isGrabBrush && !m_firstStrokeFrame && !m_grabbedAffectedVerts.empty()) {
        pAff = m_grabbedAffectedVerts.data();
        numAff = m_grabbedAffectedVerts.size();
        m_affectedMinV = m_grabbedMinV;
        m_affectedMaxV = m_grabbedMaxV;
        m_lastFrameProfile.sortDedupMs = 0.0;
    } else if (!allAffectedVerts.empty()) {
        tStage = std::chrono::high_resolution_clock::now();

        if (m_vertTagFlags.size() < (size_t)mesh->nbVerts) {
            auto tTagAlloc0 = std::chrono::high_resolution_clock::now();
            m_vertTagFlags.assign(mesh->nbVerts, uint8_t(0));
            tagAllocMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tTagAlloc0).count();
        }
        m_vertTagEpoch++;
        if (m_vertTagEpoch == 0) {
            std::fill(m_vertTagFlags.begin(), m_vertTagFlags.end(), uint8_t(0));
            m_vertTagEpoch = 1;
        }

        uint8_t curEpoch = m_vertTagEpoch;
        uint8_t* tagData = m_vertTagFlags.data();
        uint32_t* pVerts = allAffectedVerts.data();
        size_t total = allAffectedVerts.size();
        size_t writeIdx = 0;

        uint32_t minV = 0xFFFFFFFF;
        uint32_t maxV = 0;

        const int maxThreads = omp_get_max_threads();
        const bool inParallel = omp_in_parallel();

        if (total < 16384 || inParallel || maxThreads <= 1) {
            for (size_t i = 0; i < total; ++i) {
                uint32_t v = pVerts[i];
                if (v < (uint32_t)mesh->nbVerts && tagData[v] != curEpoch) {
                    tagData[v] = curEpoch;
                    pVerts[writeIdx++] = v;
                    minV = std::min(minV, v);
                    maxV = std::max(maxV, v);
                }
            }
            allAffectedVerts.resize(writeIdx);
        } else {
            static std::vector<std::vector<uint32_t>> s_threadVerts;
            if ((int)s_threadVerts.size() < maxThreads) {
                s_threadVerts.resize(maxThreads);
            }
            uint32_t globalMin = 0xFFFFFFFF;
            uint32_t globalMax = 0;

            #pragma omp parallel reduction(min:globalMin) reduction(max:globalMax)
            {
                int tid = omp_get_thread_num();
                auto& localVerts = s_threadVerts[tid];
                localVerts.clear();
                size_t estCount = (total / maxThreads) + 64;
                if (localVerts.capacity() < estCount) {
                    localVerts.reserve(estCount);
                }

                #pragma omp for schedule(static)
                for (int64_t i = 0; i < (int64_t)total; ++i) {
                    uint32_t v = pVerts[i];
                    if (v < (uint32_t)mesh->nbVerts && tagData[v] != curEpoch) {
                        std::atomic_ref<uint8_t> ref(tagData[v]);
                        if (ref.exchange(curEpoch, std::memory_order_relaxed) != curEpoch) {
                            localVerts.push_back(v);
                            globalMin = std::min(globalMin, v);
                            globalMax = std::max(globalMax, v);
                        }
                    }
                }
            }

            size_t offsets[129] = {0};
            int numMergeThreads = std::min(maxThreads, 128);
            for (int t = 0; t < numMergeThreads; ++t) {
                offsets[t + 1] = offsets[t] + s_threadVerts[t].size();
            }
            writeIdx = offsets[numMergeThreads];
            allAffectedVerts.resize(writeIdx);
            pVerts = allAffectedVerts.data();

            #pragma omp parallel for schedule(static) if(writeIdx > 8192)
            for (int t = 0; t < numMergeThreads; ++t) {
                size_t sz = s_threadVerts[t].size();
                if (sz > 0) {
                    std::memcpy(pVerts + offsets[t], s_threadVerts[t].data(), sz * sizeof(uint32_t));
                }
            }
            minV = globalMin;
            maxV = globalMax;
        }
        auto tDedupEnd = std::chrono::high_resolution_clock::now();
        m_lastFrameProfile.dedupOnlyMs = std::chrono::duration<double, std::milli>(tDedupEnd - tStage).count();
        bool isSculptDeform = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_MASK_GRADIENT_BLUR && activeBrush != BRUSH_POLYGROUP);
        if (isSculptDeform && writeIdx > 1) {
            radixSortVertices(allAffectedVerts, m_allAffectedVertsCache, maxV);
        }
        auto tSortEnd = std::chrono::high_resolution_clock::now();
        m_lastFrameProfile.radixSortMs = std::chrono::duration<double, std::milli>(tSortEnd - tDedupEnd).count();
        m_affectedMinV = minV;
        m_affectedMaxV = maxV;

        m_lastFrameProfile.sortDedupMs = std::chrono::duration<double, std::milli>(tSortEnd - tStage).count();
        if (isGrabBrush) {
            m_grabbedAffectedVerts = allAffectedVerts;
            m_grabbedMinV = minV;
            m_grabbedMaxV = maxV;
        }
        pAff = allAffectedVerts.data();
        numAff = allAffectedVerts.size();
    }
    m_lastFrameProfile.affectedVertCount = (int)numAff;

    if (numAff > 0) {
        bool isSculptDeform = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_MASK_GRADIENT_BLUR && activeBrush != BRUSH_POLYGROUP);

        if (isSculptDeform) {
            auto tTagAlloc1 = std::chrono::high_resolution_clock::now();
            if (m_tagFlags.size() < (size_t)mesh->nbFaces) {
                m_tagFlags.assign(mesh->nbFaces, 0);
            }
            size_t neededFaces = std::min((size_t)mesh->nbFaces, numAff * 8 + 1024);
            if (m_iFacesCache.size() < neededFaces) {
                m_iFacesCache.resize(neededFaces);
            }
            tagAllocMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tTagAlloc1).count();

            // --- Stage 7: Face Lookup ---
            uint32_t numIFaces = 0;
            if (isGrabBrush && !m_firstStrokeFrame && m_grabbedNumIFaces > 0) {
                numIFaces = m_grabbedNumIFaces;
                m_lastFrameProfile.faceLookupMs = 0.0;
            } else {
                tStage = std::chrono::high_resolution_clock::now();
                numIFaces = getFacesFromVerticesFast(
                    pAff,
                    numAff,
                    mesh->vrfStartCount.data(),
                    mesh->vertRingFace.data(),
                    m_iFacesCache.data(),
                    m_tagFlags.data(),
                    &m_tagEpoch,
                    mesh->nbFaces
                );
                m_lastFrameProfile.faceLookupMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
                if (isGrabBrush) {
                    m_grabbedNumIFaces = numIFaces;
                }
            }
            m_lastFrameProfile.affectedFaceCount = (int)numIFaces;

            // --- Stage 8: Face Normals & Boxes ---
            tStage = std::chrono::high_resolution_clock::now();
            updateFaceNormalsAndBoxes(
                mesh->verts.data(), mesh->nbVerts,
                mesh->faces.data(), mesh->nbFaces,
                m_iFacesCache.data(), numIFaces,
                mesh->faceNormals.data(),
                mesh->faceBoxes.data(),
                mesh->faceCenters.data()
            );
            m_lastFrameProfile.faceNormalsMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();

            // --- Stage 9: Vertex Normals ---
            tStage = std::chrono::high_resolution_clock::now();
            updateVertexNormals(
                pAff, numAff, mesh->nbVerts,
                mesh->vrfStartCount.data(),
                mesh->vertRingFace.data(),
                mesh->faceNormals.data(),
                mesh->normals.data()
            );
            m_lastFrameProfile.vertNormalsMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();

            // --- Stage 10: BVH Update ---
            if (isGrabBrush) {
                m_pendingBVHUpdate = true;
                m_lastFrameProfile.bvhUpdateMs = 0.0;
            } else {
                tStage = std::chrono::high_resolution_clock::now();
                mesh->bvh.refit(*mesh, m_iFacesCache.data(), numIFaces, true);
                m_lastFrameProfile.bvhUpdateMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStage).count();
                const BVHRefitStats& rs = mesh->bvh.getLastRefitStats();
                m_lastFrameProfile.bvhLeafDedupMs = rs.leafDedupMs;
                m_lastFrameProfile.bvhLeafAABBMs = rs.leafAABBMs;
                m_lastFrameProfile.bvhParentSortMs = rs.parentGatherMs;
                m_lastFrameProfile.bvhInnerUpdMs = rs.innerUpdateMs;
                m_lastFrameProfile.bvhDirtyLeaves = rs.dirtyLeaves;
                m_lastFrameProfile.bvhDirtyInner = rs.dirtyInnerNodes;
                m_lastFrameProfile.bvhUsedPrecomputed = rs.usedPrecomputed;
            }
        }

        uint32_t minV = m_affectedMinV;
        uint32_t maxV = m_affectedMaxV;
        if (minV > maxV && numAff > 0) {
            minV = pAff[0];
            maxV = pAff[numAff - 1];
        }

        if (mesh->isVertexDirty || mesh->isColorDirty || mesh->isMaterialDirty) {
            mesh->dirtyVertMin = std::min(mesh->dirtyVertMin, minV);
            mesh->dirtyVertMax = std::max(mesh->dirtyVertMax, maxV);
        } else {
            mesh->dirtyVertMin = minV;
            mesh->dirtyVertMax = maxV;
        }

        const auto& settings = getCurrentSettings();
        if (activeBrush == BRUSH_PAINT) {
            if (settings.writeAlbedo) {
                mesh->isColorDirty = true;
            }
            if (settings.writeRoughness || settings.writeMetalness) {
                mesh->isMaterialDirty = true;
            }
        } else if (activeBrush == BRUSH_MASK || activeBrush == BRUSH_MASK_GRADIENT_BLUR) {
            mesh->isMaterialDirty = true;
        } else if (activeBrush == BRUSH_POLYGROUP) {
            mesh->isFaceGroupDirty = true;
        } else {
            mesh->isVertexDirty = true;
        }

        auto tProxySync0 = std::chrono::high_resolution_clock::now();
        if (isSculptDeform && !isGrabBrush && mesh->vertProxy.size() == mesh->verts.size()) {
            const size_t numAff = allAffectedVerts.size();
            const uint32_t* pAff = allAffectedVerts.data();
            float* dst = mesh->vertProxy.data();
            const float* src = mesh->verts.data();
            const size_t maxVertIdx = mesh->verts.size() / 3;

            #pragma omp parallel for schedule(static) if(numAff > 4000)
            for (int64_t i = 0; i < (int64_t)numAff; ++i) {
                uint32_t v = pAff[i];
                if (v < maxVertIdx) {
                    dst[v * 3 + 0] = src[v * 3 + 0];
                    dst[v * 3 + 1] = src[v * 3 + 1];
                    dst[v * 3 + 2] = src[v * 3 + 2];
                }
            }
        }
        m_lastFrameProfile.vertProxySyncMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tProxySync0).count();
    }

    m_alphaOrigin = m_currentIntersection;
    m_hasAlphaOrigin = true;
    m_firstStrokeFrame = false;

    // Log high CPU stroke durations (> 5ms)
    double totalCpuMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tFrameStart).count();
    if (totalCpuMs > 5.0) {
        const auto& p = m_lastFrameProfile;
        uint32_t dirtySpanVerts = (mesh && mesh->dirtyVertMax >= mesh->dirtyVertMin) ? (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) : 0;
        double dirtySpanRatio = (p.affectedVertCount > 0) ? ((double)dirtySpanVerts / (double)p.affectedVertCount) : 0.0;
        if (p.dualPickActive) {
            sculpt_log("[SCULPT CPU STROKE] '%s' | Total CPU: %.2fms | Verts: %d (Picked: %d) | Faces: %d | TagAlloc: %.2fms | DirtySpan: %u (%.1fx)\n"
                      "  |- Raycast:        %6.2f ms\n"
                      "  |- Pick Vertices:  %6.2f ms [Dual sec0=%.2f sec1=%.2f cull=%.2f | BVH0(%s %dt): Cells %.2f vInit %.2f P2 %.2f Mrg %.2f L=%u F=%u | Sym(%s %dt): P2 %.2f L=%u F=%u]\n"
                      "  |- Culling/Group:  %6.2f ms\n"
                      "  |- Proxy Reset:    %6.2f ms\n"
                      "  |- Undo Record:    %6.2f ms (SymUndo: %.2f ms)\n"
                      "  |- Primary Deform: %6.2f ms (AreaNormal: %6.2f ms)\n"
                      "  |- Symmetry Pass:  %6.2f ms (Pick/Copy: %.2f, Cull: %.2f, Deform: %.2f, Undo: %.2f)\n"
                      "  |- Sort & Dedup:   %6.2f ms (Dedup: %.2f, Radix: %.2f)\n"
                      "  |- Face Lookup:    %6.2f ms (Verts input: %d)\n"
                      "  |- Face Normals:   %6.2f ms\n"
                      "  |- Vert Normals:   %6.2f ms\n"
                      "  |- BVH Update:     %6.2f ms (Dedup: %.2f, AABB: %.2f, ClimbSort: %.2f, Inner: %.2f | L=%u I=%u Pre=%d)\n"
                      "  \\- Proxy Sync:     %6.2f ms\n",
                      getBrushNameStr(activeBrush),
                      totalCpuMs, p.affectedVertCount, p.pickedVertCount, p.affectedFaceCount, tagAllocMs, dirtySpanVerts, dirtySpanRatio,
                      p.raycastMs,
                      p.pickVertsMs, p.dualPickSec0Ms, p.dualPickSec1Ms, p.dualPickCullMs,
                      p.pickUsedTopo ? "TOPO" : (p.pickUsedMT ? "MT" : "ST"), p.pickThreads,
                      p.pickCellsMs, p.pickVisitedInitMs, p.pickFilterMs, p.pickMergeMs, p.pickCandLeaves, p.pickCandFaces,
                      p.symPickUsedMT ? "MT" : "ST", p.symPickThreads, p.symPickFilterMs, p.symPickCandLeaves, p.symPickCandFaces,
                      p.cullingMs,
                      p.vertProxyResetMs,
                      p.undoRecordMs, p.symUndoMs,
                      p.primaryDeformMs, p.areaComputeMs,
                      p.symmetryMs, p.symPickMs, p.symCullMs, p.symDeformMs, p.symUndoMs,
                      p.sortDedupMs, p.dedupOnlyMs, p.radixSortMs,
                      p.faceLookupMs, p.affectedVertCount,
                      p.faceNormalsMs, p.vertNormalsMs,
                      p.bvhUpdateMs, p.bvhLeafDedupMs, p.bvhLeafAABBMs, p.bvhParentSortMs, p.bvhInnerUpdMs, p.bvhDirtyLeaves, p.bvhDirtyInner, p.bvhUsedPrecomputed ? 1 : 0,
                      p.vertProxySyncMs);
        } else {
            sculpt_log("[SCULPT CPU STROKE] '%s' | Total CPU: %.2fms | Verts: %d (Picked: %d) | Faces: %d | TagAlloc: %.2fms | DirtySpan: %u (%.1fx)\n"
                      "  |- Raycast:        %6.2f ms\n"
                      "  |- Pick Vertices:  %6.2f ms [BVH0(%s %dt): Cells %.2f vInit %.2f P2 %.2f Mrg %.2f L=%u F=%u]\n"
                      "  |- Culling/Group:  %6.2f ms\n"
                      "  |- Proxy Reset:    %6.2f ms\n"
                      "  |- Undo Record:    %6.2f ms (SymUndo: %.2f ms)\n"
                      "  |- Primary Deform: %6.2f ms (AreaNormal: %6.2f ms)\n"
                      "  |- Symmetry Pass:  %6.2f ms (Pick/Copy: %.2f, Cull: %.2f, Deform: %.2f, Undo: %.2f | SymBVH(%s %dt): P2 %.2f L=%u F=%u)\n"
                      "  |- Sort & Dedup:   %6.2f ms (Dedup: %.2f, Radix: %.2f)\n"
                      "  |- Face Lookup:    %6.2f ms (Verts input: %d)\n"
                      "  |- Face Normals:   %6.2f ms\n"
                      "  |- Vert Normals:   %6.2f ms\n"
                      "  |- BVH Update:     %6.2f ms (Dedup: %.2f, AABB: %.2f, ClimbSort: %.2f, Inner: %.2f | L=%u I=%u Pre=%d)\n"
                      "  \\- Proxy Sync:     %6.2f ms\n",
                      getBrushNameStr(activeBrush),
                      totalCpuMs, p.affectedVertCount, p.pickedVertCount, p.affectedFaceCount, tagAllocMs, dirtySpanVerts, dirtySpanRatio,
                      p.raycastMs,
                      p.pickVertsMs,
                      p.pickUsedTopo ? "TOPO" : (p.pickUsedMT ? "MT" : "ST"), p.pickThreads,
                      p.pickCellsMs, p.pickVisitedInitMs, p.pickFilterMs, p.pickMergeMs, p.pickCandLeaves, p.pickCandFaces,
                      p.cullingMs,
                      p.vertProxyResetMs,
                      p.undoRecordMs, p.symUndoMs,
                      p.primaryDeformMs, p.areaComputeMs,
                      p.symmetryMs, p.symPickMs, p.symCullMs, p.symDeformMs, p.symUndoMs,
                      p.symPickUsedMT ? "MT" : "ST", p.symPickThreads, p.symPickFilterMs, p.symPickCandLeaves, p.symPickCandFaces,
                      p.sortDedupMs, p.dedupOnlyMs, p.radixSortMs,
                      p.faceLookupMs, p.affectedVertCount,
                      p.faceNormalsMs, p.vertNormalsMs,
                      p.bvhUpdateMs, p.bvhLeafDedupMs, p.bvhLeafAABBMs, p.bvhParentSortMs, p.bvhInnerUpdMs, p.bvhDirtyLeaves, p.bvhDirtyInner, p.bvhUsedPrecomputed ? 1 : 0,
                      p.vertProxySyncMs);
        }
    }
}

glm::vec3 SculptManager::getAnchorWorldPos(const MeasurementAnchor& anchor) {
    if (anchor.type == MeasurementAnchor::VERTEX && anchor.mesh != nullptr) {
        Mesh* mesh = anchor.mesh;
        uint32_t vertIdx = anchor.vertIdx;
        if (vertIdx * 3 + 2 < mesh->verts.size()) {
            glm::vec3 localPos(
                mesh->verts[vertIdx * 3],
                mesh->verts[vertIdx * 3 + 1],
                mesh->verts[vertIdx * 3 + 2]
            );
            return glm::vec3(mesh->matrix * glm::vec4(localPos, 1.0f));
        }
    }
    return anchor.worldPos;
}

MeasurementAnchor SculptManager::pickAnchor(float mouseX, float mouseY, Scene& scene, const glm::vec3* referenceWorldPos) {
    Mesh* mesh = scene.getSelected();
    Camera& camera = scene.getCamera();

    if (mesh) {
        Ray ray = camera.getRay(mouseX, mouseY);
        glm::mat4 invMatrix = glm::inverse(mesh->matrix);
        glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

        mesh->ensureBVHBuilt();
        RayHit hit;
        uint32_t intersectFaceId = 0xffffffff;
        float minT = std::numeric_limits<float>::infinity();
        if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
            intersectFaceId = hit.faceId;
            minT = hit.t;
        }

        if (intersectFaceId != 0xffffffff) {
            glm::vec3 inter = localRayOrigin + minT * localRayDir;
            float bestDist2 = std::numeric_limits<float>::infinity();
            uint32_t closestVert = 0xffffffff;

            uint32_t faceVerts[4] = {
                mesh->faces[intersectFaceId * 4],
                mesh->faces[intersectFaceId * 4 + 1],
                mesh->faces[intersectFaceId * 4 + 2],
                mesh->faces[intersectFaceId * 4 + 3]
            };

            for (int k = 0; k < 4; ++k) {
                uint32_t vid = faceVerts[k];
                if (vid == 0xffffffff) break;
                glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                float dist2 = glm::dot(v - inter, v - inter);
                if (dist2 < bestDist2) {
                    bestDist2 = dist2;
                    closestVert = vid;
                }
            }

            if (closestVert != 0xffffffff) {
                MeasurementAnchor anchor;
                anchor.type = MeasurementAnchor::VERTEX;
                anchor.mesh = mesh;
                anchor.meshId = mesh ? mesh->getID() : 0;
                anchor.vertIdx = closestVert;
                glm::vec3 localPos(mesh->verts[closestVert * 3], mesh->verts[closestVert * 3 + 1], mesh->verts[closestVert * 3 + 2]);
                anchor.worldPos = glm::vec3(mesh->matrix * glm::vec4(localPos, 1.0f));
                return anchor;
            }
        }
    }

    Ray ray = camera.getRay(mouseX, mouseY);
    float depth = 0.5f;
    if (referenceWorldPos) {
        glm::vec3 screenPivot = camera.project(*referenceWorldPos);
        depth = screenPivot.z;
    } else {
        glm::vec3 screenPivot = camera.project(camera.getPivot());
        depth = screenPivot.z;
    }
    glm::vec3 worldPos = camera.unproject(mouseX, mouseY, depth);

    MeasurementAnchor anchor;
    anchor.type = MeasurementAnchor::FREE;
    anchor.worldPos = worldPos;
    return anchor;
}

void SculptManager::deleteMeasureSegment(Scene& scene, int index) {
    if (index < 0 || index >= (int)m_measureSegments.size()) return;
    std::string name = m_measureSegments[index].name.empty()
        ? ("Measure " + std::to_string(index + 1))
        : m_measureSegments[index].name;

    g_undoManager.pushMeasurementChange(scene, "Delete " + name, [this, index]() {
        if (index >= 0 && index < (int)m_measureSegments.size()) {
            bool wasRef = m_measureSegments[index].isReference;
            m_measureSegments.erase(m_measureSegments.begin() + index);
            if (wasRef && !m_measureSegments.empty()) {
                m_measureSegments[0].isReference = true;
            }
        }
    });
}

void SculptManager::clearMeasureSegments(Scene& scene) {
    if (m_measureSegments.empty()) return;
    g_undoManager.pushMeasurementChange(scene, "Clear Measure Tools", [this]() {
        m_measureSegments.clear();
        m_hasPending = false;
        m_draggedSegment = nullptr;
        m_hoveredSegment = nullptr;
        m_selectedMeasureIdx = -1;
    });
}

void SculptManager::deleteDividerSegment(Scene& scene, int index) {
    if (index < 0 || index >= (int)m_dividerSegments.size()) return;
    std::string name = m_dividerSegments[index].name.empty()
        ? ("Divider " + std::to_string(index + 1))
        : m_dividerSegments[index].name;

    g_undoManager.pushMeasurementChange(scene, "Delete " + name, [this, index]() {
        if (index >= 0 && index < (int)m_dividerSegments.size()) {
            m_dividerSegments.erase(m_dividerSegments.begin() + index);
        }
    });
}

void SculptManager::clearDividerSegments(Scene& scene) {
    if (m_dividerSegments.empty()) return;
    g_undoManager.pushMeasurementChange(scene, "Clear Divider Tools", [this]() {
        m_dividerSegments.clear();
        m_hasPending = false;
        m_draggedSegment = nullptr;
        m_hoveredSegment = nullptr;
        m_selectedDividerIdx = -1;
    });
}

void SculptManager::clearAllMeasurements(Scene* scene) {
    if (m_measureSegments.empty() && m_dividerSegments.empty()) return;
    if (scene) {
        g_undoManager.pushMeasurementChange(*scene, "Clear Measurements", [this]() {
            clearMeasurements();
        });
    } else {
        clearMeasurements();
    }
}

void SculptManager::validateSegments(Scene& scene) {
    const auto& meshes = scene.getMeshes();
    auto validate = [&](std::vector<MeasurementSegment>& segments) {
        for (auto& seg : segments) {
            if (seg.vertA.type == MeasurementAnchor::VERTEX) {
                if (!seg.vertA.mesh && seg.vertA.meshId != 0) {
                    seg.vertA.mesh = scene.getMeshById(seg.vertA.meshId);
                }
                if (seg.vertA.mesh && seg.vertA.meshId == 0) {
                    seg.vertA.meshId = seg.vertA.mesh->getID();
                }
            }
            if (seg.vertB.type == MeasurementAnchor::VERTEX) {
                if (!seg.vertB.mesh && seg.vertB.meshId != 0) {
                    seg.vertB.mesh = scene.getMeshById(seg.vertB.meshId);
                }
                if (seg.vertB.mesh && seg.vertB.meshId == 0) {
                    seg.vertB.meshId = seg.vertB.mesh->getID();
                }
            }
        }
        segments.erase(std::remove_if(segments.begin(), segments.end(), [&](const MeasurementSegment& seg) {
            if (seg.vertA.type == MeasurementAnchor::VERTEX) {
                if (!seg.vertA.mesh || std::find(meshes.begin(), meshes.end(), seg.vertA.mesh) == meshes.end()) return true;
                if (seg.vertA.vertIdx * 3 + 2 >= seg.vertA.mesh->verts.size()) return true;
            }
            if (seg.vertB.type == MeasurementAnchor::VERTEX) {
                if (!seg.vertB.mesh || std::find(meshes.begin(), meshes.end(), seg.vertB.mesh) == meshes.end()) return true;
                if (seg.vertB.vertIdx * 3 + 2 >= seg.vertB.mesh->verts.size()) return true;
            }
            return false;
        }), segments.end());
    };
    validate(m_measureSegments);
    validate(m_dividerSegments);
}


void SculptManager::handleEvent(const SDL_Event& event, Scene& scene) {
    Mesh* mesh = scene.getSelected();
    int activeVp = scene.getActiveViewport();
    if (mesh && !scene.isMeshRenderVisible(mesh, activeVp)) {
        mesh = nullptr;
    }
    Camera& camera = (scene.getSplitMode() != Scene::SplitMode::OFF && activeVp == 1 && scene.getCameraRight()) 
                     ? *scene.getCameraRight() 
                     : scene.getCamera();

    if (event.type == SDL_KEYDOWN) {
        if (m_maskExtractTool && m_maskExtractTool->isPreviewActive()) {
            if (m_maskExtractTool->handleKeyDown(scene, event.key.keysym.sym)) {
                return;
            }
        }
        if (m_currentBrush == BRUSH_QUAD_DRAW && m_quadDrawTool) {
            if (m_quadDrawTool->handleKeyDown(scene, event.key.keysym.sym)) {
                return;
            }
        }
        if (m_currentBrush == BRUSH_COMPONENT_SELECT && m_componentSelectTool && m_componentSelectTool->isBevelActive()) {
            if (m_componentSelectTool->handleKeyDown(scene, event.key.keysym.sym)) {
                return;
            }
        }
    }

    if (event.type == SDL_MOUSEBUTTONDOWN) {
        int mouseX = event.button.x;
        int mouseY = event.button.y;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;
        m_mouseDownX = mouseX;
        m_mouseDownY = mouseY;

        // Check if event is handled by camera navigation preset
        bool qdIntercept = false;
        if (m_currentBrush == BRUSH_QUAD_DRAW && m_quadDrawTool) {
            if (m_quadDrawTool->isDragging()) {
                qdIntercept = true;
            } else if (event.button.button == SDL_BUTTON_LEFT && m_quadDrawTool->isHoveringInteractive()) {
                qdIntercept = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT && m_quadDrawTool->getSubTool() == retopo::QuadDrawSubTool::MultiCut && m_quadDrawTool->getMultiCutPointsCount() > 0) {
                qdIntercept = true;
            }
        }
        if (!qdIntercept && m_cameraController.isNavigationEvent(event, camera.getRef2DMode())) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            return;
        }

        if (m_currentBrush == BRUSH_QUAD_DRAW) {
            if (m_quadDrawTool) {
                SDL_Keymod mod = SDL_GetModState();
                bool handled = m_quadDrawTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0, event.button.button);
                if (!handled && m_cameraController.isNavigationEvent(event, camera.getRef2DMode())) {
                    m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
                }
            }
            return;
        }

        if (m_currentBrush == BRUSH_COMPONENT_SELECT) {
            if (m_componentSelectTool) {
                SDL_Keymod mod = SDL_GetModState();
                bool handled = m_componentSelectTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_SHIFT) != 0, event.button.button);
                if (!handled && m_cameraController.isNavigationEvent(event, camera.getRef2DMode())) {
                    m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
                }
            }
            return;
        }

        if (m_currentBrush == BRUSH_EDGE_CREASE) {
            if (m_edgeCreaseTool) {
                SDL_Keymod mod = SDL_GetModState();
                bool handled = m_edgeCreaseTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_SHIFT) != 0, event.button.button);
                if (!handled && m_cameraController.isNavigationEvent(event, camera.getRef2DMode())) {
                    m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
                }
            }
            return;
        }

        if (m_currentBrush == BRUSH_CURVE_FOLDS) {
            if (m_curveFoldsTool) {
                SDL_Keymod mod = SDL_GetModState();
                bool handled = m_curveFoldsTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0);
                if (!handled && m_cameraController.isNavigationEvent(event, camera.getRef2DMode())) {
                    m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
                }
            }
            return;
        }

        // Left button:
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (camera.getRefDragEnabled()) {
                return;
            }
            SDL_Keymod mod = SDL_GetModState();

            // Check if SubTool selection is triggered based on preset modifier behavior
            bool ctrlAltSubTool = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltSubToolSelect : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
            bool isSubToolSelect = ctrlAltSubTool
                ? (((mod & KMOD_ALT) != 0) && ((mod & KMOD_CTRL) != 0))
                : (((mod & KMOD_ALT) != 0) && ((mod & KMOD_CTRL) == 0));

            if (isSubToolSelect) {
                float minT = std::numeric_limits<float>::infinity();
                Mesh* closestMesh = nullptr;
                uint32_t closestFaceId = 0xffffffff;
                float closestLocalMinT = std::numeric_limits<float>::infinity();
                glm::vec3 closestLocalRayOrigin{0.0f};
                glm::vec3 closestLocalRayDir{0.0f};

                Ray ray = camera.getRay((float)mouseX, (float)mouseY);

                for (Mesh* m : scene.getMeshes()) {
                    if (!m) continue;
                    if (!scene.isMeshRenderVisible(m, activeVp)) continue;

                    glm::mat4 invMatrix = glm::inverse(m->matrix);
                    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

                    m->ensureBVHBuilt();
                    RayHit hit;
                    if (m->bvh.raycast(*m, localRayOrigin, localRayDir, hit)) {
                        glm::vec3 worldHit = glm::vec3(m->matrix * glm::vec4(hit.position, 1.0f));
                        float worldT = glm::distance(ray.origin, worldHit);
                        if (worldT < minT) {
                            minT = worldT;
                            closestMesh = m;
                            closestFaceId = hit.faceId;
                            closestLocalMinT = hit.t;
                            closestLocalRayOrigin = localRayOrigin;
                            closestLocalRayDir = localRayDir;
                        }
                    }
                }

                if (closestMesh) {
                    if (closestMesh != scene.getSelected()) {
                        scene.setOrUnsetMesh(closestMesh, false);
                        return;
                    }

                    if (m_currentBrush == BRUSH_TRANSFORM) {
                        auto tStart = std::chrono::high_resolution_clock::now();
                        glm::vec3 localPt = closestLocalRayOrigin + closestLocalMinT * closestLocalRayDir;
                        glm::vec3 localNormal(0.0f, 0.0f, 1.0f);
                        if (closestFaceId < (uint32_t)closestMesh->nbFaces && !closestMesh->faceNormals.empty()) {
                            localNormal = glm::vec3(
                                closestMesh->faceNormals[closestFaceId * 3],
                                closestMesh->faceNormals[closestFaceId * 3 + 1],
                                closestMesh->faceNormals[closestFaceId * 3 + 2]
                            );
                        }
                        if (glm::length(localNormal) < 1e-6f) {
                            uint32_t v0Id = closestMesh->faces[closestFaceId * 4];
                            uint32_t v1Id = closestMesh->faces[closestFaceId * 4 + 1];
                            uint32_t v2Id = closestMesh->faces[closestFaceId * 4 + 2];
                            glm::vec3 v0(closestMesh->verts[v0Id * 3], closestMesh->verts[v0Id * 3 + 1], closestMesh->verts[v0Id * 3 + 2]);
                            glm::vec3 v1(closestMesh->verts[v1Id * 3], closestMesh->verts[v1Id * 3 + 1], closestMesh->verts[v1Id * 3 + 2]);
                            glm::vec3 v2(closestMesh->verts[v2Id * 3], closestMesh->verts[v2Id * 3 + 1], closestMesh->verts[v2Id * 3 + 2]);
                            localNormal = glm::cross(v1 - v0, v2 - v0);
                        }
                        if (glm::length(localNormal) > 1e-6f) {
                            localNormal = glm::normalize(localNormal);
                        } else {
                            localNormal = glm::vec3(0.0f, 0.0f, 1.0f);
                        }

                        glm::vec3 worldPt = glm::vec3(closestMesh->matrix * glm::vec4(localPt, 1.0f));
                        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(closestMesh->matrix)));
                        glm::vec3 worldNormal = glm::normalize(normalMatrix * localNormal);
                        if (glm::dot(worldNormal, ray.dir) > 0.0f) {
                            worldNormal = -worldNormal;
                        }

                        float sx = glm::length(glm::vec3(closestMesh->matrix[0]));
                        float sy = glm::length(glm::vec3(closestMesh->matrix[1]));
                        float sz = glm::length(glm::vec3(closestMesh->matrix[2]));
                        if (sx < 1e-6f) sx = 1.0f;
                        if (sy < 1e-6f) sy = 1.0f;
                        if (sz < 1e-6f) sz = 1.0f;

                        glm::vec3 zAxis = worldNormal;
                        glm::vec3 up = (std::abs(zAxis.y) < 0.999f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
                        glm::vec3 xAxis = glm::normalize(glm::cross(up, zAxis));
                        glm::vec3 yAxis = glm::cross(zAxis, xAxis);

                        glm::mat4 targetMatrix(1.0f);
                        targetMatrix[0] = glm::vec4(xAxis * sx, 0.0f);
                        targetMatrix[1] = glm::vec4(yAxis * sy, 0.0f);
                        targetMatrix[2] = glm::vec4(zAxis * sz, 0.0f);
                        targetMatrix[3] = glm::vec4(worldPt, 1.0f);

                        glm::mat4 beforeMatrix = closestMesh->matrix;
                        glm::mat4 beforeLocalTransform = (closestMesh->isPrimitive && closestMesh->primitiveData) ? closestMesh->primitiveData->localTransform : glm::mat4(1.0f);
                        std::vector<float> beforeVerts = closestMesh->verts;
                        std::vector<float> beforeNormals = closestMesh->normals;

                        glm::mat4 deltaLocalMatrix = glm::inverse(closestMesh->matrix) * targetMatrix;
                        glm::mat4 deltaLocalMatrixInv = glm::inverse(deltaLocalMatrix);
                        glm::mat3 enMatrix = glm::transpose(glm::inverse(glm::mat3(deltaLocalMatrixInv)));

                        int nVerts = closestMesh->nbVerts;
#pragma omp parallel for schedule(static) if(nVerts > 2000)
                        for (int i = 0; i < nVerts; ++i) {
                            glm::vec4 pos(closestMesh->verts[i * 3], closestMesh->verts[i * 3 + 1], closestMesh->verts[i * 3 + 2], 1.0f);
                            glm::vec4 newPos = deltaLocalMatrixInv * pos;
                            closestMesh->verts[i * 3]     = newPos.x;
                            closestMesh->verts[i * 3 + 1] = newPos.y;
                            closestMesh->verts[i * 3 + 2] = newPos.z;

                            glm::vec3 normal(closestMesh->normals[i * 3], closestMesh->normals[i * 3 + 1], closestMesh->normals[i * 3 + 2]);
                            glm::vec3 newNormal = glm::normalize(enMatrix * normal);
                            closestMesh->normals[i * 3]     = newNormal.x;
                            closestMesh->normals[i * 3 + 1] = newNormal.y;
                            closestMesh->normals[i * 3 + 2] = newNormal.z;
                        }

                        closestMesh->matrix = targetMatrix;
                        if (closestMesh->isPrimitive && closestMesh->primitiveData) {
                            closestMesh->primitiveData->localTransform = deltaLocalMatrixInv * closestMesh->primitiveData->localTransform;
                            closestMesh->primitiveData->centerOffset = glm::vec3(closestMesh->primitiveData->localTransform[3]);
                        }
                        closestMesh->postInit();
                        closestMesh->isDirty = true;

                        TransformUndoEntry undoEntry("Alt+Click Pivot Surface Align");
                        TransformMeshState meshUndoState;
                        meshUndoState.meshId = closestMesh->getID();
                        meshUndoState.beforeMatrix = beforeMatrix;
                        meshUndoState.afterMatrix = targetMatrix;
                        if (closestMesh->isPrimitive && closestMesh->primitiveData) {
                            meshUndoState.isPrimitive = true;
                            meshUndoState.beforeLocalTransform = beforeLocalTransform;
                            meshUndoState.afterLocalTransform = closestMesh->primitiveData->localTransform;
                        }
                        meshUndoState.hasVertexDeformation = true;
                        meshUndoState.beforeVerts = std::move(beforeVerts);
                        meshUndoState.afterVerts = closestMesh->verts;
                        meshUndoState.beforeNormals = std::move(beforeNormals);
                        meshUndoState.afterNormals = closestMesh->normals;
                        undoEntry.m_meshStates.push_back(std::move(meshUndoState));
                        g_undoManager.pushTransformChange(scene, std::move(undoEntry));

                        double totalMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
                        sculpt_log("[TRANSFORM TOOL PROFILE] Alt+Click Pivot Surface Align | Mesh: '%s' (Verts: %d) | Total: %.2fms\n",
                                  closestMesh->outlinerName.c_str(), closestMesh->nbVerts, totalMs);
                        return;

                    }
                } else {
                    if (m_currentBrush == BRUSH_TRANSFORM) {
                        return;
                    }
                }
            }

            if (m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) {
                if (mod & KMOD_ALT) {
                    return;
                }

                float mouseX = (float)event.button.x;
                float mouseY = (float)event.button.y;
                float threshold = 15.0f;

                auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

                m_draggedSegment = nullptr;
                m_draggedVertexKey = "";

                // Check for dragging existing segment endpoint
                for (auto& seg : segments) {
                    glm::vec3 worldA = getAnchorWorldPos(seg.vertA);
                    glm::vec3 worldB = getAnchorWorldPos(seg.vertB);
                    glm::vec3 screenA = camera.project(worldA);
                    glm::vec3 screenB = camera.project(worldB);

                    float distA = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenA.x, screenA.y));
                    float distB = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenB.x, screenB.y));

                    if (distA < threshold && distA <= distB) {
                        m_draggedSegment = &seg;
                        m_draggedVertexKey = "vertA";
                        scene.pushHistoryState();
                        return;
                    }
                    if (distB < threshold) {
                        m_draggedSegment = &seg;
                        m_draggedVertexKey = "vertB";
                        scene.pushHistoryState();
                        return;
                    }
                }

                // If not dragging, start a new segment
                MeasurementAnchor anchor = pickAnchor(mouseX, mouseY, scene, nullptr);
                m_pendingAnchorA = anchor;
                m_pendingAnchorB = anchor;
                m_hasPending = true;
                scene.pushHistoryState();
                return;
            }

            if (m_maskExtractTool && m_maskExtractTool->isPreviewActive()) {
                if (m_maskExtractTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0)) {
                    return;
                }
            }

            if (m_currentBrush == BRUSH_EXTRACT) {
                if (m_maskExtractTool) {
                    if (!m_maskExtractTool->isPreviewActive()) {
                        Mesh* sel = scene.getSelected();
                        if (sel) m_maskExtractTool->startPreview(scene, sel);
                    }
                    if (m_maskExtractTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0)) {
                        return;
                    }
                }
                return;
            }

            if (m_primitiveTool && m_primitiveTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0)) {
                return;
            }

            if (m_currentBrush == BRUSH_TRANSFORM) {
                if (mod & KMOD_ALT) {
                    return;
                }
                return;
            }

            if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
                if (m_armatureTool->start(scene, camera, (float)mouseX, (float)mouseY, mod & KMOD_CTRL, mod & KMOD_ALT)) {
                    // Handled
                }
                return; // Return anyway, we don't want standard sculpting
            }

            if (m_currentBrush == BRUSH_POSE) {
                if (m_poseTool) {
                    m_poseTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0);
                }
                return;
            }

            if (m_currentBrush == BRUSH_TUBE && !((mod & KMOD_SHIFT) != 0)) {
                if (m_tubeTool) {
                    m_tubeTool->handleMouseDown(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0);
                }
                return;
            }

            bool isVisibilityTool = (m_currentBrush == BRUSH_VISIBILITY);
            bool isLassoMode = (mod & KMOD_CTRL) && (mod & KMOD_SHIFT);

            if (isVisibilityTool || isLassoMode) {
                m_isLassoActive = true;
                m_isMaskLasso = false;
                m_lassoPoints.clear();
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                m_lassoAlt = (mod & KMOD_ALT) != 0;
                return;
            }

            if (m_currentBrush == BRUSH_CLIP_CURVE) {
                m_isClipCurveActive = true;
                m_clipCurveAlt = (mod & KMOD_ALT) != 0;
                m_clipCurvePoints.clear();
                m_clipCurvePoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                return;
            }

            if (m_currentBrush == BRUSH_TRIM) {
                m_isTrimLassoActive = true;
                m_trimLassoAlt = (mod & KMOD_ALT) != 0;
                m_trimLassoPoints.clear();
                m_trimLassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                sculpt_log("[TrimLasso] MouseDown: started lasso at (%.0f, %.0f), alt=%d\n",
                           (float)mouseX, (float)mouseY, m_trimLassoAlt ? 1 : 0);
                return;
            }

            if (m_currentBrush == BRUSH_SPLIT) {
                m_isSplitLassoActive = true;
                m_splitLassoAlt = (mod & KMOD_ALT) != 0;
                m_splitLassoPoints.clear();
                m_splitLassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                sculpt_log("[SplitTool] MouseDown: started split at (%.0f, %.0f), alt=%d, mode=%d\n",
                           (float)mouseX, (float)mouseY, m_splitLassoAlt ? 1 : 0, m_brushSettings[BRUSH_SPLIT].splitMode);
                return;
            }

            if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh) {
                if (mod & KMOD_ALT) {
                    return;
                }
                glm::vec2 mousePos((float)mouseX, (float)mouseY);
                bool interactPoint = false;
                if (m_gradActive) {
                    float distA = glm::distance(mousePos, m_gradPointA);
                    float distB = glm::distance(mousePos, m_gradPointB);
                    if (distA < 20.0f) {
                        m_gradActivePoint = 'A';
                        interactPoint = true;
                    } else if (distB < 20.0f) {
                        m_gradActivePoint = 'B';
                        interactPoint = true;
                    }
                }

                if (interactPoint) {
                    scene.pushHistoryState();
                    m_gradIsDrawing = false;
                } else {
                    scene.pushHistoryState();
                    m_gradPointA = mousePos;
                    m_gradPointB = mousePos;
                    m_gradActivePoint = 'B';
                    m_gradIsDrawing = true;
                    m_gradActive = true;
                }

                int nbVerts = mesh->nbVerts;
                m_origMasks.resize(nbVerts);
                std::vector<float> tempMasks(nbVerts);
                for (int i = 0; i < nbVerts; ++i) {
                    m_origMasks[i] = mesh->materials[i * 3 + 2];
                    tempMasks[i] = mesh->materials[i * 3 + 2];
                }

                std::vector<uint32_t> iVerts;
                iVerts.reserve(nbVerts);
                for (int i = 0; i < nbVerts; ++i) {
                    if (tempMasks[i] < 1.0f) {
                        iVerts.push_back(i);
                    }
                }

                if (!iVerts.empty()) {
                    int iterations = getCurrentSettings().maskSharpenBlurIterations;
                    std::vector<uint8_t> visited(nbVerts, 0);
                    for (uint32_t v : iVerts) visited[v] = 1;
                    std::vector<uint32_t> queue = iVerts;
                    size_t head = 0;
                    for (int step = 0; step < iterations; ++step) {
                        size_t size = queue.size();
                        while (head < size) {
                            uint32_t id = queue[head++];
                            uint32_t start = mesh->vrvStartCount[id * 2];
                            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
                            for (uint32_t j = start; j < start + count; ++j) {
                                uint32_t neighbor = mesh->vertRingVert[j];
                                if (!visited[neighbor]) {
                                    visited[neighbor] = 1;
                                    queue.push_back(neighbor);
                                }
                            }
                        }
                    }
                    iVerts = queue;
                    ::blurMask(
                        iVerts.data(), (int)iVerts.size(),
                        mesh->vrvStartCount.data(),
                        mesh->vertRingVert.data(),
                        mesh->vertOnEdge.data(),
                        iterations,
                        tempMasks.data()
                    );
                }
                m_blurredMasks = tempMasks;
                m_gradActiveVerts = iVerts;
                return;
            }

            bool isTubeSelected = (mesh && mesh->isTube && mesh->tubeData != nullptr && m_tubeTool != nullptr);
            if (isTubeSelected && m_tubeTool->getActiveTubeMesh() != mesh) {
                m_tubeTool->loadFromMesh(mesh);
            }

            // Perform picking intersection test to see if we hit the mesh
            bool hitMesh = false;
            float minT = std::numeric_limits<float>::infinity();
            uint32_t intersectFaceId = 0xffffffff;
            glm::vec3 localRayOrigin{0.0f};
            glm::vec3 localRayDir{0.0f};

            if (mesh) {
                Ray ray = camera.getRay((float)mouseX, (float)mouseY);
                glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

                mesh->ensureBVHBuilt();
                RayHit hit;
                if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
                    minT = hit.t;
                    intersectFaceId = hit.faceId;
                    hitMesh = true;
                }
            }

            m_isTubeMoveStroke = false;
            m_tubeMoveHitMesh = nullptr;
            m_tubeMoveHitPointIdx = -1;
            m_tubeMoveTargets.clear();
            m_tubeMoveOrigPoints.clear();

            std::vector<Mesh*> selectedTubes;
            for (Mesh* sm : scene.getSelectedMeshes()) {
                if (sm && sm->isTube && sm->tubeData != nullptr) {
                    if (std::find(selectedTubes.begin(), selectedTubes.end(), sm) == selectedTubes.end()) {
                        selectedTubes.push_back(sm);
                    }
                }
            }
            if (mesh && mesh->isTube && mesh->tubeData != nullptr) {
                if (std::find(selectedTubes.begin(), selectedTubes.end(), mesh) == selectedTubes.end()) {
                    selectedTubes.push_back(mesh);
                }
            }

            bool isShift = (mod & KMOD_SHIFT) != 0;
            bool isSmooth = (m_currentBrush == BRUSH_SMOOTH || isShift);
            bool isMove = (m_currentBrush == BRUSH_MOVE || m_currentBrush == BRUSH_DRAG);

            if (m_tubeTool && !selectedTubes.empty() && (isMove || isSmooth)) {
                Mesh* hitTubeMesh = nullptr;
                int hitPoint = -1;
                float brushScreenRad = getSettings(isSmooth ? BRUSH_SMOOTH : m_currentBrush).radius;
                float closestPtDist = 16.0f;
                for (Mesh* tMesh : selectedTubes) {
                    const CurveSpline& spl = (tMesh == m_tubeTool->getActiveTubeMesh()) ? m_tubeTool->getSpline() : tMesh->tubeData->spline;
                    int ptIdx = m_tubeTool->hitTestTubeControlPoints(camera, spl, (float)mouseX, (float)mouseY, closestPtDist);
                    if (ptIdx != -1) {
                        glm::vec3 sProj = camera.project(spl.getPoint(ptIdx).position);
                        float d = glm::distance(glm::vec2(sProj.x, sProj.y), glm::vec2((float)mouseX, (float)mouseY));
                        if (d < closestPtDist) {
                            closestPtDist = d;
                            hitPoint = ptIdx;
                            hitTubeMesh = tMesh;
                        }
                    }
                }

                // If no control point hit, check if mouse clicked on tube mesh surface
                if (hitPoint == -1) {
                    if (hitMesh && mesh && mesh->isTube && mesh->tubeData != nullptr) {
                        hitTubeMesh = mesh;
                    } else {
                        for (Mesh* tMesh : selectedTubes) {
                            Ray ray = camera.getRay((float)mouseX, (float)mouseY);
                            glm::mat4 invMatrix = glm::inverse(tMesh->matrix);
                            glm::vec3 lOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                            glm::vec3 lDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));
                            tMesh->ensureBVHBuilt();
                            RayHit hit;
                            if (tMesh->bvh.raycast(*tMesh, lOrigin, lDir, hit) && hit.t < minT) {
                                minT = hit.t;
                                intersectFaceId = hit.faceId;
                                hitMesh = true;
                                hitTubeMesh = tMesh;
                                localRayOrigin = lOrigin;
                                localRayDir = lDir;
                            }
                        }
                    }
                }

                // If neither point nor surface mesh hit, check curve segments
                float segT = 0.5f;
                int hitSeg = -1;
                if (hitPoint == -1 && !hitMesh) {
                    float closestSegDist = std::max(18.0f, brushScreenRad);
                    for (Mesh* tMesh : selectedTubes) {
                        const CurveSpline& spl = (tMesh == m_tubeTool->getActiveTubeMesh()) ? m_tubeTool->getSpline() : tMesh->tubeData->spline;
                        float outT = 0.5f;
                        int seg = m_tubeTool->findClosestSegmentOnSpline(camera, spl, (float)mouseX, (float)mouseY, closestSegDist, &outT);
                        if (seg != -1) {
                            hitSeg = seg;
                            segT = outT;
                            hitTubeMesh = tMesh;
                            closestSegDist = 12.0f;
                        }
                    }
                }

                if (hitTubeMesh && (hitPoint != -1 || hitMesh || hitSeg != -1)) {
                    m_isTubeMoveStroke = true;
                    m_lastTubeActiveBrush = isSmooth ? BRUSH_SMOOTH : m_currentBrush;
                    m_tubeMoveHitMesh = hitTubeMesh;
                    m_tubeMoveHitPointIdx = hitPoint;

                    m_tubeTool->syncToActiveMesh();

                    m_tubeMoveTargets.clear();
                    m_tubeMoveOrigPoints.clear();
                    for (Mesh* tMesh : selectedTubes) {
                        TubeMoveTarget target;
                        target.mesh = tMesh;
                        const CurveSpline& spl = (tMesh == m_tubeTool->getActiveTubeMesh()) ? m_tubeTool->getSpline() : tMesh->tubeData->spline;
                        for (size_t i = 0; i < spl.getPointCount(); ++i) {
                            target.origPoints.push_back(spl.getPoint(i).position);
                        }
                        if (tMesh == hitTubeMesh) {
                            m_tubeMoveOrigPoints = target.origPoints;
                        }
                        m_tubeMoveTargets.push_back(std::move(target));
                    }

                    Ray ray = camera.getRay((float)mouseX, (float)mouseY);
                    glm::vec3 camFacingNormal = (glm::length(ray.dir) > 1e-6f) ? -glm::normalize(ray.dir) : glm::vec3(0.0f, 0.0f, 1.0f);

                    const CurveSpline& hitSpl = (hitTubeMesh == m_tubeTool->getActiveTubeMesh()) ? m_tubeTool->getSpline() : hitTubeMesh->tubeData->spline;

                    glm::mat4 invMat = glm::inverse(hitTubeMesh->matrix);
                    glm::mat4 camWorldMatrix = glm::inverse(camera.getViewMatrix());
                    glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(invMat * glm::vec4(glm::vec3(camWorldMatrix[2]), 0.0f)));
                    glm::vec3 lOrigin = glm::vec3(invMat * glm::vec4(ray.origin, 1.0f));
                    glm::vec3 lDir = glm::normalize(glm::vec3(invMat * glm::vec4(ray.dir, 0.0f)));

                    glm::vec3 refPtLocal;
                    if (hitPoint != -1) {
                        if (hitTubeMesh == m_tubeTool->getActiveTubeMesh()) {
                            m_tubeTool->setSelectedPointIndex(hitPoint);
                        }
                        glm::vec3 ptWorld = hitSpl.getPoint(hitPoint).position;
                        refPtLocal = glm::vec3(invMat * glm::vec4(ptWorld, 1.0f));
                    } else if (hitMesh) {
                        refPtLocal = localRayOrigin + minT * localRayDir;
                    } else {
                        float globalU = (static_cast<float>(hitSeg) + segT) / static_cast<float>(std::max(1, (int)hitSpl.getPointCount() - 1));
                        glm::vec3 curveWorld = hitSpl.evaluatePosition(globalU);
                        refPtLocal = glm::vec3(invMat * glm::vec4(curveWorld, 1.0f));
                    }

                    float denom = glm::dot(camFrontLocal, lDir);
                    if (std::abs(denom) > 1e-12f) {
                        float t = glm::dot(camFrontLocal, refPtLocal - lOrigin) / denom;
                        m_initialIntersection = lOrigin + lDir * t;
                    } else {
                        m_initialIntersection = refPtLocal;
                    }

                    if (hitMesh && intersectFaceId != 0xffffffff && hitTubeMesh &&
                        intersectFaceId < (uint32_t)hitTubeMesh->nbFaces &&
                        intersectFaceId * 3 + 2 < hitTubeMesh->faceNormals.size()) {
                        m_initialIntersectionNormal = glm::vec3(
                            hitTubeMesh->faceNormals[intersectFaceId * 3],
                            hitTubeMesh->faceNormals[intersectFaceId * 3 + 1],
                            hitTubeMesh->faceNormals[intersectFaceId * 3 + 2]
                        );
                    } else {
                        m_initialIntersectionNormal = camFacingNormal;
                    }
                    hitMesh = true;
                    mesh = hitTubeMesh;
                }
            }

            // Dynamic tool activation:
            // If Ctrl is held and click starts on empty space (no mesh intersection), trigger Masking Lasso
            if ((mod & KMOD_CTRL) && !hitMesh) {
                m_isLassoActive = true;
                m_isMaskLasso = true;
                m_lassoPoints.clear();
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                m_lassoAlt = (mod & KMOD_ALT) != 0;
                return;
            }

            if (hitMesh) {
                BrushType activeBrush = m_currentBrush;
                bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
                bool isCtrl = (mod & KMOD_CTRL) != 0;
                bool isAlt = (mod & KMOD_ALT) != 0;
                bool isShift = (mod & KMOD_SHIFT) != 0;
                if (isShift) {
                    activeBrush = BRUSH_SMOOTH;
                } else if (isMayaNav ? (isCtrl && !isAlt) : isCtrl) {
                    if (m_currentBrush != BRUSH_POLYGROUP) {
                        activeBrush = BRUSH_MASK;
                    }
                }
                bool isDeformBrush = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_POLYGROUP);
                if (mesh && mesh->hasLayers() && isDeformBrush && !mesh->isLayerRecording()) {
                    return;
                }

                if (mesh && mesh->isPrimitive) {
                    // Procedural primitive: do NOT auto-bake on brush click!
                    // Keeps handles and procedural editing active until user explicitly clicks Bake.
                    return;
                }

                if (!m_isTubeMoveStroke) {
                    bool strokeAffectsPositions = (m_currentBrush != BRUSH_PAINT && m_currentBrush != BRUSH_MASK && m_currentBrush != BRUSH_MASK_GRADIENT_BLUR);
                    bool strokeAffectsColors = (m_currentBrush == BRUSH_PAINT && getCurrentSettings().writeAlbedo);
                    bool strokeAffectsMaterials = (m_currentBrush == BRUSH_PAINT && (getCurrentSettings().writeRoughness || getCurrentSettings().writeMetalness)) ||
                                                   (m_currentBrush == BRUSH_MASK || m_currentBrush == BRUSH_MASK_GRADIENT_BLUR);
                    g_undoManager.beginSculptStroke(scene, mesh->m_id, {}, strokeAffectsPositions, strokeAffectsColors, strokeAffectsMaterials, "Sculpt Stroke");
                }
                m_isSculpting = true;
                m_currentIntersectionValid = true;
                m_firstStrokeFrame = true;
                m_pendingBVHUpdate = false;
                m_grabbedNumIFaces = 0;
                m_grabUndoRecorded = false;
                m_grabbedMinV = 0xFFFFFFFF;
                m_grabbedMaxV = 0;
                m_hasAlphaOrigin = false;
                m_hasRakeDir = false;
                if (!m_isTubeMoveStroke) {
                    m_initialIntersection = localRayOrigin + minT * localRayDir;
                    m_initialIntersectionNormal = glm::vec3(
                        mesh->faceNormals[intersectFaceId * 3],
                        mesh->faceNormals[intersectFaceId * 3 + 1],
                        mesh->faceNormals[intersectFaceId * 3 + 2]
                    );
                }
                m_currentIntersection = m_initialIntersection;
                m_currentIntersectionNormal = m_initialIntersectionNormal;
                if (intersectFaceId != 0xffffffff && !mesh->faceGroups.empty() && intersectFaceId < (uint32_t)mesh->nbFaces) {
                    m_strokeTargetPolyGroup = mesh->faceGroups[intersectFaceId];
                }

                std::vector<glm::vec3> symScales = getActiveSymmetryScales();
                m_initialSymIntersections.resize(symScales.size());
                for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                    m_initialSymIntersections[sIdx] = reflectPointSymmetry(m_initialIntersection, symScales[sIdx], mesh, m_symmetryMode);
                }

                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;

                // Ensure vertProxy is sized to mesh vertices without redundant copy on every click
                if (mesh && !m_isTubeMoveStroke) {
                    if (mesh->vertProxy.size() != mesh->verts.size()) {
                        mesh->vertProxy = mesh->verts;
                    }
                }

                // Check stylus/tablet pressure
                float currentPressure = 1.0f;
                if (g_tablet.isAvailable() && g_tablet.isPenActive()) {
                    currentPressure = g_tablet.getPressure();
                } else {
                    if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                        m_usingStylus = false;
                        m_stylusPressure = 1.0f;
                    }
                    currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
                }

                // Execute first stroke frame immediately
                executeStroke(scene, mesh, camera, (float)mouseX, (float)mouseY, currentPressure);
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONUP) {
        if (m_cameraController.isDragging() || event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            if (m_currentBrush == BRUSH_QUAD_DRAW && m_quadDrawTool) {
                SDL_Keymod mod = SDL_GetModState();
                m_quadDrawTool->handleMouseMove(scene, camera, (float)event.button.x, (float)event.button.y, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0);
            }
            return;
        }

        if ((m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) && event.button.button == SDL_BUTTON_LEFT) {
            float mouseX = (float)event.button.x;
            float mouseY = (float)event.button.y;

            auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

            if (m_draggedSegment) {
                glm::vec3 posA = getAnchorWorldPos(m_draggedSegment->vertA);
                glm::vec3 posB = getAnchorWorldPos(m_draggedSegment->vertB);
                if (glm::distance(posA, posB) < 1e-4f) {
                    std::string segName = m_draggedSegment->name.empty() ? "Segment" : m_draggedSegment->name;
                    MeasurementSegment* toDel = m_draggedSegment;
                    g_undoManager.pushMeasurementChange(scene, "Delete " + segName, [&]() {
                        segments.erase(std::remove_if(segments.begin(), segments.end(),
                            [&](const MeasurementSegment& seg) { return &seg == toDel; }),
                            segments.end());
                    });
                }
                m_draggedSegment = nullptr;
                m_draggedVertexKey = "";

                if (m_currentBrush == BRUSH_MEASURE) {
                    bool hasReference = false;
                    for (const auto& seg : segments) {
                        if (seg.isReference) { hasReference = true; break; }
                    }
                    if (!hasReference && !segments.empty()) {
                        segments[0].isReference = true;
                    }
                }
                return;
            }

            if (m_hasPending) {
                glm::vec3 posA = getAnchorWorldPos(m_pendingAnchorA);
                glm::vec3 posB = getAnchorWorldPos(m_pendingAnchorB);
                if (glm::distance(posA, posB) > 1e-4f) {
                    MeasurementSegment newSeg;
                    newSeg.vertA = m_pendingAnchorA;
                    newSeg.vertB = m_pendingAnchorB;
                    newSeg.isReference = false;

                    if (m_currentBrush == BRUSH_MEASURE) {
                        bool hasReference = false;
                        for (const auto& seg : segments) {
                            if (seg.isReference) { hasReference = true; break; }
                        }
                        newSeg.isReference = !hasReference;
                        newSeg.name = "Measure " + std::to_string(m_measureSegments.size() + 1);
                    } else {
                        newSeg.name = "Divider " + std::to_string(m_dividerSegments.size() + 1);
                        newSeg.divisions = m_dividerDivisions;
                    }

                    g_undoManager.pushMeasurementChange(scene, "Create " + newSeg.name, [&]() {
                        segments.push_back(newSeg);
                    });
                }
                m_hasPending = false;
                return;
            }

            return;
        }

        if (m_maskExtractTool && m_maskExtractTool->isPreviewActive()) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (m_maskExtractTool->handleMouseUp(scene)) {
                    return;
                }
            }
        }

        if (m_currentBrush == BRUSH_EXTRACT) {
            if (event.button.button == SDL_BUTTON_LEFT && m_maskExtractTool) {
                m_maskExtractTool->handleMouseUp(scene);
            }
            return;
        }

        if (m_primitiveTool && m_primitiveTool->isInteracting()) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_primitiveTool->handleMouseUp(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_TRANSFORM) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            return;
        }

        if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_armatureTool->end(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_POSE) {
            if (event.button.button == SDL_BUTTON_LEFT && m_poseTool) {
                m_poseTool->handleMouseUp(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_TUBE && !m_isTubeMoveStroke) {
            if (event.button.button == SDL_BUTTON_LEFT && m_tubeTool) {
                m_tubeTool->handleMouseUp(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_QUAD_DRAW && m_quadDrawTool) {
            m_quadDrawTool->handleMouseUp(scene, camera, event.button.button);
            return;
        }

        if (m_currentBrush == BRUSH_COMPONENT_SELECT && m_componentSelectTool) {
            m_componentSelectTool->handleMouseUp(scene, camera, event.button.button);
            return;
        }

        if (m_currentBrush == BRUSH_EDGE_CREASE && m_edgeCreaseTool) {
            m_edgeCreaseTool->handleMouseUp(scene, camera, event.button.button);
            return;
        }

        if (m_currentBrush == BRUSH_CURVE_FOLDS && m_curveFoldsTool) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_curveFoldsTool->handleMouseUp(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh && event.button.button == SDL_BUTTON_LEFT) {
            if (m_gradIsDrawing) {
                float dist = glm::distance(m_gradPointA, m_gradPointB);
                if (dist < 5.0f) {
                    m_gradPointA = glm::vec2(m_mouseDownX - 75.0f, (float)m_mouseDownY);
                    m_gradPointB = glm::vec2(m_mouseDownX + 75.0f, (float)m_mouseDownY);

                    glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
                    float width = (float)camera.getWidth();
                    float height = (float)camera.getHeight();
                    float localToScreen[16] = {0.0f};

                    localToScreen[0] = (mvp[0][0] + mvp[0][3]) * 0.5f * width;
                    localToScreen[4] = (mvp[1][0] + mvp[1][3]) * 0.5f * width;
                    localToScreen[8] = (mvp[2][0] + mvp[2][3]) * 0.5f * width;
                    localToScreen[12] = (mvp[3][0] + mvp[3][3]) * 0.5f * width;

                    localToScreen[1] = (mvp[0][1] + mvp[0][3]) * 0.5f * height;
                    localToScreen[5] = (mvp[1][1] + mvp[1][3]) * 0.5f * height;
                    localToScreen[9] = (mvp[2][1] + mvp[2][3]) * 0.5f * height;
                    localToScreen[13] = (mvp[3][1] + mvp[3][3]) * 0.5f * height;

                    localToScreen[3] = mvp[0][3];
                    localToScreen[7] = mvp[1][3];
                    localToScreen[11] = mvp[2][3];
                    localToScreen[15] = mvp[3][3];

                    float ptPlaneX = 0.0f, ptPlaneY = 0.0f, ptPlaneZ = 0.0f;
                    float nPlaneX = m_symX ? 1.0f : 0.0f;
                    float nPlaneY = m_symY ? 1.0f : 0.0f;
                    float nPlaneZ = m_symZ ? 1.0f : 0.0f;

                    ::applyGradientMask(
                        mesh->verts.data(),
                        mesh->materials.data(),
                        m_gradActiveVerts.data(),
                        (int)m_gradActiveVerts.size(),
                        m_origMasks.data(),
                        m_blurredMasks.data(),
                        localToScreen,
                        height,
                        m_gradPointA.x, m_gradPointA.y,
                        m_gradPointB.x, m_gradPointB.y,
                        m_useSym,
                        ptPlaneX, ptPlaneY, ptPlaneZ,
                        nPlaneX, nPlaneY, nPlaneZ,
                        getCurrentSettings().blurMaskedOnly,
                        mesh->nbVerts
                    );

                    mesh->isVertexDirty = true;
                    mesh->dirtyVertMin = 0;
                    mesh->dirtyVertMax = mesh->nbVerts - 1;
                    mesh->isDirty = true;
                }
                m_gradIsDrawing = false;
                m_gradActivePoint = '\0';
                return;
            } else if (m_gradActivePoint != '\0') {
                m_gradActivePoint = '\0';
                return;
            }
        }

        if (m_isLassoActive) {
            m_isLassoActive = false;

            float maxDragDistSq = 0.0f;
            glm::vec2 startPt((float)m_mouseDownX, (float)m_mouseDownY);
            for (const auto& pt : m_lassoPoints) {
                glm::vec2 diff = pt - startPt;
                float d2 = glm::dot(diff, diff);
                if (d2 > maxDragDistSq) maxDragDistSq = d2;
            }
            float maxDragDist = std::sqrt(maxDragDistSq);

            const float LASSO_DRAG_THRESHOLD = 8.0f; // Threshold in pixels to distinguish click vs lasso drag
            bool isRealDrag = (m_lassoPoints.size() >= 3) && (maxDragDist >= LASSO_DRAG_THRESHOLD);

            if (isRealDrag && mesh) {
                auto tLassoStart = std::chrono::high_resolution_clock::now();
                sculpt_log("[LassoRelease] Processing lasso drag (Points: %zu, MeshVerts: %d, IsMask: %d, Alt: %d, Sym: %d)...\n",
                          m_lassoPoints.size(), mesh->nbVerts, m_isMaskLasso ? 1 : 0, m_lassoAlt ? 1 : 0, m_useSym ? 1 : 0);

                std::vector<uint32_t> selectedVertices = getVerticesInLasso(mesh, camera);

                if (m_isMaskLasso) {
                    if (!selectedVertices.empty()) {
                        auto tHistStart = std::chrono::high_resolution_clock::now();
                        g_undoManager.beginSculptStroke(scene, mesh->getID(), selectedVertices, false, false, true, "Mask Lasso");
                        auto tHistEnd = std::chrono::high_resolution_clock::now();
                        double histMs = std::chrono::duration<double, std::milli>(tHistEnd - tHistStart).count();

                        float maskVal = m_lassoAlt ? 1.0f : 0.0f;
                        float* materials = mesh->materials.data();
#pragma omp parallel for schedule(static) if(selectedVertices.size() > 1000)
                        for (size_t i = 0; i < selectedVertices.size(); ++i) {
                            uint32_t vid = selectedVertices[i];
                            materials[vid * 3 + 2] = maskVal;
                        }
                        mesh->isMaterialDirty = true;
                        mesh->dirtyVertMin = 0;
                        mesh->dirtyVertMax = mesh->nbVerts - 1;
                        mesh->isDirty = true;

                        g_undoManager.endSculptStroke(scene);

                        auto tLassoEnd = std::chrono::high_resolution_clock::now();
                        double totalMs = std::chrono::duration<double, std::milli>(tLassoEnd - tLassoStart).count();
                        sculpt_log("[LassoRelease] Mask Applied: %zu / %d verts (DeltaRecord: %.2f ms, Total: %.2f ms)\n",
                                  selectedVertices.size(), mesh->nbVerts, histMs, totalMs);
                    } else {
                        sculpt_log("[LassoRelease] Empty lasso selection -> clearing mask\n");
                        clearMask(mesh, &scene);
                        auto tLassoEnd = std::chrono::high_resolution_clock::now();
                        double totalMs = std::chrono::duration<double, std::milli>(tLassoEnd - tLassoStart).count();
                        sculpt_log("[LassoRelease] Clear Mask completed in %.2f ms\n", totalMs);
                    }
                } else {
                    scene.pushHistoryState();
                    if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                        mesh->faceVisible.assign(mesh->nbFaces, 1);
                    }

                    if (!selectedVertices.empty()) {
                        std::vector<uint8_t> isVertSel(mesh->nbVerts, 0);
                        for (uint32_t vid : selectedVertices) {
                            if (vid < (uint32_t)mesh->nbVerts) isVertSel[vid] = 1;
                        }

                        bool hideUnselected = !m_lassoAlt;
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            uint32_t v0 = mesh->faces[f * 4];
                            uint32_t v1 = mesh->faces[f * 4 + 1];
                            uint32_t v2 = mesh->faces[f * 4 + 2];
                            uint32_t v3 = mesh->faces[f * 4 + 3];

                            bool fSel = isVertSel[v0] && isVertSel[v1] && isVertSel[v2] &&
                                        (v3 == 0xffffffff || isVertSel[v3]);

                            if (hideUnselected) {
                                // Isolate lasso selection: only show faces fully selected
                                mesh->faceVisible[f] = fSel ? 1 : 0;
                            } else {
                                // Hide lasso selection: hide faces fully selected
                                if (fSel) mesh->faceVisible[f] = 0;
                            }
                        }

                        // Synchronize vertVisible from faceVisible
                        std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 0);
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            if (mesh->faceVisible[f]) {
                                uint32_t v0 = mesh->faces[f * 4];
                                uint32_t v1 = mesh->faces[f * 4 + 1];
                                uint32_t v2 = mesh->faces[f * 4 + 2];
                                uint32_t v3 = mesh->faces[f * 4 + 3];

                                if (v0 < mesh->vertVisible.size()) mesh->vertVisible[v0] = 1;
                                if (v1 < mesh->vertVisible.size()) mesh->vertVisible[v1] = 1;
                                if (v2 < mesh->vertVisible.size()) mesh->vertVisible[v2] = 1;
                                if (v3 != 0xffffffff && v3 < mesh->vertVisible.size()) mesh->vertVisible[v3] = 1;
                            }
                        }
                    } else {
                        // Ctrl + Shift + Click Drag on empty space -> clear hiding (show all)
                        std::fill(mesh->faceVisible.begin(), mesh->faceVisible.end(), 1);
                        std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 1);
                    }
                    mesh->isDirty = true;
                    mesh->isFaceGroupDirty = true;
                }
            } else {
                // Click action (or micro-drag gesture under threshold)
                Ray ray = camera.getRay((float)event.button.x, (float)event.button.y);
                bool hitMesh = false;
                uint32_t closestVert = 0xffffffff;
                uint32_t intersectFaceId = 0xffffffff;
                float minT = std::numeric_limits<float>::infinity();
                float bestMask = 1.0f;

                if (mesh) {
                    glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

                    mesh->ensureBVHBuilt();
                    RayHit hit;
                    if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
                        minT = hit.t;
                        intersectFaceId = hit.faceId;
                        hitMesh = true;
                    }

                    if (intersectFaceId != 0xffffffff) {
                        hitMesh = true;
                        glm::vec3 inter = localRayOrigin + minT * localRayDir;
                        float bestDist2 = std::numeric_limits<float>::infinity();

                        uint32_t faceVerts[4] = {
                            mesh->faces[intersectFaceId * 4],
                            mesh->faces[intersectFaceId * 4 + 1],
                            mesh->faces[intersectFaceId * 4 + 2],
                            mesh->faces[intersectFaceId * 4 + 3]
                        };

                        for (int k = 0; k < 4; ++k) {
                            uint32_t vid = faceVerts[k];
                            if (vid == 0xffffffff) break;
                            glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                            float dist2 = glm::dot(v - inter, v - inter);
                            if (dist2 < bestDist2) {
                                bestDist2 = dist2;
                                closestVert = vid;
                                bestMask = mesh->materials[vid * 3 + 2];
                            }
                        }
                    }
                }

                if (m_isMaskLasso) {
                    if (!hitMesh && mesh) {
                        std::cout << "[MaskClick] Canvas Ctrl+Click detected (maxDragDist=" << maxDragDist << "px). Inverting mask..." << std::endl;
                        scene.pushHistoryState();
                        invertMask(mesh);
                    } else if (hitMesh && mesh) {
                        SDL_Keymod mod = SDL_GetModState();
                        bool ctrlKey = (mod & KMOD_CTRL) != 0;
                        bool altKey = (mod & KMOD_ALT) != 0;

                        scene.pushHistoryState();
                        if (ctrlKey && altKey) {
                            sharpenMask(mesh);
                        } else {
                            blurMask(mesh);
                        }
                    }
                } else {
                    if (!hitMesh && mesh) {
                        std::cout << "[VisibilityClick] Canvas Ctrl+Shift+Click detected (maxDragDist=" << maxDragDist << "px). Inverting visibility..." << std::endl;
                        scene.pushHistoryState();
                        if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                            mesh->faceVisible.assign(mesh->nbFaces, 1);
                        }

                        // Invert face visibility (0 -> 1, 1 -> 0)
                        for (size_t f = 0; f < mesh->faceVisible.size(); ++f) {
                            mesh->faceVisible[f] = mesh->faceVisible[f] ? 0 : 1;
                        }

                        // Synchronize vertVisible from faceVisible:
                        // Mark vertVisible = 1 for all vertices that belong to ANY visible face
                        std::vector<uint8_t> newVertVisible(mesh->nbVerts, 0);
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            if (mesh->faceVisible[f]) {
                                uint32_t v0 = mesh->faces[f * 4];
                                uint32_t v1 = mesh->faces[f * 4 + 1];
                                uint32_t v2 = mesh->faces[f * 4 + 2];
                                uint32_t v3 = mesh->faces[f * 4 + 3];

                                if (v0 < newVertVisible.size()) newVertVisible[v0] = 1;
                                if (v1 < newVertVisible.size()) newVertVisible[v1] = 1;
                                if (v2 < newVertVisible.size()) newVertVisible[v2] = 1;
                                if (v3 != 0xffffffff && v3 < newVertVisible.size()) newVertVisible[v3] = 1;
                            }
                        }

                        mesh->vertVisible = std::move(newVertVisible);
                        mesh->isDirty = true;
                        mesh->isFaceGroupDirty = true;
                    } else if (hitMesh && mesh && intersectFaceId != 0xffffffff) {
                        if (mesh->faceGroups.size() != (size_t)mesh->nbFaces) {
                            mesh->initFaceGroups();
                        }
                        uint32_t clickedGroupId = m_polyGroupTool.getGroupAtFace(mesh, intersectFaceId);
                        SDL_Keymod mod = SDL_GetModState();
                        bool altPressed = m_lassoAlt || ((mod & KMOD_ALT) != 0);

                        scene.pushHistoryState();

                        if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                            mesh->faceVisible.assign(mesh->nbFaces, 1);
                        }

                        if (!altPressed) {
                            // Check if clickedGroupId is currently isolated (all its faces visible, and all other group faces hidden)
                            bool isAlreadyIsolated = true;
                            bool hasOtherGroups = false;
                            for (int f = 0; f < mesh->nbFaces; ++f) {
                                uint32_t gid = (mesh->faceGroups.size() > (size_t)f) ? mesh->faceGroups[f] : 0;
                                if (gid == clickedGroupId) {
                                    if (!mesh->faceVisible[f]) {
                                        isAlreadyIsolated = false;
                                        break;
                                    }
                                } else {
                                    hasOtherGroups = true;
                                    if (mesh->faceVisible[f]) {
                                        isAlreadyIsolated = false;
                                        break;
                                    }
                                }
                            }

                            if (isAlreadyIsolated && hasOtherGroups) {
                                // Toggle isolation OFF: show ALL polygroups
                                std::cout << "[VisibilityClick] PolyGroup " << clickedGroupId << " is already isolated. Showing all polygroups..." << std::endl;
                                std::fill(mesh->faceVisible.begin(), mesh->faceVisible.end(), 1);
                            } else {
                                // Isolate clicked polygroup (show ONLY clickedGroupId)
                                std::cout << "[VisibilityClick] Ctrl+Shift+Click on PolyGroup " << clickedGroupId << ". Isolating polygroup..." << std::endl;
                                for (int f = 0; f < mesh->nbFaces; ++f) {
                                    uint32_t gid = (mesh->faceGroups.size() > (size_t)f) ? mesh->faceGroups[f] : 0;
                                    mesh->faceVisible[f] = (gid == clickedGroupId) ? 1 : 0;
                                }
                            }
                        } else {
                            // Ctrl + Shift + Alt + Click: hide clicked polygroup (hide clickedGroupId, keeping other currently visible faces)
                            std::cout << "[VisibilityClick] Ctrl+Shift+Alt+Click on PolyGroup " << clickedGroupId << ". Hiding polygroup..." << std::endl;
                            for (int f = 0; f < mesh->nbFaces; ++f) {
                                uint32_t gid = (mesh->faceGroups.size() > (size_t)f) ? mesh->faceGroups[f] : 0;
                                if (gid == clickedGroupId) {
                                    mesh->faceVisible[f] = 0;
                                }
                            }
                        }

                        // Synchronize vertVisible from faceVisible:
                        // Mark vertVisible = 1 for all vertices that belong to ANY visible face
                        std::vector<uint8_t> newVertVisible(mesh->nbVerts, 0);
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            if (mesh->faceVisible[f]) {
                                uint32_t v0 = mesh->faces[f * 4];
                                uint32_t v1 = mesh->faces[f * 4 + 1];
                                uint32_t v2 = mesh->faces[f * 4 + 2];
                                uint32_t v3 = mesh->faces[f * 4 + 3];

                                if (v0 < newVertVisible.size()) newVertVisible[v0] = 1;
                                if (v1 < newVertVisible.size()) newVertVisible[v1] = 1;
                                if (v2 < newVertVisible.size()) newVertVisible[v2] = 1;
                                if (v3 != 0xffffffff && v3 < newVertVisible.size()) newVertVisible[v3] = 1;
                            }
                        }

                        mesh->vertVisible = std::move(newVertVisible);
                        mesh->isDirty = true;
                        mesh->isFaceGroupDirty = true;
                    }
                }
            }
            m_lassoPoints.clear();
            m_isMaskLasso = false;
            return;
        }

        if (m_isClipCurveActive) {
            m_isClipCurveActive = false;

            sculpt_log("[SculptManager] ClipCurve mouse up released. Points: %zu, Mesh: %p (verts: %d)\n",
                       m_clipCurvePoints.size(), (void*)mesh, mesh ? mesh->nbVerts : 0);

            if (m_clipCurvePoints.size() >= 2 && mesh) {
                HistoryState beforeState = scene.saveCurrentState(false);
                std::vector<glm::vec3> symScales = getActiveSymmetryScales();
                bool modified = ClipCurveTool::execute(
                    mesh,
                    scene.getCamera(),
                    m_clipCurvePoints,
                    m_clipCurveAlt,
                    m_useSym,
                    m_symmetryMode,
                    symScales
                );
                sculpt_log("[SculptManager] ClipCurveTool::execute result = %s\n", modified ? "TRUE (modified)" : "FALSE (unmodified)");
                if (modified) {
                    HistoryState afterState = scene.saveCurrentState(false);
                    g_undoManager.pushTopologyChange(scene, "Clip Curve", std::move(beforeState), std::move(afterState));
                    scene.setModified(true);
                }
            } else {
                sculpt_log("[SculptManager] ClipCurve skipped execution (points < 2 or mesh is null)\n");
            }
            m_clipCurvePoints.clear();
            return;
        }

        if (m_isTrimLassoActive) {
            m_isTrimLassoActive = false;

            sculpt_log("[TrimLasso] MouseUp: lasso completed. Points: %zu, Mesh: %p (verts: %d), alt=%d, activeVp=%d\n",
                       m_trimLassoPoints.size(), (void*)mesh, mesh ? mesh->nbVerts : 0, m_trimLassoAlt ? 1 : 0, activeVp);

            if (m_trimLassoPoints.size() >= 3 && mesh) {
                float lassoMinX = 1e9f, lassoMinY = 1e9f, lassoMaxX = -1e9f, lassoMaxY = -1e9f;
                for (const auto& pt : m_trimLassoPoints) {
                    lassoMinX = std::min(lassoMinX, pt.x);
                    lassoMinY = std::min(lassoMinY, pt.y);
                    lassoMaxX = std::max(lassoMaxX, pt.x);
                    lassoMaxY = std::max(lassoMaxY, pt.y);
                }
                sculpt_log("[TrimLasso] Lasso Screen AABB: min=(%.1f, %.1f), max=(%.1f, %.1f), Start=(%.1f, %.1f), End=(%.1f, %.1f)\n",
                           lassoMinX, lassoMinY, lassoMaxX, lassoMaxY,
                           m_trimLassoPoints.front().x, m_trimLassoPoints.front().y,
                           m_trimLassoPoints.back().x, m_trimLassoPoints.back().y);

                HistoryState beforeState = scene.saveCurrentState(true);
                TrimConfig config;
                bool modified = TrimTool::execute(
                    mesh,
                    camera,
                    m_trimLassoPoints,
                    m_trimLassoAlt,
                    getUseSym(),
                    m_symmetryMode,
                    getActiveSymmetryScales(),
                    config
                );

                if (modified) {
                    HistoryState afterState = scene.saveCurrentState(true);
                    g_undoManager.pushTopologyChange(scene, "Trim", std::move(beforeState), std::move(afterState));
                    scene.setModified(true);
                    sculpt_log("[TrimLasso] TrimTool execution modified the mesh successfully.\n");
                } else {
                    sculpt_log("[TrimLasso] TrimTool execution returned false (unmodified).\n");
                }
            } else {
                sculpt_log("[TrimLasso] Lasso too small (points=%zu) or no mesh. Skipping execution.\n",
                           m_trimLassoPoints.size());
            }

            m_trimLassoPoints.clear();
            return;
        }

        if (m_isSplitLassoActive) {
            m_isSplitLassoActive = false;
            int splitMode = m_brushSettings[BRUSH_SPLIT].splitMode;

            sculpt_log("[SplitTool] MouseUp: stroke completed. Points: %zu, Mesh: %p (verts: %d), alt=%d, mode=%d\n",
                       m_splitLassoPoints.size(), (void*)mesh, mesh ? mesh->nbVerts : 0, m_splitLassoAlt ? 1 : 0, splitMode);

            if (mesh) {
                if (splitMode == 1 && m_splitLassoPoints.size() >= 2) {
                    scene.pushHistoryState();
                    std::vector<glm::vec3> symScales = getActiveSymmetryScales();
                    bool modified = sculpt::SplitTool::sliceByLine(
                        scene,
                        mesh,
                        camera,
                        m_splitLassoPoints.front(),
                        m_splitLassoPoints.back(),
                        getUseSym(),
                        m_symmetryMode,
                        symScales
                    );
                    if (modified) {
                        scene.setModified(true);
                        sculpt_log("[SplitTool] sliceByLine modified the scene successfully.\n");
                    } else {
                        sculpt_log("[SplitTool] sliceByLine returned false (unmodified).\n");
                    }
                } else if (splitMode == 0 && m_splitLassoPoints.size() >= 3) {
                    scene.pushHistoryState();
                    std::vector<glm::vec3> symScales = getActiveSymmetryScales();
                    bool modified = sculpt::SplitTool::splitByLasso(
                        scene,
                        mesh,
                        camera,
                        m_splitLassoPoints,
                        m_splitLassoAlt,
                        getUseSym(),
                        m_symmetryMode,
                        symScales
                    );
                    if (modified) {
                        scene.setModified(true);
                        sculpt_log("[SplitTool] splitByLasso modified the scene successfully.\n");
                    } else {
                        sculpt_log("[SplitTool] splitByLasso returned false (unmodified).\n");
                    }
                } else {
                    sculpt_log("[SplitTool] Stroke too small (points=%zu). Skipping execution.\n",
                               m_splitLassoPoints.size());
                }
            }

            m_splitLassoPoints.clear();
            return;
        }

        if (m_isSculpting) {
            m_isSculpting = false;
            m_relaxEngine.endStroke();
            m_currentIntersectionValid = false;
            m_hasAnyValidIntersection = false;
            m_snakeActiveVerts.clear();
            m_snakeActiveVertsSyms.clear();
            m_snakePrevTipPosSyms.clear();
            m_hasRakeDir = false;

            auto tRelStart = std::chrono::high_resolution_clock::now();
            double grabProxyMs = 0.0;
            if (!m_grabbedAffectedVerts.empty()) {
                auto tGp0 = std::chrono::high_resolution_clock::now();
                if (mesh && mesh->vertProxy.size() == mesh->verts.size()) {
                    const size_t numAff = m_grabbedAffectedVerts.size();
                    const uint32_t* pAff = m_grabbedAffectedVerts.data();
                    float* dst = mesh->vertProxy.data();
                    const float* src = mesh->verts.data();
                    const size_t maxVertIdx = mesh->verts.size() / 3;

                    #pragma omp parallel for schedule(static) if(numAff > 4000)
                    for (int64_t i = 0; i < (int64_t)numAff; ++i) {
                        uint32_t v = pAff[i];
                        if (v < maxVertIdx) {
                            dst[v * 3 + 0] = src[v * 3 + 0];
                            dst[v * 3 + 1] = src[v * 3 + 1];
                            dst[v * 3 + 2] = src[v * 3 + 2];
                        }
                    }
                }
                m_grabbedAffectedVerts.clear();
                grabProxyMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tGp0).count();
            }
            m_grabbedVertices.clear();
            m_grabbedVerticesSyms.clear();
            m_grabUndoRecorded = false;
            m_grabbedMinV = 0xFFFFFFFF;
            m_grabbedMaxV = 0;

            if (m_isTubeMoveStroke) {
                m_isTubeMoveStroke = false;
                m_tubeMoveHitMesh = nullptr;
                m_tubeMoveHitPointIdx = -1;
                m_tubeMoveTargets.clear();
                m_tubeMoveOrigPoints.clear();
                if (m_tubeTool) {
                    m_tubeTool->syncToActiveMesh();
                }
                scene.pushHistoryState();
                return;
            }
            int dragDistX = std::abs(event.button.x - m_mouseDownX);
            int dragDistY = std::abs(event.button.y - m_mouseDownY);
            bool wasClick = (dragDistX <= 12 && dragDistY <= 12);

            bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
            bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;
            bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
            bool isMaskAction = (m_currentBrush == BRUSH_MASK) || (isMayaNav ? (ctrlPressed && !altPressed) : ctrlPressed);

            if (wasClick && isMaskAction && mesh) {
                sculpt_log("[MaskClick] Ctrl+Click mask action detected. Canceling active click stroke...\n");
                g_undoManager.cancelSculptStroke();

                // Mesh pointer stays valid when using delta UndoManager, but re-get just in case
                mesh = scene.getSelected();
                if (!mesh) {
                    sculpt_log("[MaskClick] Error: Selected mesh is null after cancel!\n");
                    return;
                }

                // Compute click action on the mesh at click coordinate
                Ray ray = camera.getRay((float)event.button.x, (float)event.button.y);
                bool hitMesh = false;
                uint32_t closestVert = 0xffffffff;
                uint32_t intersectFaceId = 0xffffffff;
                float minT = std::numeric_limits<float>::infinity();
                float bestMask = 1.0f;

                glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

                mesh->ensureBVHBuilt();
                RayHit hit;
                if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
                    minT = hit.t;
                    intersectFaceId = hit.faceId;
                }

                if (intersectFaceId != 0xffffffff) {
                    hitMesh = true;
                    glm::vec3 inter = localRayOrigin + minT * localRayDir;
                    float bestDist2 = std::numeric_limits<float>::infinity();

                    uint32_t faceVerts[4] = {
                        mesh->faces[intersectFaceId * 4],
                        mesh->faces[intersectFaceId * 4 + 1],
                        mesh->faces[intersectFaceId * 4 + 2],
                        mesh->faces[intersectFaceId * 4 + 3]
                    };

                    for (int k = 0; k < 4; ++k) {
                        uint32_t vid = faceVerts[k];
                        if (vid == 0xffffffff) break;
                        glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                        float dist2 = glm::dot(v - inter, v - inter);
                        if (dist2 < bestDist2) {
                            bestDist2 = dist2;
                            closestVert = vid;
                            bestMask = mesh->materials[vid * 3 + 2];
                        }
                    }
                }

                if (hitMesh) {
                    SDL_Keymod mod = SDL_GetModState();
                    bool ctrlKey = (mod & KMOD_CTRL) != 0;
                    bool altKey = (mod & KMOD_ALT) != 0;

                    sculpt_log("[MaskClick] Ray hit mesh. closestVert=%u bestMask=%.2f\n", closestVert, bestMask);
                    scene.pushHistoryState();

                    mesh = scene.getSelected();
                    if (!mesh) return;

                    if (ctrlKey && altKey) {
                        sharpenMask(mesh);
                    } else {
                        blurMask(mesh);
                    }
                } else {
                    sculpt_log("[MaskClick] Ray missed mesh. Inverting mask...\n");
                    scene.pushHistoryState();
                    mesh = scene.getSelected();
                    if (mesh) {
                        invertMask(mesh);
                    }
                }
            } else {
                auto tEndUndo0 = std::chrono::high_resolution_clock::now();
                g_undoManager.endSculptStroke(scene);
                double endUndoMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tEndUndo0).count();
                double bvhMs = 0.0;
                double bvhRebuildMs = 0.0;
                double layerBakeMs = 0.0;
                if (mesh && m_pendingBVHUpdate) {
                    auto tBvhStart = std::chrono::high_resolution_clock::now();
                    if (m_grabbedNumIFaces > 0) {
                        mesh->bvh.refit(*mesh, m_iFacesCache.data(), m_grabbedNumIFaces, true);
                        bvhMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tBvhStart).count();
                        const BVHRefitStats& rs = mesh->bvh.getLastRefitStats();
                        sculpt_log("[Stroke Release] Deferred Incremental BVH Refit completed in %.2f ms (%d faces | Dedup %.2f, AABB %.2f, ClimbSort %.2f, Inner %.2f | L=%u I=%u Pre=%d)\n",
                                   bvhMs, (int)m_grabbedNumIFaces, rs.leafDedupMs, rs.leafAABBMs, rs.parentGatherMs, rs.innerUpdateMs, rs.dirtyLeaves, rs.dirtyInnerNodes, rs.usedPrecomputed ? 1 : 0);
                    } else {
                        mesh->bvh.refitAll(*mesh);
                        bvhMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tBvhStart).count();
                        const BVHRefitStats& rs = mesh->bvh.getLastRefitStats();
                        sculpt_log("[Stroke Release] Deferred Full BVH Refit completed in %.2f ms (all %d faces | AABB %.2f, Inner %.2f)\n",
                                   bvhMs, mesh->nbFaces, rs.leafAABBMs, rs.innerUpdateMs);
                    }
                    if (mesh->bvh.isDegraded()) {
                        auto tRb0 = std::chrono::high_resolution_clock::now();
                        mesh->bvh.build(*mesh);
                        bvhRebuildMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRb0).count();
                    }
                    m_pendingBVHUpdate = false;
                    m_grabbedNumIFaces = 0;
                }
                if (mesh && mesh->isLayerRecording()) {
                    auto tLb0 = std::chrono::high_resolution_clock::now();
                    mesh->layerStack.bake(mesh->layerStack.getBase(), mesh->verts);
                    mesh->updateAfterLayerBake();
                    layerBakeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tLb0).count();
                }
                double relTotalMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRelStart).count();
                if (relTotalMs > 5.0) {
                    sculpt_log("[Stroke Release Detail] Total: %.2f ms (GrabProxy: %.2f, EndUndo: %.2f, BVHRefit: %.2f, BVHRebuild: %.2f, LayerBake: %.2f)\n",
                               relTotalMs, grabProxyMs, endUndoMs, bvhMs, bvhRebuildMs, layerBakeMs);
                }
            }
        }
        m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
    } else if (event.type == SDL_MOUSEWHEEL) {
        if (m_currentBrush == BRUSH_COMPONENT_SELECT && m_componentSelectTool && m_componentSelectTool->isBevelActive()) {
            if (m_componentSelectTool->handleMouseWheel(scene, event.wheel.y)) {
                return;
            }
        }
        m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
    } else if (event.type == SDL_MOUSEMOTION) {
        int mouseX = event.motion.x;
        int mouseY = event.motion.y;

        m_rawMouseX = mouseX;
        m_rawMouseY = mouseY;

        int dx = mouseX - m_prevMouseX;
        int dy = mouseY - m_prevMouseY;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;

        if (m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) {
            float threshold = 15.0f;
            auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

            if (m_draggedSegment) {
                std::string otherKey = (m_draggedVertexKey == "vertA") ? "vertB" : "vertA";
                glm::vec3 refPos = getAnchorWorldPos(otherKey == "vertA" ? m_draggedSegment->vertA : m_draggedSegment->vertB);
                MeasurementAnchor anchor = pickAnchor((float)mouseX, (float)mouseY, scene, &refPos);
                if (m_draggedVertexKey == "vertA") {
                    m_draggedSegment->vertA = anchor;
                } else {
                    m_draggedSegment->vertB = anchor;
                }
                return;
            }

            if (m_hasPending) {
                glm::vec3 refPos = getAnchorWorldPos(m_pendingAnchorA);
                MeasurementAnchor anchor = pickAnchor((float)mouseX, (float)mouseY, scene, &refPos);
                m_pendingAnchorB = anchor;
                return;
            }

            // Hover scanning
            m_hoveredSegment = nullptr;
            m_hoveredVertexKey = "";

            for (auto& seg : segments) {
                glm::vec3 worldA = getAnchorWorldPos(seg.vertA);
                glm::vec3 worldB = getAnchorWorldPos(seg.vertB);
                glm::vec3 screenA = camera.project(worldA);
                glm::vec3 screenB = camera.project(worldB);

                float distA = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenA.x, screenA.y));
                float distB = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenB.x, screenB.y));

                if (distA < threshold && distA <= distB) {
                    m_hoveredSegment = &seg;
                    m_hoveredVertexKey = "vertA";
                    break;
                }
                if (distB < threshold) {
                    m_hoveredSegment = &seg;
                    m_hoveredVertexKey = "vertB";
                    break;
                }
            }

            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            }
            return;
        }

        if (m_maskExtractTool && m_maskExtractTool->isPreviewActive()) {
            if (m_maskExtractTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY)) {
                return;
            }
        }

        if (m_primitiveTool && m_primitiveTool->isInteracting()) {
            m_primitiveTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            return;
        }

        if (m_currentBrush == BRUSH_TRANSFORM) {
            if (mesh && mesh->isPrimitive && mesh->primitiveData != nullptr && m_primitiveTool && !m_cameraController.isDragging()) {
                m_primitiveTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            }
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            }
            return;
        }

        if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else {
                m_armatureTool->update(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (m_currentBrush == BRUSH_POSE) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else if (m_poseTool) {
                m_poseTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (m_currentBrush == BRUSH_TUBE && !m_isSculpting) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else if (m_tubeTool) {
                m_tubeTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (m_currentBrush == BRUSH_QUAD_DRAW && m_quadDrawTool) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else {
                SDL_Keymod mod = SDL_GetModState();
                m_quadDrawTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0, (mod & KMOD_SHIFT) != 0);
            }
            return;
        }

        if (m_currentBrush == BRUSH_EXTRACT) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else if (m_maskExtractTool) {
                m_maskExtractTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (m_currentBrush == BRUSH_COMPONENT_SELECT && m_componentSelectTool) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else {
                SDL_Keymod mod = SDL_GetModState();
                m_componentSelectTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_SHIFT) != 0);
            }
            return;
        }

        if (m_currentBrush == BRUSH_EDGE_CREASE && m_edgeCreaseTool) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else {
                SDL_Keymod mod = SDL_GetModState();
                m_edgeCreaseTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_SHIFT) != 0);
            }
            return;
        }

        if (m_currentBrush == BRUSH_CURVE_FOLDS && m_curveFoldsTool) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
            } else {
                m_curveFoldsTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (mesh && mesh->isTube && mesh->tubeData != nullptr && m_tubeTool && !m_isSculpting && !m_cameraController.isDragging()) {
            if (!m_tubeTool->isInteracting() && m_tubeTool->getActiveTubeMesh() != mesh) {
                m_tubeTool->loadFromMesh(mesh);
            }
            m_tubeTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
        }

        if (mesh && mesh->isPrimitive && mesh->primitiveData != nullptr && m_primitiveTool && !m_isSculpting && !m_cameraController.isDragging()) {
            m_primitiveTool->handleMouseMove(scene, camera, (float)mouseX, (float)mouseY);
        }

        if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh && m_gradActive && m_gradActivePoint != '\0') {
            glm::vec2 mousePos((float)mouseX, (float)mouseY);
            if (m_gradActivePoint == 'A') {
                m_gradPointA = mousePos;
            } else if (m_gradActivePoint == 'B') {
                m_gradPointB = mousePos;
            }

            glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
            float width = (float)camera.getWidth();
            float height = (float)camera.getHeight();
            float localToScreen[16] = {0.0f};

            localToScreen[0] = (mvp[0][0] + mvp[0][3]) * 0.5f * width;
            localToScreen[4] = (mvp[1][0] + mvp[1][3]) * 0.5f * width;
            localToScreen[8] = (mvp[2][0] + mvp[2][3]) * 0.5f * width;
            localToScreen[12] = (mvp[3][0] + mvp[3][3]) * 0.5f * width;

            localToScreen[1] = (mvp[0][1] + mvp[0][3]) * 0.5f * height;
            localToScreen[5] = (mvp[1][1] + mvp[1][3]) * 0.5f * height;
            localToScreen[9] = (mvp[2][1] + mvp[2][3]) * 0.5f * height;
            localToScreen[13] = (mvp[3][1] + mvp[3][3]) * 0.5f * height;

            localToScreen[3] = mvp[0][3];
            localToScreen[7] = mvp[1][3];
            localToScreen[11] = mvp[2][3];
            localToScreen[15] = mvp[3][3];

            float ptPlaneX = 0.0f, ptPlaneY = 0.0f, ptPlaneZ = 0.0f;
            float nPlaneX = m_symX ? 1.0f : 0.0f;
            float nPlaneY = m_symY ? 1.0f : 0.0f;
            float nPlaneZ = m_symZ ? 1.0f : 0.0f;

            ::applyGradientMask(
                mesh->verts.data(),
                mesh->materials.data(),
                m_gradActiveVerts.data(),
                (int)m_gradActiveVerts.size(),
                m_origMasks.data(),
                m_blurredMasks.data(),
                localToScreen,
                height,
                m_gradPointA.x, m_gradPointA.y,
                m_gradPointB.x, m_gradPointB.y,
                m_useSym,
                ptPlaneX, ptPlaneY, ptPlaneZ,
                nPlaneX, nPlaneY, nPlaneZ,
                getCurrentSettings().blurMaskedOnly,
                mesh->nbVerts
            );

            mesh->isVertexDirty = true;
            mesh->dirtyVertMin = 0;
            mesh->dirtyVertMax = mesh->nbVerts - 1;
            mesh->isDirty = true;
            return;
        }

        if (m_isLassoActive) {
            glm::vec2 newPt((float)mouseX, (float)mouseY);
            if (m_lassoPoints.empty()) {
                m_lassoPoints.push_back(newPt);
            } else {
                glm::vec2 diff = newPt - m_lassoPoints.back();
                if (glm::dot(diff, diff) >= 4.0f) { // min 2px spacing
                    m_lassoPoints.push_back(newPt);
                }
            }
            m_lassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_isClipCurveActive) {
            glm::vec2 newPt((float)mouseX, (float)mouseY);
            if (m_clipCurvePoints.empty()) {
                m_clipCurvePoints.push_back(newPt);
            } else {
                glm::vec2 diff = newPt - m_clipCurvePoints.back();
                if (glm::dot(diff, diff) >= 4.0f) {
                    m_clipCurvePoints.push_back(newPt);
                }
            }
            m_clipCurveAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_isTrimLassoActive) {
            glm::vec2 newPt((float)mouseX, (float)mouseY);
            if (m_trimLassoPoints.empty()) {
                m_trimLassoPoints.push_back(newPt);
            } else {
                glm::vec2 diff = newPt - m_trimLassoPoints.back();
                if (glm::dot(diff, diff) >= 4.0f) {
                    m_trimLassoPoints.push_back(newPt);
                }
            }
            m_trimLassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_isSplitLassoActive) {
            glm::vec2 newPt((float)mouseX, (float)mouseY);
            int splitMode = m_brushSettings[BRUSH_SPLIT].splitMode;
            if (splitMode == 1) { // Line (Slice) mode
                if (m_splitLassoPoints.size() < 2) {
                    m_splitLassoPoints.push_back(newPt);
                } else {
                    m_splitLassoPoints.back() = newPt;
                }
            } else { // Lasso mode (like Trim)
                if (m_splitLassoPoints.empty()) {
                    m_splitLassoPoints.push_back(newPt);
                } else {
                    glm::vec2 diff = newPt - m_splitLassoPoints.back();
                    if (glm::dot(diff, diff) >= 4.0f) {
                        m_splitLassoPoints.push_back(newPt);
                    }
                }
            }
            m_splitLassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_cameraController.isDragging()) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes(), true, &scene, m_renderer);
        } else if (m_isSculpting && (mesh || m_isTubeMoveStroke)) {
            Mesh* targetMesh = m_isTubeMoveStroke ? (m_tubeMoveHitMesh ? m_tubeMoveHitMesh : mesh) : mesh;
            // Check stylus/tablet pressure
            float currentPressure = 1.0f;
            if (g_tablet.isAvailable() && g_tablet.isPenActive()) {
                currentPressure = g_tablet.getPressure();
            } else {
                if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                    m_usingStylus = false;
                    m_stylusPressure = 1.0f;
                }
                currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
            }

            float strokeDx = (float)(mouseX - m_lastStrokeX);
            float strokeDy = (float)(mouseY - m_lastStrokeY);
            float dist = std::sqrt(strokeDx * strokeDx + strokeDy * strokeDy);
            
            BrushType activeBrush = m_currentBrush;
            SDL_Keymod mod = SDL_GetModState();
            bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (camera.getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
            bool isCtrl = (mod & KMOD_CTRL) != 0;
            bool isAlt = (mod & KMOD_ALT) != 0;
            bool isShift = (mod & KMOD_SHIFT) != 0;

            if (isShift) {
                activeBrush = BRUSH_SMOOTH;
            } else if (isMayaNav ? (isCtrl && !isAlt) : isCtrl) {
                if (m_currentBrush != BRUSH_POLYGROUP) {
                    activeBrush = BRUSH_MASK;
                }
            }
            if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK)) {
                m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
            }
            const auto& activeSettings = getSettings(activeBrush);
            float minSpacing = activeSettings.spacing * activeSettings.radius;

            bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC || activeBrush == BRUSH_SNAKEHOOK || m_isTubeMoveStroke);

            if (isGrabBrush || minSpacing <= 0.0f) {
                auto tMot0 = std::chrono::high_resolution_clock::now();
                executeStroke(scene, targetMesh, camera, (float)mouseX, (float)mouseY, currentPressure);
                double motMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tMot0).count();
                if (motMs > 20.0) {
                    sculpt_log("[SCULPT MOTION SUBSTEPS] '%s' (Grab/Immediate) | 1 step in SDL_MOUSEMOTION took %.2fms (dist=%.1fpx)\n",
                               getBrushNameStr(activeBrush), motMs, dist);
                }
                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;
            } else if (dist > minSpacing) {
                auto tMot0 = std::chrono::high_resolution_clock::now();
                float step = 1.0f / std::floor(dist / minSpacing);
                float stepDx = strokeDx * step;
                float stepDy = strokeDy * step;

                float curX = (float)m_lastStrokeX + stepDx;
                float curY = (float)m_lastStrokeY + stepDy;
                int subStepCount = 0;

                for (float i = step; i <= 1.01f; i += step) {
                    executeStroke(scene, targetMesh, camera, curX, curY, currentPressure);
                    curX += stepDx;
                    curY += stepDy;
                    subStepCount++;
                }
                double motMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tMot0).count();
                if (subStepCount > 1 || motMs > 20.0) {
                    sculpt_log("[SCULPT MOTION SUBSTEPS] '%s' | %d sub-steps in single SDL_MOUSEMOTION took %.2fms (dist=%.1fpx, minSpacing=%.2fpx)\n",
                               getBrushNameStr(activeBrush), subStepCount, motMs, dist, minSpacing);
                }
                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;
            }
        }
    }
}

void SculptManager::saveSymmetryToMesh(Mesh* mesh) {
    if (!mesh) return;
    mesh->symEnabled = m_useSym;
    mesh->symX       = m_symX;
    mesh->symY       = m_symY;
    mesh->symZ       = m_symZ;
    mesh->symMode    = static_cast<int>(m_symmetryMode);
}

void SculptManager::loadSymmetryFromMesh(const Mesh* mesh) {
    if (!mesh) return;
    m_useSym        = mesh->symEnabled;
    m_symX          = mesh->symX;
    m_symY          = mesh->symY;
    m_symZ          = mesh->symZ;
    m_symmetryMode  = static_cast<SymmetryMode>(mesh->symMode);
}

void SculptManager::processFrame(Scene& scene) {
    Mesh* activeMeshSelected = scene.getSelected();
    const auto& sceneMeshes = scene.getMeshes();

    // Prune deleted meshes from m_knownMeshes
    for (auto it = m_knownMeshes.begin(); it != m_knownMeshes.end(); ) {
        if (std::find(sceneMeshes.begin(), sceneMeshes.end(), *it) == sceneMeshes.end()) {
            it = m_knownMeshes.erase(it);
        } else {
            ++it;
        }
    }

    bool lastValid = (m_lastActiveMesh != nullptr && std::find(sceneMeshes.begin(), sceneMeshes.end(), m_lastActiveMesh) != sceneMeshes.end());
    if (!lastValid) {
        m_lastActiveMesh = nullptr;
    }

    if (activeMeshSelected) {
        if (m_knownMeshes.find(activeMeshSelected) == m_knownMeshes.end()) {
            // New mesh: inherit active global symmetry settings
            saveSymmetryToMesh(activeMeshSelected);
            m_knownMeshes.insert(activeMeshSelected);
        }
    }

    if (activeMeshSelected != m_lastActiveMesh) {
        if (m_lastActiveMesh != nullptr) {
            saveSymmetryToMesh(m_lastActiveMesh);
            if (m_tubeTool && m_tubeTool->getActiveTubeMesh() == m_lastActiveMesh) {
                m_tubeTool->syncToActiveMesh();
            }
        }
        m_lastActiveMesh = activeMeshSelected;
        if (m_lastActiveMesh != nullptr) {
            loadSymmetryFromMesh(m_lastActiveMesh);
            warmupMeshCaches(m_lastActiveMesh);
            if (m_tubeTool && !m_tubeTool->isInteracting()) {
                if (m_lastActiveMesh->isTube && m_lastActiveMesh->tubeData != nullptr) {
                    m_tubeTool->loadFromMesh(m_lastActiveMesh);
                } else if (m_tubeTool->getActiveTubeMesh() != nullptr) {
                    m_tubeTool->resetActiveTube();
                }
            }
        } else {
            if (m_tubeTool && !m_tubeTool->isInteracting()) {
                m_tubeTool->resetActiveTube();
            }
        }
    }

    if (m_isClipCurveActive) {
        m_clipCurveAlt = (SDL_GetModState() & KMOD_ALT) != 0;
    }
    if (m_isTrimLassoActive) {
        m_trimLassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
    }
    if (m_isSplitLassoActive) {
        m_splitLassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
    }

    bool isRefDragMode = (scene.getCamera().getRefDragEnabled() || (scene.getCameraRight() && scene.getCameraRight()->getRefDragEnabled())) && scene.areAnyReferenceImagesVisible();
    if (isRefDragMode || m_cameraController.isDragging() || m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER || m_currentBrush == BRUSH_TRANSFORM || m_currentBrush == BRUSH_POSE || m_currentBrush == BRUSH_TUBE || m_currentBrush == BRUSH_QUAD_DRAW || m_currentBrush == BRUSH_EXTRACT || m_currentBrush == BRUSH_COMPONENT_SELECT || m_currentBrush == BRUSH_EDGE_CREASE || m_currentBrush == BRUSH_CURVE_FOLDS) {
        m_cursor.hide();
    } else {
        // Resolve dynamic brush modifier swap (Shift->Smooth, Ctrl->Mask, Ctrl+Shift->Visibility)
        BrushType activeBrush = m_currentBrush;
        SDL_Keymod mod = SDL_GetModState();

        if (activeBrush == BRUSH_ARMATURE_SPHERES) {
            m_armatureTool->preUpdate(scene, scene.getCamera(), (float)m_rawMouseX, (float)m_rawMouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0);
        }
        bool isMayaNav = m_hotkeyManager ? m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush : (scene.getCamera().getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
        if (m_currentBrush != BRUSH_TUBE && m_currentBrush != BRUSH_POSE && m_currentBrush != BRUSH_TRANSFORM && m_currentBrush != BRUSH_QUAD_DRAW && m_currentBrush != BRUSH_EXTRACT && m_currentBrush != BRUSH_COMPONENT_SELECT && m_currentBrush != BRUSH_EDGE_CREASE && m_currentBrush != BRUSH_CURVE_FOLDS) {
            if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT)) {
                activeBrush = BRUSH_VISIBILITY;
            } else if (mod & KMOD_SHIFT) {
                activeBrush = BRUSH_SMOOTH;
            } else if (isMayaNav ? ((mod & KMOD_CTRL) && !(mod & KMOD_ALT)) : (mod & KMOD_CTRL)) {
                activeBrush = BRUSH_MASK;
            }
        }

        if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK || activeBrush == BRUSH_VISIBILITY)) {
            m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
        }

        const auto& activeSettings = getSettings(activeBrush);
        Mesh* activeMesh = scene.getSelected();
        if (activeMesh && m_isSculpting) {
            activeMesh->bakeScale();
        }

        glm::vec3 localPt = m_isSculpting ? m_lastValidIntersection : m_currentIntersection;
        glm::vec3 localNorm = m_isSculpting ? m_lastValidIntersectionNormal : m_currentIntersectionNormal;
        bool hasIntersection = m_isSculpting ? m_hasAnyValidIntersection : m_currentIntersectionValid;

        if (m_isSculpting && activeMesh) {
            const Camera& camera = (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getActiveViewport() == 1 && scene.getCameraRight()) 
                                   ? *scene.getCameraRight() 
                                   : scene.getCamera();
            Ray ray = camera.getRay((float)m_rawMouseX, (float)m_rawMouseY);
            glm::mat4 invMatrix = glm::inverse(activeMesh->matrix);
            glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

            activeMesh->ensureBVHBuilt();
            RayHit hit;
            if (activeMesh->bvh.raycast(*activeMesh, localRayOrigin, localRayDir, hit)) {
                localPt = hit.position;
                if (hit.faceId < (uint32_t)activeMesh->nbFaces && !activeMesh->faceNormals.empty()) {
                    localNorm = glm::vec3(
                        activeMesh->faceNormals[hit.faceId * 3],
                        activeMesh->faceNormals[hit.faceId * 3 + 1],
                        activeMesh->faceNormals[hit.faceId * 3 + 2]
                    );
                } else {
                    localNorm = hit.normal;
                }
                hasIntersection = true;
            }
        }

        glm::vec3 worldPt = localPt;
        glm::vec3 worldNorm = localNorm;
        if (activeMesh) {
            worldPt = glm::vec3(activeMesh->matrix * glm::vec4(localPt, 1.0f));
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(activeMesh->matrix)));
            worldNorm = glm::normalize(normalMatrix * localNorm);
        }

        m_cursor.update(
            m_rawMouseX, m_rawMouseY,
            scene,
            activeSettings.radius,
            m_useSym,
            m_symX,
            m_symY,
            m_symZ,
            m_isSculpting,
            activeBrush,
            hasIntersection,
            worldPt,
            worldNorm,
            activeSettings.focalShift,
            activeSettings.hardness,
            activeSettings.paintColor,
            m_symmetryMode,
            m_modalMode
        );
    }
}

struct PrecomputedLassoEdge {
    float xi, yi;
    float yj;
    float invSlope; // (xj - xi) / (yj - yi + 1e-12f)
};

static inline bool isPointInPolygonFast(float x, float y, const PrecomputedLassoEdge* edges, size_t count) {
    bool inside = false;
    for (size_t i = 0; i < count; ++i) {
        const auto& e = edges[i];
        if (((e.yi > y) != (e.yj > y)) && (x < (y - e.yi) * e.invSlope + e.xi)) {
            inside = !inside;
        }
    }
    return inside;
}

std::vector<uint32_t> SculptManager::getVerticesInLasso(Mesh* mesh, const Camera& camera) {
    if (!mesh) return {};

    auto t0 = std::chrono::high_resolution_clock::now();

    size_t numEdges = m_lassoPoints.size();
    if (numEdges < 3) return {};

    // Compute lasso AABB in screen space
    float lassoMinX = std::numeric_limits<float>::max();
    float lassoMinY = std::numeric_limits<float>::max();
    float lassoMaxX = -std::numeric_limits<float>::max();
    float lassoMaxY = -std::numeric_limits<float>::max();
    for (const auto& pt : m_lassoPoints) {
        lassoMinX = std::min(lassoMinX, pt.x);
        lassoMinY = std::min(lassoMinY, pt.y);
        lassoMaxX = std::max(lassoMaxX, pt.x);
        lassoMaxY = std::max(lassoMaxY, pt.y);
    }

    std::vector<PrecomputedLassoEdge> precomputedEdges(numEdges);
    for (size_t i = 0, j = numEdges - 1; i < numEdges; j = i++) {
        precomputedEdges[i].xi = m_lassoPoints[i].x;
        precomputedEdges[i].yi = m_lassoPoints[i].y;
        precomputedEdges[i].yj = m_lassoPoints[j].y;
        precomputedEdges[i].invSlope = (m_lassoPoints[j].x - m_lassoPoints[i].x) /
                                       (m_lassoPoints[j].y - m_lassoPoints[i].y + 1e-12f);
    }
    const PrecomputedLassoEdge* edgeData = precomputedEdges.data();

    glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
    float width = (float)camera.getWidth();
    float height = (float)camera.getHeight();
    if (width <= 0.0f || height <= 0.0f) return {};

    int nbVerts = mesh->nbVerts;
    const float* verts = mesh->verts.data();

    bool is2D = camera.getRef2DMode();
    float zoom2D = camera.getView2DZoom();
    float offX2D = camera.getView2DOffsetX();
    float offY2D = camera.getView2DOffsetY();

    // Map screen bounds [lassoMinX, lassoMaxX] x [lassoMinY, lassoMaxY] to NDC bounds
    float ndcMinX = (2.0f * lassoMinX / width) - 1.0f;
    float ndcMaxX = (2.0f * lassoMaxX / width) - 1.0f;
    float ndcMinY = 1.0f - (2.0f * lassoMaxY / height);
    float ndcMaxY = 1.0f - (2.0f * lassoMinY / height);

    if (is2D && zoom2D > 1e-6f) {
        ndcMinX = (ndcMinX - offX2D) / zoom2D;
        ndcMaxX = (ndcMaxX - offX2D) / zoom2D;
        ndcMinY = (ndcMinY - offY2D) / zoom2D;
        ndcMaxY = (ndcMaxY - offY2D) / zoom2D;
    }

    // Safety margin to prevent any precision edge exclusion
    ndcMinX -= 1e-5f;
    ndcMaxX += 1e-5f;
    ndcMinY -= 1e-5f;
    ndcMaxY += 1e-5f;

    struct CachedSymMatrix {
        glm::mat4 symMvp;
    };
    std::vector<CachedSymMatrix> cachedSymMatrices;
    if (m_useSym) {
        std::vector<glm::vec3> symScales = getActiveSymmetryScales();
        cachedSymMatrices.reserve(symScales.size());
        for (const auto& sScale : symScales) {
            std::vector<std::pair<glm::vec3, glm::vec3>> planes;
            for (int axis = 0; axis < 3; ++axis) {
                if (sScale[axis] < 0.0f) {
                    planes.push_back({
                        mesh->getSymmetryOriginForAxis(axis, m_symmetryMode),
                        mesh->getSymmetryNormalForAxis(axis, m_symmetryMode)
                    });
                }
            }
            if (planes.empty()) continue;

            auto reflectPoint = [&](const glm::vec3& p) {
                glm::vec3 res = p;
                for (const auto& plane : planes) {
                    float dist = glm::dot(res - plane.first, plane.second);
                    res -= 2.0f * dist * plane.second;
                }
                return res;
            };

            glm::vec3 b  = reflectPoint(glm::vec3(0.0f, 0.0f, 0.0f));
            glm::vec3 c0 = reflectPoint(glm::vec3(1.0f, 0.0f, 0.0f)) - b;
            glm::vec3 c1 = reflectPoint(glm::vec3(0.0f, 1.0f, 0.0f)) - b;
            glm::vec3 c2 = reflectPoint(glm::vec3(0.0f, 0.0f, 1.0f)) - b;

            glm::mat4 symMat(
                glm::vec4(c0, 0.0f),
                glm::vec4(c1, 0.0f),
                glm::vec4(c2, 0.0f),
                glm::vec4(b,  1.0f)
            );
            cachedSymMatrices.push_back({ mvp * symMat });
        }
    }

    float m00 = mvp[0][0], m10 = mvp[1][0], m20 = mvp[2][0], m30 = mvp[3][0];
    float m01 = mvp[0][1], m11 = mvp[1][1], m21 = mvp[2][1], m31 = mvp[3][1];
    float m03 = mvp[0][3], m13 = mvp[1][3], m23 = mvp[2][3], m33 = mvp[3][3];

    float sxScale = (is2D ? zoom2D : 1.0f) * (0.5f * width);
    float sxBias  = (is2D ? (offX2D + 1.0f) : 1.0f) * (0.5f * width);
    float syScale = -(is2D ? zoom2D : 1.0f) * (0.5f * height);
    float syBias  = (is2D ? (1.0f - offY2D) : 1.0f) * (0.5f * height);

    struct CachedSymCoeffs {
        float sm00, sm10, sm20, sm30;
        float sm01, sm11, sm21, sm31;
        float sm03, sm13, sm23, sm33;
    };
    std::vector<CachedSymCoeffs> cachedSymCoeffs;
    cachedSymCoeffs.reserve(cachedSymMatrices.size());
    for (const auto& csm : cachedSymMatrices) {
        cachedSymCoeffs.push_back({
            csm.symMvp[0][0], csm.symMvp[1][0], csm.symMvp[2][0], csm.symMvp[3][0],
            csm.symMvp[0][1], csm.symMvp[1][1], csm.symMvp[2][1], csm.symMvp[3][1],
            csm.symMvp[0][3], csm.symMvp[1][3], csm.symMvp[2][3], csm.symMvp[3][3]
        });
    }

    size_t numSym = cachedSymCoeffs.size();
    const CachedSymCoeffs* symCoeffsData = cachedSymCoeffs.data();

    struct FrustumPlane {
        float a, b, c, d;
    };
    auto makeFrustum = [&](const glm::mat4& m, float nMinX, float nMaxX, float nMinY, float nMaxY) {
        std::array<FrustumPlane, 5> p;
        float r00 = m[0][0], r10 = m[1][0], r20 = m[2][0], r30 = m[3][0];
        float r01 = m[0][1], r11 = m[1][1], r21 = m[2][1], r31 = m[3][1];
        float r03 = m[0][3], r13 = m[1][3], r23 = m[2][3], r33 = m[3][3];
        p[0] = { r00 - nMinX * r03, r10 - nMinX * r13, r20 - nMinX * r23, r30 - nMinX * r33 };
        p[1] = { nMaxX * r03 - r00, nMaxX * r13 - r10, nMaxX * r23 - r20, nMaxX * r33 - r30 };
        p[2] = { r01 - nMinY * r03, r11 - nMinY * r13, r21 - nMinY * r23, r31 - nMinY * r33 };
        p[3] = { nMaxY * r03 - r01, nMaxY * r13 - r11, nMaxY * r23 - r21, nMaxY * r33 - r31 };
        p[4] = { r03, r13, r23, r33 };
        return p;
    };

    auto testAabbFrustum = [](const std::array<FrustumPlane, 5>& planes, const glm::vec3& bMin, const glm::vec3& bMax) -> bool {
        for (int i = 0; i < 5; ++i) {
            const auto& pl = planes[i];
            float px = (pl.a >= 0.0f) ? bMax.x : bMin.x;
            float py = (pl.b >= 0.0f) ? bMax.y : bMin.y;
            float pz = (pl.c >= 0.0f) ? bMax.z : bMin.z;
            if (pl.a * px + pl.b * py + pl.c * pz + pl.d < 0.0f) {
                return false;
            }
        }
        return true;
    };

    std::array<FrustumPlane, 5> primaryFrustum = makeFrustum(mvp, ndcMinX, ndcMaxX, ndcMinY, ndcMaxY);
    std::vector<std::array<FrustumPlane, 5>> symFrustums;
    if (numSym > 0) {
        symFrustums.reserve(numSym);
        for (const auto& csm : cachedSymMatrices) {
            symFrustums.push_back(makeFrustum(csm.symMvp, ndcMinX, ndcMaxX, ndcMinY, ndcMaxY));
        }
    }

    std::vector<uint32_t> candidateLeaves;
    bool useCandidateLeaves = false;

    if (mesh->bvh.isBuilt()) {
        const auto& nodes = mesh->bvh.nodes();
        bool rootIntersects = testAabbFrustum(primaryFrustum, nodes[0].aabbMin, nodes[0].aabbMax);
        if (!rootIntersects && numSym > 0) {
            for (const auto& sf : symFrustums) {
                if (testAabbFrustum(sf, nodes[0].aabbMin, nodes[0].aabbMax)) {
                    rootIntersects = true;
                    break;
                }
            }
        }
        if (!rootIntersects) {
            sculpt_log("[getVerticesInLasso] Mesh AABB culled outside lasso screen bounds in 0.00 ms\n");
            return {};
        }

        candidateLeaves.reserve(mesh->bvh.leafCount());
        uint32_t nodeStack[128];
        int stackTop = 0;
        nodeStack[stackTop++] = 0;

        while (stackTop > 0) {
            uint32_t nodeIdx = nodeStack[--stackTop];
            const BVHNode& node = nodes[nodeIdx];
            bool inFrustum = testAabbFrustum(primaryFrustum, node.aabbMin, node.aabbMax);
            if (!inFrustum && numSym > 0) {
                for (const auto& sf : symFrustums) {
                    if (testAabbFrustum(sf, node.aabbMin, node.aabbMax)) {
                        inFrustum = true;
                        break;
                    }
                }
            }
            if (!inFrustum) continue;

            if (node.isLeaf()) {
                candidateLeaves.push_back(nodeIdx);
            } else {
                nodeStack[stackTop++] = node.firstIndex;
                nodeStack[stackTop++] = node.firstIndex + 1;
            }
        }

        size_t totalLeaves = mesh->bvh.leafCount();
        float candRatio = totalLeaves > 0 ? ((float)candidateLeaves.size() / (float)totalLeaves) : 1.0f;
        if (candRatio < 0.65f && !candidateLeaves.empty()) {
            useCandidateLeaves = true;
        }
    }

    int maxThreads = omp_get_max_threads();
    static std::vector<std::vector<uint32_t>> s_threadSelected;
    if ((int)s_threadSelected.size() < maxThreads) {
        s_threadSelected.resize(maxThreads);
    }
    size_t maxPerThread = (size_t)(nbVerts / maxThreads) + 4096;
    #pragma omp parallel for schedule(static)
    for (int t = 0; t < maxThreads; ++t) {
        if (s_threadSelected[t].size() < maxPerThread) {
            s_threadSelected[t].resize(maxPerThread);
        }
    }
    std::vector<size_t> threadCounts(maxThreads, 0);
    auto tBufDone = std::chrono::high_resolution_clock::now();

    const uint8_t* vVis = mesh->vertVisible.empty() ? nullptr : mesh->vertVisible.data();

    if (useCandidateLeaves) {
        if (m_vertTagFlags.size() < (size_t)nbVerts) {
            m_vertTagFlags.assign(nbVerts, uint8_t(0));
            m_vertTagEpoch = 0;
        }
        m_vertTagEpoch++;
        if (m_vertTagEpoch == 0) {
            std::fill(m_vertTagFlags.begin(), m_vertTagFlags.end(), uint8_t(0));
            m_vertTagEpoch = 1;
        }
        uint8_t curEpoch = m_vertTagEpoch;
        uint8_t* tagData = m_vertTagFlags.data();

        const auto& nodes = mesh->bvh.nodes();
        const uint32_t* primIndices = mesh->bvh.primitiveIndices().data();
        const uint32_t* faces = mesh->faces.data();
        size_t numCand = candidateLeaves.size();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            uint32_t* outPtr = s_threadSelected[tid].data();
            size_t localCount = 0;

            #pragma omp for schedule(dynamic, 64)
            for (int64_t l = 0; l < (int64_t)numCand; ++l) {
                const BVHNode& leaf = nodes[candidateLeaves[l]];
                for (uint32_t f = 0; f < leaf.count; ++f) {
                    uint32_t faceId = primIndices[leaf.firstIndex + f];
                    const uint32_t* fv = faces + (faceId << 2);
                    for (int k = 0; k < 4; ++k) {
                        uint32_t v = fv[k];
                        if (v == 0xFFFFFFFF || v >= (uint32_t)nbVerts) continue;
                        std::atomic_ref<uint8_t> ref(tagData[v]);
                        if (ref.exchange(curEpoch, std::memory_order_relaxed) == curEpoch) continue;

                        if (vVis && !vVis[v]) continue;

                        int ind = v * 3;
                        float px = verts[ind + 0], py = verts[ind + 1], pz = verts[ind + 2];

                        float w = m03 * px + m13 * py + m23 * pz + m33;
                        bool isInside = false;

                        if (w > 0.0f) {
                            float clipX = m00 * px + m10 * py + m20 * pz + m30;
                            if (clipX >= ndcMinX * w && clipX <= ndcMaxX * w) {
                                float clipY = m01 * px + m11 * py + m21 * pz + m31;
                                if (clipY >= ndcMinY * w && clipY <= ndcMaxY * w) {
                                    float invW = 1.0f / w;
                                    float screenX = clipX * (invW * sxScale) + sxBias;
                                    float screenY = clipY * (invW * syScale) + syBias;

                                    if (isPointInPolygonFast(screenX, screenY, edgeData, numEdges)) {
                                        isInside = true;
                                    }
                                }
                            }
                        }

                        if (!isInside && numSym > 0) {
                            for (size_t s = 0; s < numSym; ++s) {
                                const auto& csc = symCoeffsData[s];
                                float sw = csc.sm03 * px + csc.sm13 * py + csc.sm23 * pz + csc.sm33;
                                if (sw > 0.0f) {
                                    float symClipX = csc.sm00 * px + csc.sm10 * py + csc.sm20 * pz + csc.sm30;
                                    if (symClipX >= ndcMinX * sw && symClipX <= ndcMaxX * sw) {
                                        float symClipY = csc.sm01 * px + csc.sm11 * py + csc.sm21 * pz + csc.sm31;
                                        if (symClipY >= ndcMinY * sw && symClipY <= ndcMaxY * sw) {
                                            float invSw = 1.0f / sw;
                                            float screenX = symClipX * (invSw * sxScale) + sxBias;
                                            float screenY = symClipY * (invSw * syScale) + syBias;

                                            if (isPointInPolygonFast(screenX, screenY, edgeData, numEdges)) {
                                                isInside = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (isInside) {
                            outPtr[localCount++] = v;
                        }
                    }
                }
            }
            threadCounts[tid] = localCount;
        }
    } else {
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            uint32_t* outPtr = s_threadSelected[tid].data();
            size_t localCount = 0;

            #pragma omp for schedule(static)
            for (int i = 0; i < nbVerts; ++i) {
                if (i + 16 < nbVerts) {
                    __builtin_prefetch(&verts[(i + 16) * 3], 0, 1);
                }
                if (vVis && !vVis[i]) continue;

                int ind = i * 3;
                float px = verts[ind + 0], py = verts[ind + 1], pz = verts[ind + 2];

                float w = m03 * px + m13 * py + m23 * pz + m33;
                bool isInside = false;

                if (w > 0.0f) {
                    float clipX = m00 * px + m10 * py + m20 * pz + m30;
                    if (clipX >= ndcMinX * w && clipX <= ndcMaxX * w) {
                        float clipY = m01 * px + m11 * py + m21 * pz + m31;
                        if (clipY >= ndcMinY * w && clipY <= ndcMaxY * w) {
                            float invW = 1.0f / w;
                            float screenX = clipX * (invW * sxScale) + sxBias;
                            float screenY = clipY * (invW * syScale) + syBias;

                            if (isPointInPolygonFast(screenX, screenY, edgeData, numEdges)) {
                                isInside = true;
                            }
                        }
                    }
                }

                if (!isInside && numSym > 0) {
                    for (size_t s = 0; s < numSym; ++s) {
                        const auto& csc = symCoeffsData[s];
                        float sw = csc.sm03 * px + csc.sm13 * py + csc.sm23 * pz + csc.sm33;
                        if (sw > 0.0f) {
                            float symClipX = csc.sm00 * px + csc.sm10 * py + csc.sm20 * pz + csc.sm30;
                            if (symClipX >= ndcMinX * sw && symClipX <= ndcMaxX * sw) {
                                float symClipY = csc.sm01 * px + csc.sm11 * py + csc.sm21 * pz + csc.sm31;
                                if (symClipY >= ndcMinY * sw && symClipY <= ndcMaxY * sw) {
                                    float invSw = 1.0f / sw;
                                    float screenX = symClipX * (invSw * sxScale) + sxBias;
                                    float screenY = symClipY * (invSw * syScale) + syBias;

                                    if (isPointInPolygonFast(screenX, screenY, edgeData, numEdges)) {
                                        isInside = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                if (isInside) {
                    outPtr[localCount++] = (uint32_t)i;
                }
            }
            threadCounts[tid] = localCount;
        }
    }
    auto tScanDone = std::chrono::high_resolution_clock::now();

    // Prefix sum concatenation
    std::vector<size_t> offsets(maxThreads + 1, 0);
    for (int t = 0; t < maxThreads; ++t) {
        offsets[t + 1] = offsets[t] + threadCounts[t];
    }
    size_t totalSelected = offsets[maxThreads];

    std::vector<uint32_t> insideVertices(totalSelected);

    #pragma omp parallel for schedule(static)
    for (int t = 0; t < maxThreads; ++t) {
        size_t count = threadCounts[t];
        if (count > 0) {
            std::memcpy(insideVertices.data() + offsets[t],
                        s_threadSelected[t].data(),
                        count * sizeof(uint32_t));
        }
    }
    for (int t = 0; t < maxThreads; ++t) {
        std::vector<uint32_t>().swap(s_threadSelected[t]);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double bufMs = std::chrono::duration<double, std::milli>(tBufDone - t0).count();
    double scanMs = std::chrono::duration<double, std::milli>(tScanDone - tBufDone).count();
    double gatherMs = std::chrono::duration<double, std::milli>(t1 - tScanDone).count();

    sculpt_log("[getVerticesInLasso] Processed %d verts (Selected: %zu, Sym: %s, LassoPts: %zu) in %.2f ms (BufPrep: %.2f, Scan: %.2f, Gather: %.2f)\n",
              nbVerts, insideVertices.size(), m_useSym ? "ON" : "OFF", m_lassoPoints.size(), ms, bufMs, scanMs, gatherMs);

    return insideVertices;
}

bool SculptManager::saveSettings(IniFile& ini) {
    for (int i = 0; i < BRUSH_COUNT; ++i) {
        std::string sec = "Brush_" + std::to_string(i);
        ini.setFloat(sec, "radius", m_brushSettings[i].radius);
        ini.setFloat(sec, "intensity", m_brushSettings[i].intensity);
        ini.setFloat(sec, "focalShift", m_brushSettings[i].focalShift);
        ini.setBool(sec, "focalShiftFalloff", m_brushSettings[i].focalShiftFalloff);
        ini.setFloat(sec, "hardness", m_brushSettings[i].hardness);
        ini.setFloat(sec, "spacing", m_brushSettings[i].spacing);
        ini.setBool(sec, "negative", m_brushSettings[i].negative);
        ini.setBool(sec, "culling", m_brushSettings[i].culling);
        ini.setBool(sec, "accumulate", m_brushSettings[i].accumulate);
        ini.setBool(sec, "lockPosition", m_brushSettings[i].lockPosition);
        ini.setInt(sec, "idAlpha", m_brushSettings[i].idAlpha);
        ini.setBool(sec, "clay", m_brushSettings[i].clay);
        ini.setBool(sec, "tangent", m_brushSettings[i].tangent);
        ini.setBool(sec, "topoCheck", m_brushSettings[i].topoCheck);
        ini.setFloat(sec, "elasticity", m_brushSettings[i].elasticity);
        
        glm::vec3 col = m_brushSettings[i].paintColor;
        ini.set(sec, "paintColor", std::to_string(col.r) + " " + std::to_string(col.g) + " " + std::to_string(col.b));
        ini.setFloat(sec, "paintRoughness", m_brushSettings[i].paintRoughness);
        ini.setFloat(sec, "paintMetallic", m_brushSettings[i].paintMetallic);
        ini.setBool(sec, "writeAlbedo", m_brushSettings[i].writeAlbedo);
        ini.setBool(sec, "writeRoughness", m_brushSettings[i].writeRoughness);
        ini.setBool(sec, "writeMetalness", m_brushSettings[i].writeMetalness);
        ini.setInt(sec, "maskSharpenBlurIterations", m_brushSettings[i].maskSharpenBlurIterations);
        ini.setFloat(sec, "maskSharpenFactor", m_brushSettings[i].maskSharpenFactor);
        ini.setFloat(sec, "maskExtractThickness", m_brushSettings[i].maskExtractThickness);
        ini.setBool(sec, "blurMaskedOnly", m_brushSettings[i].blurMaskedOnly);
        ini.setInt(sec, "stampType", m_brushSettings[i].stampType);
        ini.setInt(sec, "stampSides", m_brushSettings[i].stampSides);
        ini.setFloat(sec, "stampInnerRatio", m_brushSettings[i].stampInnerRatio);
        ini.setFloat(sec, "stampAngle", m_brushSettings[i].stampAngle);
        ini.setFloat(sec, "stampBlur", m_brushSettings[i].stampBlur);
        ini.setBool(sec, "stampLockRotation", m_brushSettings[i].stampLockRotation);
        ini.setBool(sec, "stampUseTilt", m_brushSettings[i].stampUseTilt);

        ini.setInt(sec, "relaxAlgorithm", m_brushSettings[i].relaxAlgorithm);
        ini.setInt(sec, "relaxMode", m_brushSettings[i].relaxMode);
        ini.setBool(sec, "relaxLockBorder", m_brushSettings[i].relaxLockBorder);
        ini.setBool(sec, "relaxLockInterior", m_brushSettings[i].relaxLockInterior);
        ini.setBool(sec, "relaxPreserveCreases", m_brushSettings[i].relaxPreserveCreases);
        ini.setFloat(sec, "relaxCreaseAngle", m_brushSettings[i].relaxCreaseAngle);
        ini.setInt(sec, "relaxIterations", m_brushSettings[i].relaxIterations);
        ini.setInt(sec, "splitMode", m_brushSettings[i].splitMode);

        ini.setBool(sec, "rakeClayMode", m_brushSettings[i].rakeClayMode);
        ini.setBool(sec, "rakeSquareFootprint", m_brushSettings[i].rakeSquareFootprint);
        ini.setBool(sec, "rakeRidgesOnly", m_brushSettings[i].rakeRidgesOnly);
        ini.setInt(sec, "rakeWaveType", m_brushSettings[i].rakeWaveType);
        ini.setInt(sec, "rakeTeethCount", m_brushSettings[i].rakeTeethCount);
        ini.setFloat(sec, "rakeDepth", m_brushSettings[i].rakeDepth);
        ini.setFloat(sec, "rakeSharpness", m_brushSettings[i].rakeSharpness);
        ini.setFloat(sec, "rakeTurnSmoothness", m_brushSettings[i].rakeTurnSmoothness);
        ini.setFloat(sec, "rakeAngleOffset", m_brushSettings[i].rakeAngleOffset);
    }

    std::string genSec = "General";
    ini.setInt(genSec, "dividerDivisions", m_dividerDivisions);
    ini.setBool(genSec, "measureUseDistanceThickness", m_measureUseDistanceThickness);
#ifdef _WIN32
    ini.setBool(genSec, "usePressure", g_tablet.isPressureEnabled());
    ini.setBool(genSec, "usePressureSize", g_tablet.isPressureSizeEnabled());
    ini.setBool(genSec, "usePressureCursor", g_tablet.isPressureCursorEnabled());
    ini.setBool(genSec, "useTilt", g_tablet.isTiltEnabled());
    ini.set(genSec, "pressureCurve", g_tablet.getPressureCurveString());
    ini.setInt(genSec, "pressureCurveType", (int)g_tablet.getInterpolationType());
#endif

    std::string symSec = "Symmetry";
    ini.setBool(symSec, "useSym", m_useSym);
    ini.setBool(symSec, "symX", m_symX);
    ini.setBool(symSec, "symY", m_symY);
    ini.setBool(symSec, "symZ", m_symZ);
    ini.set(symSec, "symmetryMode", (m_symmetryMode == SymmetryMode::World ? "world" : "local"));

    return true;
}

bool SculptManager::loadSettings(const IniFile& ini) {
    for (int i = 0; i < BRUSH_COUNT; ++i) {
        std::string sec = "Brush_" + std::to_string(i);
        if (!ini.hasSection(sec)) continue;

        if (ini.hasKey(sec, "radius")) m_brushSettings[i].radius = ini.getFloat(sec, "radius", 50.0f);
        if (ini.hasKey(sec, "intensity")) m_brushSettings[i].intensity = ini.getFloat(sec, "intensity", 0.5f);
        if (ini.hasKey(sec, "focalShift")) m_brushSettings[i].focalShift = ini.getFloat(sec, "focalShift", 0.0f);
        if (ini.hasKey(sec, "focalShiftFalloff")) m_brushSettings[i].focalShiftFalloff = ini.getBool(sec, "focalShiftFalloff");
        if (ini.hasKey(sec, "hardness")) m_brushSettings[i].hardness = ini.getFloat(sec, "hardness", 0.5f);
        if (ini.hasKey(sec, "spacing")) m_brushSettings[i].spacing = ini.getFloat(sec, "spacing", 0.15f);
        if (ini.hasKey(sec, "negative")) m_brushSettings[i].negative = ini.getBool(sec, "negative");
        if (ini.hasKey(sec, "culling")) m_brushSettings[i].culling = ini.getBool(sec, "culling");
        if (ini.hasKey(sec, "accumulate")) m_brushSettings[i].accumulate = ini.getBool(sec, "accumulate");
        if (ini.hasKey(sec, "lockPosition")) m_brushSettings[i].lockPosition = ini.getBool(sec, "lockPosition");
        if (ini.hasKey(sec, "idAlpha")) m_brushSettings[i].idAlpha = ini.getInt(sec, "idAlpha", 0);
        if (ini.hasKey(sec, "clay")) m_brushSettings[i].clay = ini.getBool(sec, "clay");
        if (ini.hasKey(sec, "tangent")) m_brushSettings[i].tangent = ini.getBool(sec, "tangent");
        if (ini.hasKey(sec, "topoCheck")) m_brushSettings[i].topoCheck = ini.getBool(sec, "topoCheck");
        if (ini.hasKey(sec, "elasticity")) m_brushSettings[i].elasticity = ini.getFloat(sec, "elasticity", 1.0f);

        if (ini.hasKey(sec, "paintColor")) {
            std::stringstream ss(ini.get(sec, "paintColor"));
            float r, g, b;
            if (ss >> r >> g >> b) {
                m_brushSettings[i].paintColor = glm::vec3(r, g, b);
            }
        }

        if (ini.hasKey(sec, "paintRoughness")) m_brushSettings[i].paintRoughness = ini.getFloat(sec, "paintRoughness", 0.5f);
        if (ini.hasKey(sec, "paintMetallic")) m_brushSettings[i].paintMetallic = ini.getFloat(sec, "paintMetallic", 0.0f);

        if (ini.hasKey(sec, "writeAlbedo")) m_brushSettings[i].writeAlbedo = ini.getBool(sec, "writeAlbedo");
        if (ini.hasKey(sec, "writeRoughness")) m_brushSettings[i].writeRoughness = ini.getBool(sec, "writeRoughness");
        if (ini.hasKey(sec, "writeMetalness")) m_brushSettings[i].writeMetalness = ini.getBool(sec, "writeMetalness");

        if (ini.hasKey(sec, "maskSharpenBlurIterations")) m_brushSettings[i].maskSharpenBlurIterations = ini.getInt(sec, "maskSharpenBlurIterations", 4);
        if (ini.hasKey(sec, "maskSharpenFactor")) m_brushSettings[i].maskSharpenFactor = ini.getFloat(sec, "maskSharpenFactor", 1.0f);
        if (ini.hasKey(sec, "maskExtractThickness")) m_brushSettings[i].maskExtractThickness = ini.getFloat(sec, "maskExtractThickness", 0.05f);
        if (ini.hasKey(sec, "blurMaskedOnly")) m_brushSettings[i].blurMaskedOnly = ini.getBool(sec, "blurMaskedOnly");
        if (ini.hasKey(sec, "stampType")) m_brushSettings[i].stampType = ini.getInt(sec, "stampType", 0);
        if (ini.hasKey(sec, "stampSides")) m_brushSettings[i].stampSides = ini.getInt(sec, "stampSides", 5);
        if (ini.hasKey(sec, "stampInnerRatio")) m_brushSettings[i].stampInnerRatio = ini.getFloat(sec, "stampInnerRatio", 0.5f);
        if (ini.hasKey(sec, "stampAngle")) m_brushSettings[i].stampAngle = ini.getFloat(sec, "stampAngle", 0.0f);
        if (ini.hasKey(sec, "stampBlur")) m_brushSettings[i].stampBlur = ini.getFloat(sec, "stampBlur", 0.0f);
        if (ini.hasKey(sec, "stampLockRotation")) m_brushSettings[i].stampLockRotation = ini.getBool(sec, "stampLockRotation");
        if (ini.hasKey(sec, "stampUseTilt")) m_brushSettings[i].stampUseTilt = ini.getBool(sec, "stampUseTilt");

        if (ini.hasKey(sec, "relaxAlgorithm")) m_brushSettings[i].relaxAlgorithm = ini.getInt(sec, "relaxAlgorithm", 1);
        if (ini.hasKey(sec, "relaxMode")) m_brushSettings[i].relaxMode = ini.getInt(sec, "relaxMode", 0);
        if (ini.hasKey(sec, "relaxLockBorder")) m_brushSettings[i].relaxLockBorder = ini.getBool(sec, "relaxLockBorder");
        if (ini.hasKey(sec, "relaxLockInterior")) m_brushSettings[i].relaxLockInterior = ini.getBool(sec, "relaxLockInterior");
        if (ini.hasKey(sec, "relaxPreserveCreases")) m_brushSettings[i].relaxPreserveCreases = ini.getBool(sec, "relaxPreserveCreases");
        if (ini.hasKey(sec, "relaxCreaseAngle")) m_brushSettings[i].relaxCreaseAngle = ini.getFloat(sec, "relaxCreaseAngle", 45.0f);
        if (ini.hasKey(sec, "relaxIterations")) m_brushSettings[i].relaxIterations = ini.getInt(sec, "relaxIterations", 1);
        if (ini.hasKey(sec, "splitMode")) m_brushSettings[i].splitMode = ini.getInt(sec, "splitMode", 0);

        if (ini.hasKey(sec, "rakeClayMode")) m_brushSettings[i].rakeClayMode = ini.getBool(sec, "rakeClayMode");
        if (ini.hasKey(sec, "rakeSquareFootprint")) m_brushSettings[i].rakeSquareFootprint = ini.getBool(sec, "rakeSquareFootprint");
        if (ini.hasKey(sec, "rakeRidgesOnly")) m_brushSettings[i].rakeRidgesOnly = ini.getBool(sec, "rakeRidgesOnly");
        if (ini.hasKey(sec, "rakeWaveType")) m_brushSettings[i].rakeWaveType = ini.getInt(sec, "rakeWaveType", 0);
        if (ini.hasKey(sec, "rakeTeethCount")) m_brushSettings[i].rakeTeethCount = ini.getInt(sec, "rakeTeethCount", 6);
        if (ini.hasKey(sec, "rakeDepth")) m_brushSettings[i].rakeDepth = ini.getFloat(sec, "rakeDepth", 0.85f);
        if (ini.hasKey(sec, "rakeSharpness")) m_brushSettings[i].rakeSharpness = ini.getFloat(sec, "rakeSharpness", 0.5f);
        if (ini.hasKey(sec, "rakeTurnSmoothness")) m_brushSettings[i].rakeTurnSmoothness = ini.getFloat(sec, "rakeTurnSmoothness", 0.65f);
        if (ini.hasKey(sec, "rakeAngleOffset")) m_brushSettings[i].rakeAngleOffset = ini.getFloat(sec, "rakeAngleOffset", 0.0f);
    }

    std::string genSec = "General";
    if (ini.hasSection(genSec)) {
        if (ini.hasKey(genSec, "dividerDivisions")) m_dividerDivisions = ini.getInt(genSec, "dividerDivisions", 3);
        if (ini.hasKey(genSec, "measureUseDistanceThickness")) m_measureUseDistanceThickness = ini.getBool(genSec, "measureUseDistanceThickness");
#ifdef _WIN32
        if (ini.hasKey(genSec, "usePressure")) g_tablet.setPressureEnabled(ini.getBool(genSec, "usePressure"));
        if (ini.hasKey(genSec, "usePressureSize")) g_tablet.setPressureSizeEnabled(ini.getBool(genSec, "usePressureSize"));
        if (ini.hasKey(genSec, "usePressureCursor")) g_tablet.setPressureCursorEnabled(ini.getBool(genSec, "usePressureCursor"));
        if (ini.hasKey(genSec, "useTilt")) g_tablet.setTiltEnabled(ini.getBool(genSec, "useTilt"));
        if (ini.hasKey(genSec, "pressureCurve")) g_tablet.setPressureCurveFromString(ini.get(genSec, "pressureCurve"));
        if (ini.hasKey(genSec, "pressureCurveType")) g_tablet.setInterpolationType((TabletInput::InterpolationType)ini.getInt(genSec, "pressureCurveType"));
#endif
    }

    std::string symSec = "Symmetry";
    if (ini.hasSection(symSec)) {
        if (ini.hasKey(symSec, "useSym")) m_useSym = ini.getBool(symSec, "useSym");
        if (ini.hasKey(symSec, "symX")) m_symX = ini.getBool(symSec, "symX");
        if (ini.hasKey(symSec, "symY")) m_symY = ini.getBool(symSec, "symY");
        if (ini.hasKey(symSec, "symZ")) m_symZ = ini.getBool(symSec, "symZ");

        if (ini.hasKey(symSec, "symmetryMode")) {
            std::string modeStr = ini.get(symSec, "symmetryMode");
            if (modeStr == "world" || modeStr == "1") {
                m_symmetryMode = SymmetryMode::World;
            } else {
                m_symmetryMode = SymmetryMode::Local;
            }
        }
    }

    return true;
}

void SculptManager::clearMask(Mesh* mesh, Scene* scene) {
    sculpt_log("[clearMask] Called for mesh %p...\n", mesh);
    if (!mesh) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) {
        sculpt_log("[clearMask] Error: materials buffer size mismatch!\n");
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    bool hasMask = false;
    for (int i = 0; i < nbVerts; ++i) {
        if (materials[i * 3 + 2] < 1.0f) {
            hasMask = true;
            break;
        }
    }
    if (!hasMask) {
        sculpt_log("[clearMask] Mesh already unmasked (0 masked verts). Early-out.\n");
        return;
    }

    double histMs = 0.0;
    if (scene) {
        auto tHist0 = std::chrono::high_resolution_clock::now();
        scene->pushHistoryState();
        auto tHist1 = std::chrono::high_resolution_clock::now();
        histMs = std::chrono::duration<double, std::milli>(tHist1 - tHist0).count();
    }

    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f;
    }
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;

    auto t1 = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    sculpt_log("[clearMask] Cleared mask on %d verts (HistoryPush: %.2f ms, Total: %.2f ms)\n",
              nbVerts, histMs, totalMs);
}

void SculptManager::invertMask(Mesh* mesh) {
    sculpt_log("[invertMask] Inverting mask for mesh %p (nbVerts=%d)...\n", mesh, mesh ? mesh->nbVerts : 0);
    if (!mesh) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;

    auto t0 = std::chrono::high_resolution_clock::now();
    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f - materials[i * 3 + 2];
    }
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    sculpt_log("[invertMask] Mask inverted on %d verts in %.2f ms.\n", nbVerts, ms);
}

void SculptManager::blurMask(Mesh* mesh, int iterations) {
    std::cout << "[MaskBlur] Call blurMask(mesh=" << mesh << ")..." << std::endl;
    if (!mesh) {
        std::cout << "[MaskBlur] Error: mesh is null!" << std::endl;
        return;
    }
    if (mesh->nbVerts <= 0) {
        std::cout << "[MaskBlur] Error: mesh->nbVerts <= 0 (" << mesh->nbVerts << ")" << std::endl;
        return;
    }
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) {
        std::cout << "[MaskBlur] Error: materials array size mismatch! size=" << mesh->materials.size() << " expected=" << (mesh->nbVerts * 3) << std::endl;
        return;
    }
    if (mesh->vrvStartCount.empty() || mesh->vertRingVert.empty()) {
        std::cout << "[MaskBlur] Error: topology empty! vrvStartCount.size=" << mesh->vrvStartCount.size() << " vertRingVert.size=" << mesh->vertRingVert.size() << std::endl;
        return;
    }

    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    std::cout << "[MaskBlur] Initial masked verts count: " << iVerts.size() << " / " << nbVerts << std::endl;
    if (iVerts.empty()) return;

    if (iterations <= 0) {
        iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
    }
    if (iterations <= 0) iterations = 2;

    std::vector<uint8_t> visited(nbVerts, 0);
    for (uint32_t v : iVerts) {
        if (v < (uint32_t)nbVerts) visited[v] = 1;
    }

    std::vector<uint32_t> queue = iVerts;
    size_t head = 0;
    for (int step = 0; step < iterations; ++step) {
        size_t size = queue.size();
        while (head < size) {
            uint32_t id = queue[head++];
            if (id >= (uint32_t)nbVerts) continue;
            if (id * 2 + 1 >= mesh->vrvStartCount.size()) continue;

            uint32_t start = mesh->vrvStartCount[id * 2];
            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
            if (start + count > mesh->vertRingVert.size()) continue;

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t neighbor = mesh->vertRingVert[j];
                if (neighbor < (uint32_t)nbVerts && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
    }
    iVerts = queue;
    std::cout << "[MaskBlur] Expanded queue size: " << iVerts.size() << ", invoking ::blurMask..." << std::endl;

    int processed = ::blurMask(
        iVerts.data(), (int)iVerts.size(),
        mesh->vrvStartCount.data(),
        mesh->vertRingVert.data(),
        mesh->vertOnEdge.empty() ? nullptr : mesh->vertOnEdge.data(),
        iterations,
        mAr,
        3, // stride (materials contains 3 floats per vertex)
        2  // offset (mask is at index 2)
    );
    std::cout << "[MaskBlur] ::blurMask finished. Processed: " << processed << " vertices." << std::endl;

    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::sharpenMask(Mesh* mesh, int iterations) {
    std::cout << "[MaskSharpen] Call sharpenMask(mesh=" << mesh << ")..." << std::endl;
    if (!mesh || mesh->nbVerts <= 0) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;
    if (mesh->vrvStartCount.empty() || mesh->vertRingVert.empty()) return;

    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    std::cout << "[MaskSharpen] Initial masked verts count: " << iVerts.size() << " / " << nbVerts << std::endl;
    if (iVerts.empty()) return;

    if (iterations <= 0) {
        iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
    }
    if (iterations <= 0) iterations = 2;

    std::vector<uint8_t> visited(nbVerts, 0);
    for (uint32_t v : iVerts) {
        if (v < (uint32_t)nbVerts) visited[v] = 1;
    }

    std::vector<uint32_t> queue = iVerts;
    size_t head = 0;
    for (int step = 0; step < iterations; ++step) {
        size_t size = queue.size();
        while (head < size) {
            uint32_t id = queue[head++];
            if (id >= (uint32_t)nbVerts) continue;
            if (id * 2 + 1 >= mesh->vrvStartCount.size()) continue;

            uint32_t start = mesh->vrvStartCount[id * 2];
            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
            if (start + count > mesh->vertRingVert.size()) continue;

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t neighbor = mesh->vertRingVert[j];
                if (neighbor < (uint32_t)nbVerts && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
    }
    iVerts = queue;

    int nbActiveVerts = (int)iVerts.size();
    std::vector<float> originalMask(nbActiveVerts);
    for (int i = 0; i < nbActiveVerts; ++i) {
        originalMask[i] = mAr[iVerts[i] * 3 + 2];
    }

    ::blurMask(
        iVerts.data(), nbActiveVerts,
        mesh->vrvStartCount.data(),
        mesh->vertRingVert.data(),
        mesh->vertOnEdge.empty() ? nullptr : mesh->vertOnEdge.data(),
        iterations,
        mAr,
        3, // stride
        2  // offset
    );

    float factor = m_brushSettings[m_currentBrush].maskSharpenFactor;
    for (int i = 0; i < nbActiveVerts; ++i) {
        int idm = iVerts[i] * 3 + 2;
        float orig = originalMask[i];
        float blurred = mAr[idm];
        float val = orig + factor * (orig - blurred);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        mAr[idm] = val;
    }

    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::applyPreset(const BrushPreset& preset) {
    BrushType targetBrush = m_currentBrush;
    switch (preset.deformMode) {
        case DeformMode::Normal:
            targetBrush = BRUSH_BRUSH;
            break;
        case DeformMode::Clay:
            if (!preset.accumulate) {
                targetBrush = BRUSH_CLAYBUILDUP;
            } else {
                targetBrush = BRUSH_CLAY;
            }
            break;
        case DeformMode::Inflate:
            targetBrush = BRUSH_INFLATE;
            break;
        case DeformMode::Pinch:
            targetBrush = BRUSH_PINCH;
            break;
        case DeformMode::Crease:
            {
                std::string lowerName = preset.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find("dam") != std::string::npos) {
                    targetBrush = BRUSH_DAMSTANDARD;
                } else {
                    targetBrush = BRUSH_CREASE;
                }
            }
            break;
        case DeformMode::Flatten:
            targetBrush = BRUSH_FLATTEN;
            break;
        case DeformMode::Smooth:
            targetBrush = BRUSH_SMOOTH;
            break;
        case DeformMode::Move:
            {
                std::string lowerName = preset.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find("elastic") != std::string::npos) {
                    targetBrush = BRUSH_ELASTIC;
                } else if (lowerName.find("drag") != std::string::npos) {
                    targetBrush = BRUSH_DRAG;
                } else {
                    targetBrush = BRUSH_MOVE;
                }
            }
            break;
    }

    m_currentBrush = targetBrush;
    
    BrushSettings& settings = m_brushSettings[targetBrush];
    settings.radius = preset.radius;
    settings.intensity = preset.intensity;
    settings.spacing = preset.spacing;
    settings.hardness = preset.hardness;
    settings.focalShift = preset.focalShift;
    settings.focalShiftFalloff = preset.focalShiftFalloff;
    settings.negative = preset.negative;
    settings.culling = preset.culling;
    settings.singlePolyGroup = preset.singlePolyGroup;
    settings.accumulate = preset.accumulate;
    settings.lockPosition = preset.lockPosition;
    settings.idAlpha = preset.idAlpha;
    
    settings.strokeMode = preset.strokeMode;
    settings.deformMode = preset.deformMode;
    settings.altmode = preset.altmode;
    settings.lazyRadius = preset.lazyRadius;
    settings.lazySmooth = preset.lazySmooth;
    settings.grabRadius = preset.grabRadius;
    settings.grabRadiusScale = preset.grabRadiusScale;
    settings.areaNormalRadius = preset.areaNormalRadius;
    settings.areaPointRadius = preset.areaPointRadius;
    settings.areaSharp = preset.areaSharp;
    settings.areaSampling = preset.areaSampling;
    
    settings.flattenLockNormal = preset.flattenLockNormal;
    settings.flattenLockOrigin = preset.flattenLockOrigin;
    
    settings.smoothTaubin = preset.smoothTaubin;
    settings.smoothTaubinInflate = preset.smoothTaubinInflate;
    settings.smoothTaubinShrink = preset.smoothTaubinShrink;
    settings.smoothRelax = preset.smoothRelax;
    settings.smoothStable = preset.smoothStable;
    settings.smoothStickyBorder = preset.smoothStickyBorder;
    settings.tangent = preset.tangent;
    
    settings.depthFilter = preset.depthFilter;
    settings.connectedTopology = preset.connectedTopology;
    settings.onlyFrontFace = preset.onlyFrontFace;
    settings.topoCheck = preset.topoCheck;
    settings.useDynamicTopology = preset.useDynamicTopology;
    settings.elasticity = preset.elasticity;
    
    settings.paintColor = glm::vec3(preset.paintColor[0], preset.paintColor[1], preset.paintColor[2]);
    settings.paintRoughness = preset.roughness;
    settings.paintMetallic = preset.metallic;
    settings.writeAlbedo = preset.writeAlbedo;
    settings.writeRoughness = preset.writeRoughness;
    settings.writeMetalness = preset.writeMetalness;
    
    settings.pressureIntensity = preset.pressureIntensity;
    settings.pressureRadius = preset.pressureRadius;
    settings.useGlobalPressure = preset.useGlobalPressure;
    
    settings.falloff.preset = preset.falloff.preset;
    settings.falloff.points.clear();
    for (const auto& pt : preset.falloff.points) {
        settings.falloff.points.push_back({pt[0], pt[1]});
    }
    
    settings.subdivFactor = preset.subdivFactor;
    settings.decimFactor = preset.decimFactor;
}

void SculptManager::applyActivePreset() {
    const BrushPreset* activePreset = BrushPresetManager::instance().active();
    if (activePreset) {
        applyPreset(*activePreset);
    }
}

static glm::vec3 polyLerp(Mesh* mesh, uint32_t faceId, const glm::vec3& interPoint, const float* field) {
    if (!mesh || faceId == 0xffffffff || faceId >= (uint32_t)mesh->nbFaces) return glm::vec3(0.0f);
    
    uint32_t iv1 = mesh->faces[faceId * 4];
    uint32_t iv2 = mesh->faces[faceId * 4 + 1];
    uint32_t iv3 = mesh->faces[faceId * 4 + 2];
    uint32_t iv4 = mesh->faces[faceId * 4 + 3];
    bool isQuad = (iv4 != 0xffffffff);
    
    glm::vec3 v1(mesh->verts[iv1 * 3], mesh->verts[iv1 * 3 + 1], mesh->verts[iv1 * 3 + 2]);
    glm::vec3 v2(mesh->verts[iv2 * 3], mesh->verts[iv2 * 3 + 1], mesh->verts[iv2 * 3 + 2]);
    glm::vec3 v3(mesh->verts[iv3 * 3], mesh->verts[iv3 * 3 + 1], mesh->verts[iv3 * 3 + 2]);
    glm::vec3 v4(0.0f);
    if (isQuad) {
        v4 = glm::vec3(mesh->verts[iv4 * 3], mesh->verts[iv4 * 3 + 1], mesh->verts[iv4 * 3 + 2]);
    }
    
    float len1 = 1.0f / std::max(glm::distance(interPoint, v1), 1e-6f);
    float len2 = 1.0f / std::max(glm::distance(interPoint, v2), 1e-6f);
    float len3 = 1.0f / std::max(glm::distance(interPoint, v3), 1e-6f);
    float len4 = isQuad ? (1.0f / std::max(glm::distance(interPoint, v4), 1e-6f)) : 0.0f;
    
    float invSum = 1.0f / (len1 + len2 + len3 + len4);
    
    glm::vec3 f1(field[iv1 * 3], field[iv1 * 3 + 1], field[iv1 * 3 + 2]);
    glm::vec3 f2(field[iv2 * 3], field[iv2 * 3 + 1], field[iv2 * 3 + 2]);
    glm::vec3 f3(field[iv3 * 3], field[iv3 * 3 + 1], field[iv3 * 3 + 2]);
    glm::vec3 f4(0.0f);
    if (isQuad) {
        f4 = glm::vec3(field[iv4 * 3], field[iv4 * 3 + 1], field[iv4 * 3 + 2]);
    }
    
    return (f1 * len1 + f2 * len2 + f3 * len3 + f4 * len4) * invSum;
}

void SculptManager::pickColor(Mesh* mesh, uint32_t faceId, const glm::vec3& interPoint) {
    if (!mesh) return;
    glm::vec3 mat = polyLerp(mesh, faceId, interPoint, mesh->materials.data());
    glm::vec3 col = polyLerp(mesh, faceId, interPoint, mesh->colors.data());
    
    auto& settings = getCurrentSettings();
    settings.paintColor = col;
    settings.paintRoughness = mat.x;
    settings.paintMetallic = mat.y;
}

void SculptManager::paintAll(Scene& scene, Mesh* mesh) {
    if (!mesh) return;
    
    scene.pushHistoryState();

    int nbVerts = mesh->nbVerts;
    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mesh->materials[i * 3 + 2] > 0.0f) {
            iVerts.push_back(i);
        }
    }
    
    if (iVerts.empty()) return;

    const auto& settings = m_brushSettings[BRUSH_PAINT];
    strokePaintAll(
        mesh->colors.data(),
        mesh->materials.data(),
        iVerts.data(),
        iVerts.size(),
        settings.paintColor.r, settings.paintColor.g, settings.paintColor.b,
        settings.paintRoughness, settings.paintMetallic,
        settings.writeAlbedo, settings.writeRoughness, settings.writeMetalness
    );

    mesh->isColorDirty = true;
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::filterPolyGroupVertices(std::vector<uint32_t>& pickedVertices, const Mesh* mesh, uint32_t targetGroupId) {
    if (!mesh || mesh->faceGroups.empty() || mesh->faceGroups.size() != (size_t)mesh->nbFaces) return;
    if (mesh->vrfStartCount.empty() || mesh->vertRingFace.empty()) return;

    const size_t total = pickedVertices.size();
    if (total == 0) return;
    const int maxThreads = omp_get_max_threads();
    const bool inParallel = omp_in_parallel();

    auto isKeep = [&](uint32_t v) -> bool {
        if (v >= (uint32_t)mesh->nbVerts) return false;
        uint32_t start = mesh->vrfStartCount[v * 2];
        uint32_t count = mesh->vrfStartCount[v * 2 + 1];
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t f = mesh->vertRingFace[start + i];
            if (f < (uint32_t)mesh->nbFaces && mesh->faceGroups[f] == targetGroupId) {
                return true;
            }
        }
        return false;
    };

    if (total < 4096 || inParallel || maxThreads <= 1) {
        pickedVertices.erase(
            std::remove_if(pickedVertices.begin(), pickedVertices.end(), [&](uint32_t v) {
                return !isKeep(v);
            }),
            pickedVertices.end()
        );
        return;
    }

    std::vector<std::vector<uint32_t>> threadBuckets(maxThreads);
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& local = threadBuckets[tid];
        local.reserve((total / maxThreads) + 64);
        #pragma omp for schedule(static)
        for (int64_t i = 0; i < (int64_t)total; ++i) {
            uint32_t vid = pickedVertices[i];
            if (isKeep(vid)) {
                local.push_back(vid);
            }
        }
    }
    size_t offsets[129] = {0};
    int numMergeThreads = std::min(maxThreads, 128);
    for (int t = 0; t < numMergeThreads; ++t) {
        offsets[t + 1] = offsets[t] + threadBuckets[t].size();
    }
    pickedVertices.resize(offsets[numMergeThreads]);
    #pragma omp parallel for schedule(static) if(offsets[numMergeThreads] > 8192)
    for (int t = 0; t < numMergeThreads; ++t) {
        size_t count = threadBuckets[t].size();
        if (count > 0) {
            std::memcpy(pickedVertices.data() + offsets[t], threadBuckets[t].data(), count * sizeof(uint32_t));
        }
    }
}



