#define GLM_ENABLE_EXPERIMENTAL
#include "editing/BooleanTool.h"
#include "editing/manifold/ManifoldBridge.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "gui/GuiManager.h"
#include "common/Logger.h"

#include <chrono>
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

    if (meshA->nbVerts == 0 || meshB->nbVerts == 0) {
        sculpt_log("[BOOLEAN] One or both meshes are empty.\n");
        return false;
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    const char* opName = (op == BooleanOp::Union) ? "Union" :
                         (op == BooleanOp::Difference) ? "Difference" : "Intersection";

    sculpt_log("[BOOLEAN] Starting %s: '%s' (%d verts, %d faces) with '%s' (%d verts, %d faces)...\n",
               opName, meshA->outlinerName.c_str(), meshA->nbVerts, meshA->nbFaces,
               meshB->outlinerName.c_str(), meshB->nbVerts, meshB->nbFaces);

    // 1. Convert meshA and meshB to Manifold
    manifold::Manifold mA = ManifoldBridge::fromMesh(*meshA);
    manifold::Manifold mB = ManifoldBridge::fromMesh(*meshB);

    if (mA.IsEmpty() || mA.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[BOOLEAN] Failed to convert meshA to valid Manifold. Status: %d\n", (int)mA.Status());
        return false;
    }
    if (mB.IsEmpty() || mB.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[BOOLEAN] Failed to convert meshB to valid Manifold. Status: %d\n", (int)mB.Status());
        return false;
    }

    // 2. Transform mB into meshA local space
    glm::mat4 mB_to_A = glm::inverse(meshA->matrix) * meshB->matrix;
    glm::mat4x3 affine(mB_to_A);
    mB = mB.Transform(affine);

    // 3. Mark mB as original to track cut cap / boundary faces
    mB = mB.AsOriginal();
    int mBOrigID = mB.OriginalID();
    uint32_t newCapGid = meshA->getNextFreeGroupID();

    // 4. Perform CSG Boolean
    manifold::Manifold result;
    switch (op) {
        case BooleanOp::Union:
            result = mA + mB;
            break;
        case BooleanOp::Difference:
            result = mA - mB;
            break;
        case BooleanOp::Intersection:
            result = mA ^ mB;
            break;
    }

    if (result.IsEmpty()) {
        sculpt_log("[BOOLEAN] CSG operation resulted in an empty mesh.\n");
        return false;
    }

    // 5. Save history state before applying changes to scene
    scene.pushHistoryState();

    // 6. Update meshA with result
    ManifoldBridge::toMesh(result, *meshA, true, mBOrigID, newCapGid);

    // 7. Remove meshB from scene (removeMesh handles deletion internally)
    scene.removeMesh(meshB);

    scene.setModified(true);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[BOOLEAN] %s completed in %.2f ms. Output 2-Manifold valid (%d verts, %d faces, status: %d)\n",
               opName, ms, meshA->nbVerts, meshA->nbFaces, (int)result.Status());

    char toastBuf[160];
    snprintf(toastBuf, sizeof(toastBuf), "[Boolean] %s done in %.1fms (%d faces)",
             opName, ms, meshA->nbFaces);
    GuiManager::showToast(toastBuf, 3.0f);

    return true;
}

} // namespace sculpt
