#define GLM_ENABLE_EXPERIMENTAL
#include "editing/SplitTool.h"
#include "editing/manifold/ManifoldBridge.h"
#include "editing/manifold/LassoCutter.h"
#include "editing/manifold/QuadCapGenerator.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "gui/GuiManager.h"
#include "common/Logger.h"
#include "common/Constants.h"

#include <chrono>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace sculpt {

static float computeAverageEdgeLength(const Mesh* mesh) {
    if (!mesh || mesh->nbFaces == 0) return 1.0f;

    float sumEdgeLen = 0.0f;
    int edgeSampleCount = 0;
    const int faceStep = std::max(1, mesh->nbFaces / 1000);
    for (int f = 0; f < mesh->nbFaces; f += faceStep) {
        uint32_t v0 = mesh->faces[f * 4 + 0];
        uint32_t v1 = mesh->faces[f * 4 + 1];
        uint32_t v2 = mesh->faces[f * 4 + 2];
        uint32_t v3 = mesh->faces[f * 4 + 3];
        if (v0 >= (uint32_t)mesh->nbVerts || v1 >= (uint32_t)mesh->nbVerts || v2 >= (uint32_t)mesh->nbVerts) continue;
        glm::vec3 p0(mesh->verts[v0 * 3], mesh->verts[v0 * 3 + 1], mesh->verts[v0 * 3 + 2]);
        glm::vec3 p1(mesh->verts[v1 * 3], mesh->verts[v1 * 3 + 1], mesh->verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh->verts[v2 * 3], mesh->verts[v2 * 3 + 1], mesh->verts[v2 * 3 + 2]);
        sumEdgeLen += glm::distance(p0, p1);
        sumEdgeLen += glm::distance(p1, p2);
        edgeSampleCount += 2;
        if (v3 != TRI_INDEX && v3 < (uint32_t)mesh->nbVerts) {
            glm::vec3 p3(mesh->verts[v3 * 3], mesh->verts[v3 * 3 + 1], mesh->verts[v3 * 3 + 2]);
            sumEdgeLen += glm::distance(p2, p3);
            sumEdgeLen += glm::distance(p3, p0);
            edgeSampleCount += 2;
        } else {
            sumEdgeLen += glm::distance(p2, p0);
            edgeSampleCount += 1;
        }
    }
    return (edgeSampleCount > 0) ? (sumEdgeLen / edgeSampleCount) : 1.0f;
}

static void copyMeshMetadata(const Mesh* src, Mesh* dst, const std::string& suffix) {
    if (!src || !dst) return;
    dst->matrix = src->matrix;
    dst->editMatrix = src->editMatrix;
    dst->scale = src->scale;
    dst->center = src->center;
    dst->visibleV1 = src->visibleV1;
    dst->visibleV2 = src->visibleV2;
    dst->symEnabled = src->symEnabled;
    dst->symX = src->symX;
    dst->symY = src->symY;
    dst->symZ = src->symZ;
    dst->symMode = src->symMode;
    dst->hasCustomNormals = false;
    dst->outlinerName = src->outlinerName + suffix;
}

bool SplitTool::sliceByLine(
    Scene& scene,
    Mesh* mesh,
    const Camera& camera,
    const glm::vec2& lineStart,
    const glm::vec2& lineEnd,
    bool useSym,
    SymmetryMode symMode,
    const std::vector<glm::vec3>& symScales
) {
    if (!mesh || mesh->nbVerts == 0) return false;
    if (glm::distance(lineStart, lineEnd) < 5.0f) return false;

    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[SPLIT CSG] Starting Slice By Line on '%s' (%d verts, %d faces)...\n",
               mesh->outlinerName.c_str(), mesh->nbVerts, mesh->nbFaces);

    // 1. Compute 3D cutting plane from screen line
    Ray rStart = camera.getRay(lineStart.x, lineStart.y);
    Ray rEnd   = camera.getRay(lineEnd.x, lineEnd.y);

    glm::vec3 vA = rStart.origin + rStart.dir * 100.0f;
    glm::vec3 vB = rEnd.origin + rEnd.dir * 100.0f;
    glm::vec3 vC = rStart.origin;

    glm::vec3 nWorld = glm::cross(vB - vA, vC - vA);
    float len = glm::length(nWorld);
    if (len < 1e-6f) return false;
    nWorld /= len;

    // 2. Transform plane into mesh local space
    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
    glm::vec3 pLocal = glm::vec3(invMeshMatrix * glm::vec4(vA, 1.0f));
    glm::vec3 nLocal = glm::vec3(glm::transpose(mesh->matrix) * glm::vec4(nWorld, 0.0f));
    float nLocalLen = glm::length(nLocal);
    if (nLocalLen < 1e-6f) return false;
    nLocal /= nLocalLen;

    float originOffset = glm::dot(pLocal, nLocal);

    // 3. Convert target mesh to Manifold
    manifold::Manifold target = ManifoldBridge::fromMesh(*mesh);
    if (target.IsEmpty() || target.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[SPLIT CSG] Failed to convert mesh to valid Manifold. Status: %d\n", (int)target.Status());
        return false;
    }

    float targetEdgeLen = computeAverageEdgeLength(mesh);

    // 4. Split By Half-space Cutter marked with AsOriginal()
    manifold::Box bBox = target.BoundingBox();
    glm::vec3 n = glm::normalize(nLocal);
    manifold::Manifold cutter =
        manifold::Manifold::Cube(glm::vec3(2.0f), true).Translate({1.0f, 0.0f, 0.0f});
    float size = glm::length(bBox.Center() - n * originOffset) +
                 0.5f * glm::length(bBox.Size());
    cutter = cutter.Scale(glm::vec3(size)).Translate({originOffset, 0.0f, 0.0f});
    float yDeg = glm::degrees(-glm::asin(n.z));
    float zDeg = glm::degrees(glm::atan(n.y, n.x));
    cutter = cutter.Rotate(0.0f, yDeg, zDeg);

    if (useSym && !symScales.empty()) {
        std::vector<manifold::Manifold> symCutters;
        symCutters.push_back(cutter);
        for (const auto& s : symScales) {
            if (s.x < 0.0f) symCutters.push_back(cutter.Mirror(glm::vec3(1.0f, 0.0f, 0.0f)));
            if (s.y < 0.0f) symCutters.push_back(cutter.Mirror(glm::vec3(0.0f, 1.0f, 0.0f)));
            if (s.z < 0.0f) symCutters.push_back(cutter.Mirror(glm::vec3(0.0f, 0.0f, 1.0f)));
        }
        if (symCutters.size() > 1) {
            cutter = manifold::Manifold::BatchBoolean(symCutters, manifold::OpType::Add);
        }
    }

    cutter = cutter.AsOriginal();
    int cutterOrigID = cutter.OriginalID();

    auto [part1, part2] = target.Split(cutter);
    if (part1.IsEmpty() || part2.IsEmpty()) {
        sculpt_log("[SPLIT CSG] Split missed geometry. Part1 empty: %d, Part2 empty: %d\n",
                   part1.IsEmpty() ? 1 : 0, part2.IsEmpty() ? 1 : 0);
        GuiManager::showToast("[Split] Slice plane did not divide the mesh", 2.5f);
        return false;
    }

    uint32_t gid1 = scene.getNextFreePolyGroupID();
    uint32_t gid2 = gid1 + 1;

    // 5. Update original mesh with Part 1 (runPostInit=false, QuadCap will postInit)
    mesh->hasCustomNormals = false;
    ManifoldBridge::toMesh(part1, *mesh, false, cutterOrigID, gid1);
    QuadCapGenerator::generateQuadCap(*mesh, gid1, targetEdgeLen);

    // 6. Create second subtool with Part 2 (runPostInit=false, QuadCap will postInit)
    Mesh* cutMesh = new Mesh();
    copyMeshMetadata(mesh, cutMesh, "_Cut");
    ManifoldBridge::toMesh(part2, *cutMesh, false, cutterOrigID, gid2);
    QuadCapGenerator::generateQuadCap(*cutMesh, gid2, targetEdgeLen);

    scene.addMesh(cutMesh);
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[SPLIT CSG] Slice completed in %.2f ms. Part 1 (%d verts, %d faces), Part 2 (%d verts, %d faces)\n",
               totalMs, mesh->nbVerts, mesh->nbFaces, cutMesh->nbVerts, cutMesh->nbFaces);

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Split] Created '%s' in %.0fms (%d & %d faces)",
             cutMesh->outlinerName.c_str(), totalMs, mesh->nbFaces, cutMesh->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

bool SplitTool::splitByLasso(
    Scene& scene,
    Mesh* mesh,
    const Camera& camera,
    const std::vector<glm::vec2>& lassoPoints,
    bool isAlt,
    bool useSym,
    SymmetryMode symMode,
    const std::vector<glm::vec3>& symScales
) {
    if (!mesh || mesh->nbVerts == 0 || lassoPoints.size() < 3) return false;

    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[SPLIT CSG] Starting Split By Lasso on '%s' (%d verts, %d faces, alt=%d, sym=%d)...\n",
               mesh->outlinerName.c_str(), mesh->nbVerts, mesh->nbFaces, isAlt ? 1 : 0, useSym ? 1 : 0);

    float bbox[6] = {0};
    mesh->computeBbox(bbox);
    glm::vec3 bMin(bbox[0], bbox[1], bbox[2]);
    glm::vec3 bMax(bbox[3], bbox[4], bbox[5]);

    float targetEdgeLen = computeAverageEdgeLength(mesh);
    sculpt_log("[SPLIT CSG] Mesh BBox: [%.2f, %.2f, %.2f] to [%.2f, %.2f, %.2f], targetEdgeLen: %.3f mm\n",
               bMin.x, bMin.y, bMin.z, bMax.x, bMax.y, bMax.z, targetEdgeLen);

    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
    manifold::Manifold cutter = LassoCutter::build3DCutterPrism(
        lassoPoints,
        camera,
        invMeshMatrix,
        0,
        bMin,
        bMax,
        targetEdgeLen
    );
    if (cutter.IsEmpty() || cutter.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[SPLIT CSG] Failed to build valid cutter prism. Status: %d\n", (int)cutter.Status());
        return false;
    }

    // Mirror cutter prism across symmetry axes if symmetry is enabled
    if (useSym && !symScales.empty()) {
        std::vector<manifold::Manifold> symCutters;
        symCutters.push_back(cutter);

        for (const auto& s : symScales) {
            if (s.x < 0.0f) {
                symCutters.push_back(cutter.Mirror(glm::vec3(1.0f, 0.0f, 0.0f)));
            }
            if (s.y < 0.0f) {
                symCutters.push_back(cutter.Mirror(glm::vec3(0.0f, 1.0f, 0.0f)));
            }
            if (s.z < 0.0f) {
                symCutters.push_back(cutter.Mirror(glm::vec3(0.0f, 0.0f, 1.0f)));
            }
        }

        if (symCutters.size() > 1) {
            cutter = manifold::Manifold::BatchBoolean(symCutters, manifold::OpType::Add);
            sculpt_log("[SPLIT CSG] Applied symmetry: combined %zu cutter prisms.\n", symCutters.size());
        }
    }

    cutter = cutter.AsOriginal();
    int cutterOrigID = cutter.OriginalID();

    manifold::Manifold target = ManifoldBridge::fromMesh(*mesh);
    if (target.IsEmpty() || target.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[SPLIT CSG] Failed to convert mesh to valid Manifold. Status: %d\n", (int)target.Status());
        return false;
    }

    auto [inside, outside] = target.Split(cutter);
    if (inside.IsEmpty() || outside.IsEmpty()) {
        sculpt_log("[SPLIT CSG] Split missed geometry. Inside empty: %d, Outside empty: %d\n",
                   inside.IsEmpty() ? 1 : 0, outside.IsEmpty() ? 1 : 0);
        GuiManager::showToast("[Split] Lasso did not divide the mesh (missed or enclosed all)", 2.5f);
        return false;
    }

    // When isAlt is true: original mesh keeps inside, outside goes to new subtool.
    // Default (!isAlt): original mesh keeps outside (cut out hole), inside goes to new subtool.
    manifold::Manifold keepPart = isAlt ? inside : outside;
    manifold::Manifold cutPart  = isAlt ? outside : inside;

    uint32_t gid1 = scene.getNextFreePolyGroupID();
    uint32_t gid2 = gid1 + 1;

    // 1. Update original mesh with keepPart (runPostInit=false, QuadCap will postInit)
    mesh->hasCustomNormals = false;
    ManifoldBridge::toMesh(keepPart, *mesh, false, cutterOrigID, gid1);
    QuadCapGenerator::generateQuadCap(*mesh, gid1, targetEdgeLen);

    // 2. Create new subtool with cutPart (runPostInit=false, QuadCap will postInit)
    Mesh* cutMesh = new Mesh();
    copyMeshMetadata(mesh, cutMesh, "_Cut");
    ManifoldBridge::toMesh(cutPart, *cutMesh, false, cutterOrigID, gid2);
    QuadCapGenerator::generateQuadCap(*cutMesh, gid2, targetEdgeLen);

    scene.addMesh(cutMesh);
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[SPLIT CSG] Lasso Split completed in %.2f ms. Mesh (%d verts, %d faces), Cut (%d verts, %d faces)\n",
               totalMs, mesh->nbVerts, mesh->nbFaces, cutMesh->nbVerts, cutMesh->nbFaces);

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Split] Created '%s' in %.0fms (%d & %d faces)",
             cutMesh->outlinerName.c_str(), totalMs, mesh->nbFaces, cutMesh->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

} // namespace sculpt
