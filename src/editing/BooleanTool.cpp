#define GLM_ENABLE_EXPERIMENTAL
#include "editing/BooleanTool.h"
#include "editing/manifold/ManifoldBridge.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "gui/GuiManager.h"
#include "common/Logger.h"

#include <chrono>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace sculpt {

bool BooleanTool::execute(
    Scene& scene,
    Mesh* meshA,
    Mesh* meshB,
    BooleanOp op
) {
    if (!meshA || !meshB || meshA == meshB) {
        sculpt_log("[BOOLEAN] Invalid inputs: meshA=%p, meshB=%p\n", (void*)meshA, (void*)meshB);
        return false;
    }
    return executeMultiple(scene, meshA, {meshB}, op);
}

bool BooleanTool::executeMultiple(
    Scene& scene,
    Mesh* targetMesh,
    const std::vector<Mesh*>& otherMeshes,
    BooleanOp op
) {
    if (!targetMesh) {
        sculpt_log("[BOOLEAN] Invalid targetMesh: null\n");
        return false;
    }

    std::vector<Mesh*> validOthers;
    for (Mesh* m : otherMeshes) {
        if (!m || m == targetMesh) continue;
        if (std::find(validOthers.begin(), validOthers.end(), m) != validOthers.end()) continue;
        validOthers.push_back(m);
    }

    if (validOthers.empty()) {
        sculpt_log("[BOOLEAN] No valid operand meshes specified.\n");
        return false;
    }

    if (targetMesh->nbVerts == 0) {
        sculpt_log("[BOOLEAN] Target mesh '%s' is empty.\n", targetMesh->outlinerName.c_str());
        GuiManager::showToast("Boolean error: Base mesh is empty", 3.0f);
        return false;
    }

    for (Mesh* m : validOthers) {
        if (m->nbVerts == 0) {
            sculpt_log("[BOOLEAN] Operand mesh '%s' is empty.\n", m->outlinerName.c_str());
            char buf[128];
            snprintf(buf, sizeof(buf), "Boolean error: Mesh '%s' is empty", m->outlinerName.c_str());
            GuiManager::showToast(buf, 3.0f);
            return false;
        }
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    const char* opName = (op == BooleanOp::Union) ? "Union" :
                         (op == BooleanOp::Difference) ? "Difference" : "Intersection";

    sculpt_log("[BOOLEAN] Starting %s: Target '%s' (%d verts, %d faces) with %zu operand meshes...\n",
               opName, targetMesh->outlinerName.c_str(), targetMesh->nbVerts, targetMesh->nbFaces,
               validOthers.size());

    // 1. Convert target mesh to Manifold
    uint32_t targetOrigId = manifold::Manifold::ReserveIDs(1);
    manifold::Manifold targetManifold = ManifoldBridge::fromMesh(*targetMesh, targetOrigId);

    if (targetManifold.IsEmpty() || targetManifold.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[BOOLEAN] Failed to convert target mesh '%s' to valid Manifold. Status: %d\n",
                   targetMesh->outlinerName.c_str(), (int)targetManifold.Status());
        char buf[128];
        snprintf(buf, sizeof(buf), "Boolean error: Base mesh '%s' is not valid 2-manifold", targetMesh->outlinerName.c_str());
        GuiManager::showToast(buf, 3.0f);
        return false;
    }

    std::vector<manifold::Manifold> allManifolds;
    allManifolds.push_back(targetManifold);

    std::vector<const Mesh*> sourceMeshesList;
    sourceMeshesList.push_back(targetMesh);

    std::vector<uint32_t> sourceOrigIdsList;
    sourceOrigIdsList.push_back(targetOrigId);

    glm::mat4 invTargetMatrix = glm::inverse(targetMesh->matrix);

    // 2. Convert and transform all operand meshes to targetMesh local space
    for (Mesh* om : validOthers) {
        uint32_t omId = manifold::Manifold::ReserveIDs(1);
        manifold::Manifold m = ManifoldBridge::fromMesh(*om, omId);
        if (m.IsEmpty() || m.Status() != manifold::Manifold::Error::NoError) {
            sculpt_log("[BOOLEAN] Failed to convert operand mesh '%s' to valid Manifold. Status: %d\n",
                       om->outlinerName.c_str(), (int)m.Status());
            char errBuf[160];
            snprintf(errBuf, sizeof(errBuf), "Boolean error: Mesh '%s' is not valid 2-manifold", om->outlinerName.c_str());
            GuiManager::showToast(errBuf, 3.0f);
            return false;
        }

        glm::mat4 m_to_target = invTargetMatrix * om->matrix;
        glm::mat4x3 affine(m_to_target);
        m = m.Transform(affine);

        allManifolds.push_back(m);
        sourceMeshesList.push_back(om);
        sourceOrigIdsList.push_back(omId);
    }

    // 3. Perform CSG Boolean with triangle cleanup disabled to preserve non-intersecting quads
    bool prevCleanup = manifold::ManifoldParams().cleanupTriangles;
    manifold::ManifoldParams().cleanupTriangles = false;

    manifold::OpType manifoldOp = (op == BooleanOp::Union) ? manifold::OpType::Add :
                                  (op == BooleanOp::Difference) ? manifold::OpType::Subtract :
                                  manifold::OpType::Intersect;

    manifold::Manifold result = manifold::Manifold::BatchBoolean(allManifolds, manifoldOp);

    manifold::ManifoldParams().cleanupTriangles = prevCleanup;

    if (result.IsEmpty()) {
        sculpt_log("[BOOLEAN] CSG operation resulted in an empty mesh.\n");
        GuiManager::showToast("Boolean result is empty (no volume left)", 3.0f);
        return false;
    }

    // 4. Save history state before applying changes to scene
    scene.pushHistoryState();

    // 5. Update targetMesh with result, restoring original quads everywhere except intersection seam
    ManifoldBridge::toMeshBooleanMultiple(result, *targetMesh, sourceMeshesList, sourceOrigIdsList, true);

    // Boolean result is a standard sculpted mesh; clear procedural flags
    targetMesh->isTube = false;
    targetMesh->tubeData = nullptr;
    targetMesh->isPrimitive = false;
    targetMesh->primitiveData = nullptr;

    // 6. Remove operand meshes from scene
    for (Mesh* om : validOthers) {
        scene.removeMesh(om);
    }

    // Ensure targetMesh remains properly selected and active
    scene.setOrUnsetMesh(nullptr, false);
    scene.setOrUnsetMesh(targetMesh, false);
    scene.setModified(true);

    // Invalidate thumbnail for targetMesh so Outliner regenerates the preview immediately!
    GuiManager::invalidateThumbnail(targetMesh->getID());

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[BOOLEAN] %s on %zu meshes completed in %.2f ms. Output 2-Manifold (%d verts, %d faces, status: %d)\n",
               opName, allManifolds.size(), ms, targetMesh->nbVerts, targetMesh->nbFaces, (int)result.Status());

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Boolean] %s (%zu meshes) in %.1fms (%d faces)",
             opName, allManifolds.size(), ms, targetMesh->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

} // namespace sculpt
