#define GLM_ENABLE_EXPERIMENTAL
#include "editing/TrimTool.h"
#include "editing/manifold/ManifoldBridge.h"
#include "editing/manifold/LassoCutter.h"
#include "editing/manifold/QuadCapGenerator.h"
#include "gui/GuiManager.h"
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "common/Logger.h"
#include "common/Constants.h"
#include <chrono>
#include <vector>
#include <string>
#include <glm/gtc/matrix_transform.hpp>

bool TrimTool::execute(
    Mesh* mesh,
    const Camera& camera,
    const std::vector<glm::vec2>& lassoPoints,
    bool isAlt,
    bool useSym,
    SymmetryMode symMode,
    const std::vector<glm::vec3>& symScales,
    const TrimConfig& config
) {
    if (!mesh || mesh->nbVerts == 0 || lassoPoints.size() < 3) {
        sculpt_log("[TrimTool] Invalid input: mesh=%p, lassoPoints=%zu\n", (void*)mesh, lassoPoints.size());
        return false;
    }

    auto tTotalStart = std::chrono::high_resolution_clock::now();

    const int inVerts = mesh->nbVerts;
    const int inFaces = mesh->nbFaces;

    sculpt_log("[MANIFOLD CSG] Start Operation: %s on Mesh (verts: %d, faces: %d, lasso: %zu pts, sym: %d)\n",
               isAlt ? "TRIM_KEEP_INSIDE" : "TRIM_CUT_OUT", inVerts, inFaces, lassoPoints.size(), useSym ? 1 : 0);

    // 1. Allocate a new unique PolyGroup ID for cut cap faces
    uint32_t newCapGid = mesh->getNextFreeGroupID();
    if (newCapGid == 0) newCapGid = 1;

    // 2. Convert source mesh to Manifold
    auto tFromMeshStart = std::chrono::high_resolution_clock::now();
    manifold::Manifold target = sculpt::ManifoldBridge::fromMesh(*mesh);
    auto tFromMeshEnd = std::chrono::high_resolution_clock::now();
    double fromMeshMs = std::chrono::duration<double, std::milli>(tFromMeshEnd - tFromMeshStart).count();

    if (target.IsEmpty() || target.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[TrimTool] Failed to convert mesh to valid Manifold. Status: %d\n", (int)target.Status());
        return false;
    }

    // 3. Compute local bounding box and average edge length for regular cutter grid
    float bbox[6] = {0};
    mesh->computeBbox(bbox);
    glm::vec3 bMin(bbox[0], bbox[1], bbox[2]);
    glm::vec3 bMax(bbox[3], bbox[4], bbox[5]);

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
    float targetEdgeLen = (edgeSampleCount > 0) ? (sumEdgeLen / edgeSampleCount) : 1.0f;
    sculpt_log("[TrimTool] Mesh BBox: [%.2f, %.2f, %.2f] to [%.2f, %.2f, %.2f] (size: [%.2f, %.2f, %.2f]), targetEdgeLen: %.3f mm\n",
               bMin.x, bMin.y, bMin.z, bMax.x, bMax.y, bMax.z,
               bMax.x - bMin.x, bMax.y - bMin.y, bMax.z - bMin.z, targetEdgeLen);

    // 4. Build 3D cutter prism in mesh local space with regular quad grid
    auto tCutterStart = std::chrono::high_resolution_clock::now();
    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
    manifold::Manifold cutter = sculpt::LassoCutter::build3DCutterPrism(
        lassoPoints,
        camera,
        invMeshMatrix,
        newCapGid,
        bMin,
        bMax,
        targetEdgeLen
    );

    if (cutter.IsEmpty() || cutter.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[TrimTool] Failed to build valid 3D cutter prism. Status: %d\n", (int)cutter.Status());
        return false;
    }

    // 5. Handle symmetry (mirror cutter prism across symmetry axes)
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
            sculpt_log("[TrimTool] Applied symmetry: combined %zu cutter prisms.\n", symCutters.size());
        }
    }

    cutter = cutter.AsOriginal();
    int cutterOrigID = cutter.OriginalID();
    auto tCutterEnd = std::chrono::high_resolution_clock::now();
    double cutterMs = std::chrono::duration<double, std::milli>(tCutterEnd - tCutterStart).count();

    // 5. Perform Boolean operation
    auto tCsgStart = std::chrono::high_resolution_clock::now();
    manifold::Manifold result = isAlt ? (target ^ cutter) : (target - cutter);
    auto tCsgEnd = std::chrono::high_resolution_clock::now();
    double csgMs = std::chrono::duration<double, std::milli>(tCsgEnd - tCsgStart).count();

    if (result.IsEmpty() || result.NumVert() == 0) {
        sculpt_log("[TrimTool] CSG result is empty. Mesh not modified.\n");
        return false;
    }

    const int outVerts = result.NumVert();
    const int outTris = result.NumTri();

    sculpt_log("[MANIFOLD CSG] Boolean %s completed in %.2f ms: %d -> %d verts, %d -> %d tris (status: %d)\n",
               isAlt ? "Intersection (Alt)" : "Difference", csgMs, inVerts, outVerts, inFaces, outTris, (int)result.Status());

    // 6. Convert Manifold result back to Mesh (runPostInit=false to avoid redundant postInit before QuadCap)
    auto tToMeshStart = std::chrono::high_resolution_clock::now();
    mesh->hasCustomNormals = false;
    sculpt::ManifoldBridge::toMesh(result, *mesh, false, cutterOrigID, newCapGid);
    auto tToMeshEnd = std::chrono::high_resolution_clock::now();
    double toMeshMs = std::chrono::duration<double, std::milli>(tToMeshEnd - tToMeshStart).count();

    // 7. Generate Nomad Sculpt-style regular quad cap with CDT transition belt
    // Note: QuadCapGenerator performs a single complete postInit on the final topology
    auto tQuadCapStart = std::chrono::high_resolution_clock::now();
    sculpt::QuadCapResult capRes = sculpt::QuadCapGenerator::generateQuadCap(*mesh, newCapGid, targetEdgeLen);
    auto tQuadCapEnd = std::chrono::high_resolution_clock::now();
    double quadCapMs = std::chrono::duration<double, std::milli>(tQuadCapEnd - tQuadCapStart).count();

    auto tTotalEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();

    sculpt_log("[TRIM PROFILING] Total: %.2f ms | fromMesh: %.1fms | Cutter: %.1fms | CSG Boolean: %.1fms | toMesh: %.1fms | QuadCap+postInit: %.1fms\n",
               totalMs, fromMeshMs, cutterMs, csgMs, toMeshMs, quadCapMs);

    sculpt_log("[TRIM COMPLETE] Total Time: %.2f ms (in: %d tris, out: %d faces, Cap Group ID: %u)\n",
               totalMs, inFaces, mesh->nbFaces, newCapGid);

    // 8. Viewport HUD Toast notification (3s)
    char toastBuf[160];
    if (capRes.success && (capRes.numQuads > 0 || capRes.numTris > 0)) {
        snprintf(toastBuf, sizeof(toastBuf), "[Trim] %d -> %d faces (Cut: %.0fms, Cap: %.0fms) | Cap: %.1f%% Quads (%d quads, %d tris)",
                 inFaces, mesh->nbFaces, csgMs, capRes.executionTimeMs,
                 capRes.quadRatio * 100.0f, capRes.numQuads, capRes.numTris);
    } else {
        snprintf(toastBuf, sizeof(toastBuf), "[Trim] %d -> %d faces in %.1fms | Cap ID: %u",
                 inFaces, mesh->nbFaces, totalMs, newCapGid);
    }
    GuiManager::showToast(toastBuf, 3.0f);
    GuiManager::invalidateThumbnail(mesh->getID());

    return true;
}
