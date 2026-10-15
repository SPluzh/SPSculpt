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

#include <chrono>
#include <glm/gtc/matrix_transform.hpp>

namespace sculpt {

bool SplitTool::sliceByLine(
    Scene& scene,
    Mesh* mesh,
    const Camera& camera,
    const glm::vec2& lineStart,
    const glm::vec2& lineEnd
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
    cutter = cutter.Rotate(0.0f, yDeg, zDeg).AsOriginal();
    int cutterOrigID = cutter.OriginalID();

    auto [part1, part2] = target.Split(cutter);
    if (part1.IsEmpty() || part2.IsEmpty()) {
        sculpt_log("[SPLIT CSG] Split missed geometry. Part1 empty: %d, Part2 empty: %d\n",
                   part1.IsEmpty() ? 1 : 0, part2.IsEmpty() ? 1 : 0);
        return false;
    }

    uint32_t gid1 = mesh->getNextFreeGroupID();
    uint32_t gid2 = gid1 + 1;

    // 5. Update original mesh with Part 1
    ManifoldBridge::toMesh(part1, *mesh, true, cutterOrigID, gid1);
    QuadCapGenerator::generateQuadCap(*mesh, gid1);

    // 6. Create second subtool with Part 2
    Mesh* cutMesh = new Mesh();
    cutMesh->matrix = mesh->matrix;
    cutMesh->outlinerName = mesh->outlinerName + "_Cut";
    ManifoldBridge::toMesh(part2, *cutMesh, true, cutterOrigID, gid2);
    QuadCapGenerator::generateQuadCap(*cutMesh, gid2);

    scene.addMesh(cutMesh);
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[SPLIT CSG] Slice completed in %.2f ms. Part 1 (%d verts, %d faces), Part 2 (%d verts, %d faces)\n",
               totalMs, mesh->nbVerts, mesh->nbFaces, cutMesh->nbVerts, cutMesh->nbFaces);

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Slice] Created '%s' in %.0fms (%d & %d faces)",
             cutMesh->outlinerName.c_str(), totalMs, mesh->nbFaces, cutMesh->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

bool SplitTool::splitByLasso(
    Scene& scene,
    Mesh* mesh,
    const Camera& camera,
    const std::vector<glm::vec2>& lassoPoints
) {
    if (!mesh || mesh->nbVerts == 0 || lassoPoints.size() < 3) return false;

    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[SPLIT CSG] Starting Split By Lasso on '%s' (%d verts, %d faces)...\n",
               mesh->outlinerName.c_str(), mesh->nbVerts, mesh->nbFaces);

    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
    manifold::Manifold cutter = LassoCutter::build3DCutterPrism(lassoPoints, camera, invMeshMatrix);
    if (cutter.IsEmpty() || cutter.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[SPLIT CSG] Failed to build valid cutter prism. Status: %d\n", (int)cutter.Status());
        return false;
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
        return false;
    }

    uint32_t gid1 = mesh->getNextFreeGroupID();
    uint32_t gid2 = gid1 + 1;

    // Outside stays in mesh
    ManifoldBridge::toMesh(outside, *mesh, true, cutterOrigID, gid1);
    QuadCapGenerator::generateQuadCap(*mesh, gid1);

    // Inside goes into new subtool
    Mesh* cutMesh = new Mesh();
    cutMesh->matrix = mesh->matrix;
    cutMesh->outlinerName = mesh->outlinerName + "_Cut";
    ManifoldBridge::toMesh(inside, *cutMesh, true, cutterOrigID, gid2);
    QuadCapGenerator::generateQuadCap(*cutMesh, gid2);

    scene.addMesh(cutMesh);
    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[SPLIT CSG] Lasso Split completed in %.2f ms. Outside (%d verts, %d faces), Inside (%d verts, %d faces)\n",
               totalMs, mesh->nbVerts, mesh->nbFaces, cutMesh->nbVerts, cutMesh->nbFaces);

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Split] Created '%s' in %.0fms (%d & %d faces)",
             cutMesh->outlinerName.c_str(), totalMs, mesh->nbFaces, cutMesh->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

} // namespace sculpt
