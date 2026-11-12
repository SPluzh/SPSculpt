#pragma once

#include "common/Symmetry.h"
#include "common/IniFile.h"
#include "common/FormatConstants.h"
#include "common/Enums.h"
#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/TrimTool.h"
#include "editing/SplitTool.h"
#include "editing/undo/UndoManager.h"
#include "editing/undo/UndoSerializer.h"
#include "editing/undo/SculptUndoEntry.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "editing/undo/DynamicTopologyUndoEntry.h"
#include "files/ExportSGL.h"
#include "files/ImportSGL.h"
#include "render/AngleRenderer.h"

#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

class RadialSymmetryTest {
public:
    static std::unique_ptr<Mesh> createUVSphere(float radius, int rings, int sectors) {
        auto mesh = std::make_unique<Mesh>();
        mesh->verts.reserve((rings + 1) * (sectors + 1) * 3);
        mesh->faces.reserve(rings * sectors * 4);

        for (int r = 0; r <= rings; ++r) {
            float phi = float(M_PI) * float(r) / float(rings);
            for (int s = 0; s <= sectors; ++s) {
                float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
                float x = radius * std::sin(phi) * std::cos(theta);
                float y = radius * std::cos(phi);
                float z = radius * std::sin(phi) * std::sin(theta);
                mesh->verts.push_back(x);
                mesh->verts.push_back(y);
                mesh->verts.push_back(z);
                mesh->nbVerts++;
            }
        }

        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < sectors; ++s) {
                uint32_t v0 = r * (sectors + 1) + s;
                uint32_t v1 = v0 + 1;
                uint32_t v2 = (r + 1) * (sectors + 1) + s + 1;
                uint32_t v3 = (r + 1) * (sectors + 1) + s;
                mesh->faces.push_back(v0);
                mesh->faces.push_back(v1);
                mesh->faces.push_back(v2);
                mesh->faces.push_back(v3);
                mesh->nbFaces++;
            }
        }

        mesh->vertVisible.assign(mesh->nbVerts, 1);
        mesh->faceVisible.assign(mesh->nbFaces, 1);
        return mesh;
    }

    static bool runAllTests() {
        std::cout << "\n======================================================================\n";
        std::cout << "               RADIAL SYMMETRY TEST & BENCHMARK SUITE\n";
        std::cout << "======================================================================\n\n";

        int passed = 0;
        const int total = 7;

        auto runCase = [&](int idx, const char* name, bool (*fn)()) {
            bool ok = false;
            try {
                ok = fn();
            } catch (const std::exception& e) {
                std::cerr << "  [Exception] " << name << ": " << e.what() << "\n";
                ok = false;
            }
            if (ok) {
                std::cout << "[PASS] Test " << idx << "/" << total << ": " << name << "\n";
                passed++;
            } else {
                std::cout << "[FAIL] Test " << idx << "/" << total << ": " << name << "\n";
            }
        };

        runCase(1, "Symmetry Math, Deduplication, Inverses & Local/World", &testSymmetryMathAndDedup);
        runCase(2, "Brush Stroke Radial Symmetry Audit & Pole Stability", &testBrushSymmetryAuditAndPole);
        runCase(3, "Undo / Redo & UndoSerializer v2 Roundtrip", &testUndoRedoAndSerializerV2);
        runCase(4, "Dynamic Topology + Radial 5 + Bit-for-Bit Undo", &testDynTopoRadialSymmetry);
        runCase(5, "TrimTool & SplitTool CSG with Radial Symmetry", &testTrimAndSplitRadial);
        runCase(6, "INI Settings & .spsculpt v21 Serialization Roundtrip", &testSettingsIniAndSglV21Persistence);
        runCase(7, "1M Vertex Mesh Radial 8 & Radial 32 Stroke Benchmark", &testPerformance1MRadial);

        std::cout << "\n----------------------------------------------------------------------\n";
        std::cout << "Summary: " << passed << " / " << total << " tests passed.\n";
        std::cout << "======================================================================\n\n";
        return passed == total;
    }

    // 1. Math & deduplication & inverses & Local/World transforms
    static bool testSymmetryMathAndDedup() {
        auto makeParams = [](bool sx, bool sy, bool sz, bool rad, int count, int axis, SymmetryMode mode = SymmetryMode::Local) {
            SymmetryParams p;
            p.useSym = true;
            p.symX = sx;
            p.symY = sy;
            p.symZ = sz;
            p.radialEnabled = rad;
            p.radialCount = count;
            p.radialAxis = axis;
            p.mode = mode;
            return p;
        };

        // Case A: Radial 8 without mirrors -> 7 secondary, 8 with identity
        {
            auto p = makeParams(false, false, false, true, 8, 1);
            auto sec = buildSymmetryCopies(p, nullptr, false);
            auto all = buildSymmetryCopies(p, nullptr, true);
            if (sec.size() != 7 || all.size() != 8) {
                std::cerr << "  [Error] Radial 8 counts wrong: sec=" << sec.size() << ", all=" << all.size() << "\n";
                return false;
            }
        }

        // Case B: Radial 8 + Mirror X -> 15 secondary
        {
            auto p = makeParams(true, false, false, true, 8, 1);
            auto sec = buildSymmetryCopies(p, nullptr, false);
            if (sec.size() != 15) {
                std::cerr << "  [Error] Radial 8 + Mirror X expected 15 secondary, got " << sec.size() << "\n";
                return false;
            }
        }

        // Case C: Mirror X + Radial 2 (axis Y) -> 3 secondary
        {
            auto p = makeParams(true, false, false, true, 2, 1);
            auto sec = buildSymmetryCopies(p, nullptr, false);
            if (sec.size() != 3) {
                std::cerr << "  [Error] Mirror X + Radial 2 expected 3 secondary, got " << sec.size() << "\n";
                return false;
            }
        }

        // Case D: Mirror X + Z + Radial 2 (axis Y) -> 180 deg rotation coincides with X*Z reflection -> 3 secondary (not 7)
        {
            auto p = makeParams(true, false, true, true, 2, 1);
            auto sec = buildSymmetryCopies(p, nullptr, false);
            if (sec.size() != 3) {
                std::cerr << "  [Error] Mirror X+Z + Radial 2 dedup failed, got " << sec.size() << "\n";
                return false;
            }
        }

        // Case E: Exact 90/180/270 deg matrices for Radial 4 (axis Y: u=2 (Z), v=0 (X))
        {
            auto p = makeParams(false, false, false, true, 4, 1);
            auto all = buildSymmetryCopies(p, nullptr, true);
            if (all.size() != 4) {
                std::cerr << "  [Error] Radial 4 expected 4 copies, got " << all.size() << "\n";
                return false;
            }
            for (const auto& c : all) {
                for (int col = 0; col < 3; ++col) {
                    for (int row = 0; row < 3; ++row) {
                        float v = c.m[col][row];
                        if (std::abs(v) > 1e-6f && std::abs(std::abs(v) - 1.0f) > 1e-6f) {
                            std::cerr << "  [Error] Radial 4 90-deg drift detected: m[" << col << "][" << row << "]=" << v << "\n";
                            return false;
                        }
                    }
                }
            }
        }

        // Case F: symmetryInverses roundtrip for odd & even N with & without mirror
        for (int n : { 5, 6, 7, 8 }) {
            for (bool mirrorX : { false, true }) {
                auto p = makeParams(mirrorX, false, false, true, n, 2); // axis Z
                auto copies = buildSymmetryCopies(p, nullptr, true);
                auto invIdx = symmetryInverses(copies);
                if (invIdx.size() != copies.size()) return false;
                const glm::vec3 sample(1.37f, -0.82f, 2.19f);
                for (size_t i = 0; i < copies.size(); ++i) {
                    glm::vec3 q = copies[i].point(sample);
                    glm::vec3 back = copies[invIdx[i]].point(q);
                    if (glm::distance(sample, back) > 1e-4f) {
                        std::cerr << "  [Error] Inverse roundtrip mismatch for N=" << n << ", mirrorX=" << mirrorX << "\n";
                        return false;
                    }
                }
            }
        }

        // Case G: Local vs World mode with rotated/translated mesh + symmetryOffset
        {
            Mesh testMesh;
            testMesh.verts = { 1.0f, 0.0f, 0.0f,  -1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, -1.0f, 0.0f };
            testMesh.nbVerts = 4;
            testMesh.scale = 1.0f;
            testMesh.symmetryOffset = 0.0f;
            testMesh.matrix = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f)) *
                              glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0.0f, 0.0f, 1.0f));

            auto pWorld = makeParams(false, false, false, true, 4, 1, SymmetryMode::World);
            auto worldCopies = buildSymmetryCopies(pWorld, &testMesh, false);
            if (worldCopies.size() != 3) return false;

            const glm::vec3 localP(0.5f, 0.25f, 0.0f);
            const glm::vec3 worldP = glm::vec3(testMesh.matrix * glm::vec4(localP, 1.0f));
            bool found90 = false;
            const glm::vec3 expectedWorld90(worldP.z, worldP.y, -worldP.x); // +90 or -90 around world Y
            const glm::vec3 expectedWorld270(-worldP.z, worldP.y, worldP.x);
            for (const auto& c : worldCopies) {
                glm::vec3 mappedWorld = glm::vec3(testMesh.matrix * glm::vec4(c.point(localP), 1.0f));
                if (glm::distance(mappedWorld, expectedWorld90) < 1e-4f || glm::distance(mappedWorld, expectedWorld270) < 1e-4f) {
                    found90 = true;
                    break;
                }
            }
            if (!found90) {
                std::cerr << "  [Error] World-space radial transform check failed\n";
                return false;
            }
        }

        std::cout << "  [Detail] Dedup, 90-deg exactness, inverses, Local/World checks OK\n";
        return true;
    }

    // Helper: run a complete sculpt stroke on a world-space surface point (via Screen Projection)
    static void runScreenStroke(SculptManager& sculpt, Scene& scene, Mesh* mesh, const glm::vec3& startWorld, const glm::vec3& endWorld, int steps = 5) {
        Camera& cam = scene.getCamera();
        const glm::vec3 s0 = cam.project(startWorld);
        const glm::vec3 s1 = cam.project(endWorld);

        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = static_cast<int>(std::round(s0.x));
        evDown.button.y = static_cast<int>(std::round(s0.y));
        sculpt.handleEvent(evDown, scene);
        sculpt.processFrame(scene);

        for (int i = 1; i <= steps; ++i) {
            float t = float(i) / float(steps);
            SDL_Event evMove;
            std::memset(&evMove, 0, sizeof(evMove));
            evMove.type = SDL_MOUSEMOTION;
            evMove.motion.state = SDL_BUTTON_LMASK;
            evMove.motion.x = static_cast<int>(std::round(s0.x + t * (s1.x - s0.x)));
            evMove.motion.y = static_cast<int>(std::round(s0.y + t * (s1.y - s0.y)));
            sculpt.handleEvent(evMove, scene);
            sculpt.processFrame(scene);
        }

        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = static_cast<int>(std::round(s1.x));
        evUp.button.y = static_cast<int>(std::round(s1.y));
        sculpt.handleEvent(evUp, scene);
        (void)mesh;
    }

    // 2. Symmetry audit across brushes + Pole stability test
    static bool testBrushSymmetryAuditAndPole() {
        const float R = 35.0f;
        const int rings = 48;
        const int sectors = 96; // Divisible by 8!

        auto makeSceneAndSphere = [&](Scene& scene, SculptManager& sculpt) -> Mesh* {
            auto ptr = createUVSphere(R, rings, sectors);
            Mesh* m = ptr.release();
            m->initFaceGroups();
            m->postInit(true);
            scene.addMesh(m);
            scene.selectMesh(m);

            Camera& cam = scene.getCamera();
            cam.onResize(1280, 720);
            cam.resetViewToMesh(m, false);
            cam.update(0.016f);

            sculpt.setRenderer(nullptr);
            sculpt.setUseSym(true);
            sculpt.setSymX(false);
            sculpt.setSymY(false);
            sculpt.setSymZ(false);
            sculpt.setSymRadialEnabled(true);
            sculpt.setSymRadialCount(8);
            sculpt.setSymRadialAxis(1);
            sculpt.setSymmetryMode(SymmetryMode::Local);
            g_undoManager.clear();
            return m;
        };

        const BrushType brushes[] = {
            BRUSH_CLAYBUILDUP,
            BRUSH_MOVE,
            BRUSH_SNAKEHOOK,
            BRUSH_SMOOTH,
            BRUSH_FLATTEN
        };

        for (BrushType brush : brushes) {
            Scene scene;
            SculptManager sculpt;
            Mesh* mesh = makeSceneAndSphere(scene, sculpt);

            if (brush == BRUSH_SMOOTH || brush == BRUSH_FLATTEN) {
                // First create asymmetric bump on all 8 sectors via ClayBuildup, or pre-sculpt radial ridges
                sculpt.setBrush(BRUSH_CLAYBUILDUP);
                sculpt.getSettings(BRUSH_CLAYBUILDUP).radius = 65.0f;
                sculpt.getSettings(BRUSH_CLAYBUILDUP).intensity = 0.8f;
                runScreenStroke(sculpt, scene, mesh, glm::vec3(0.0f, 6.0f, R), glm::vec3(4.0f, 8.0f, R), 4);
            }

            sculpt.setBrush(brush);
            auto& st = sculpt.getSettings(brush);
            st.radius = 60.0f;
            st.intensity = 0.7f;
            st.spacing = 0.1f;

            const glm::vec3 p0 = glm::normalize(glm::vec3(2.0f, 10.0f, R)) * R;
            const glm::vec3 p1 = glm::normalize(glm::vec3(12.0f, 14.0f, R)) * R;
            runScreenStroke(sculpt, scene, mesh, p0, p1, 4);

            // Check that all 8 angular sectors match identically!
            const int stride = sectors + 1;
            const int sectorStep = sectors / 8;
            float maxSectorAsym = 0.0f;
            int worstR = -1, worstS = -1, worstK = -1;
            for (int r = 1; r < rings; ++r) {
                for (int s = 0; s < sectorStep; ++s) {
                    const uint32_t v0 = r * stride + s;
                    const float rad0 = glm::length(glm::vec3(mesh->verts[v0 * 3], mesh->verts[v0 * 3 + 1], mesh->verts[v0 * 3 + 2]));
                    for (int k = 1; k < 8; ++k) {
                        const uint32_t vk = r * stride + (s + k * sectorStep);
                        const float radK = glm::length(glm::vec3(mesh->verts[vk * 3], mesh->verts[vk * 3 + 1], mesh->verts[vk * 3 + 2]));
                        float diff = std::abs(rad0 - radK);
                        if (diff > maxSectorAsym) {
                            maxSectorAsym = diff;
                            worstR = r;
                            worstS = s;
                            worstK = k;
                        }
                    }
                }
            }

            // Ensure brush actually modified the mesh (except Smooth on flat sphere, which we handled above)
            float maxDeltaFromR = 0.0f;
            for (int v = 0; v < mesh->nbVerts; ++v) {
                float rad = glm::length(glm::vec3(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]));
                maxDeltaFromR = std::max(maxDeltaFromR, std::abs(rad - R));
            }
            std::cout << "  [Audit] " << getBrushNameStr(brush)
                      << " maxDeltaFromR=" << maxDeltaFromR
                      << " maxSectorAsym=" << maxSectorAsym
                      << " (r=" << worstR << " s=" << worstS << " k=" << worstK << ")\n";
            if (maxDeltaFromR < 1e-3f) {
                std::cerr << "  [Error] Brush " << getBrushNameStr(brush) << " had zero effect!\n";
                g_undoManager.clear();
                return false;
            }
            if (maxSectorAsym > 5e-2f) {
                std::cerr << "  [Error] Brush " << getBrushNameStr(brush) << " radial sector asymmetry = " << maxSectorAsym << "\n";
                g_undoManager.clear();
                return false;
            }
        }

        // Pole overlap test: hit top of sphere (aim camera from slightly elevated angle)
        {
            Scene scene;
            SculptManager sculpt;
            Mesh* mesh = makeSceneAndSphere(scene, sculpt);
            Camera& cam = scene.getCamera();
            cam.setOrbitAngles(0.0f, glm::radians(75.0f));
            cam.update(0.016f);

            sculpt.setBrush(BRUSH_MOVE);
            sculpt.getSettings(BRUSH_MOVE).radius = 70.0f;
            sculpt.getSettings(BRUSH_MOVE).intensity = 0.8f;

            const std::vector<float> origVerts = mesh->verts;
            const glm::vec3 poleStart(0.0f, R, 0.0f);
            const glm::vec3 poleEnd(4.0f, R, 0.0f);
            runScreenStroke(sculpt, scene, mesh, poleStart, poleEnd, 4);

            // Verify all vertices finite and displacement bounded
            for (int v = 0; v < mesh->nbVerts; ++v) {
                glm::vec3 p(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                    std::cerr << "  [Error] Non-finite vertex after pole stroke\n";
                    g_undoManager.clear();
                    return false;
                }
            }
            const glm::vec3 poleOrig(origVerts[0], origVerts[1], origVerts[2]);
            const glm::vec3 poleNow(mesh->verts[0], mesh->verts[1], mesh->verts[2]);
            const float poleDisp = glm::distance(poleOrig, poleNow);
            if (poleDisp > 15.0f) {
                std::cerr << "  [Error] Pole vertex exploded/multiplied displacement: " << poleDisp << "\n";
                g_undoManager.clear();
                return false;
            }
        }

        g_undoManager.clear();
        std::cout << "  [Detail] All 5 brushes symmetric across 8 sectors + pole stroke stable\n";
        return true;
    }

    // 3. Undo / Redo + UndoSerializer v2 roundtrip
    static bool testUndoRedoAndSerializerV2() {
        Scene scene;
        SculptManager sculpt;
        auto ptr = createUVSphere(35.0f, 32, 64);
        Mesh* mesh = ptr.release();
        mesh->initFaceGroups();
        mesh->postInit(true);
        scene.addMesh(mesh);
        scene.selectMesh(mesh);

        Camera& cam = scene.getCamera();
        cam.onResize(1280, 720);
        cam.resetViewToMesh(mesh, false);
        cam.update(0.016f);

        sculpt.setRenderer(nullptr);
        sculpt.setUseSym(true);
        sculpt.setSymX(false);
        sculpt.setSymY(false);
        sculpt.setSymZ(false);
        sculpt.setSymRadialEnabled(true);
        sculpt.setSymRadialCount(6);
        sculpt.setSymRadialAxis(1);
        g_undoManager.clear();

        const std::vector<float> initialVerts = mesh->verts;

        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        sculpt.getSettings(BRUSH_CLAYBUILDUP).radius = 65.0f;
        sculpt.getSettings(BRUSH_CLAYBUILDUP).intensity = 0.8f;
        runScreenStroke(sculpt, scene, mesh, glm::vec3(0.0f, 5.0f, 35.0f), glm::vec3(10.0f, 10.0f, 35.0f), 4);

        const std::vector<float> afterStrokeVerts = mesh->verts;
        if (afterStrokeVerts == initialVerts) {
            std::cerr << "  [Error] Radial stroke produced no vertex changes\n";
            g_undoManager.clear();
            return false;
        }

        // Undo
        g_undoManager.undo(scene);
        float maxUndoErr = 0.0f;
        for (size_t i = 0; i < initialVerts.size(); ++i) {
            maxUndoErr = std::max(maxUndoErr, std::abs(mesh->verts[i] - initialVerts[i]));
        }
        if (maxUndoErr > 1e-5f) {
            std::cerr << "  [Error] Undo restoration error = " << maxUndoErr << "\n";
            g_undoManager.clear();
            return false;
        }

        // Redo
        g_undoManager.redo(scene);
        float maxRedoErr = 0.0f;
        for (size_t i = 0; i < afterStrokeVerts.size(); ++i) {
            maxRedoErr = std::max(maxRedoErr, std::abs(mesh->verts[i] - afterStrokeVerts[i]));
        }
        if (maxRedoErr > 1e-5f) {
            std::cerr << "  [Error] Redo restoration error = " << maxRedoErr << "\n";
            g_undoManager.clear();
            return false;
        }

        // Serialize undo & redo stacks with UndoSerializer (v2)
        std::vector<uint8_t> buf;
        if (!UndoSerializer::serializeStacks(g_undoManager.getUndoStack(), g_undoManager.getRedoStack(), buf)) {
            std::cerr << "  [Error] UndoSerializer::serializeStacks failed\n";
            g_undoManager.clear();
            return false;
        }
        std::deque<std::unique_ptr<UndoEntry>> outUndo;
        std::deque<std::unique_ptr<UndoEntry>> outRedo;
        std::unordered_map<uint32_t, Mesh*> idMap = { { mesh->getID(), mesh } };
        size_t offset = 0;
        if (!UndoSerializer::deserializeStacks(buf.data(), buf.size(), offset, outUndo, outRedo, idMap)) {
            std::cerr << "  [Error] UndoSerializer::deserializeStacks failed\n";
            g_undoManager.clear();
            return false;
        }
        if (outUndo.size() != g_undoManager.getUndoStack().size() || outUndo.empty()) {
            std::cerr << "  [Error] Deserialized undo stack count mismatch\n";
            g_undoManager.clear();
            return false;
        }

        // Verify deserialized SculptUndoEntry restores exact initial positions
        auto* deserializedSculpt = dynamic_cast<SculptUndoEntry*>(outUndo.back().get());
        if (!deserializedSculpt || deserializedSculpt->deltas.empty()) {
            std::cerr << "  [Error] Deserialized SculptUndoEntry missing deltas\n";
            g_undoManager.clear();
            return false;
        }
        for (const auto& d : deserializedSculpt->deltas) {
            if (d.meshId == mesh->getID() && d.hasPositions) {
                for (size_t i = 0; i < d.indices.size(); ++i) {
                    const uint32_t v = d.indices[i];
                    mesh->verts[v * 3 + 0] = d.prevVerts[i * 3 + 0];
                    mesh->verts[v * 3 + 1] = d.prevVerts[i * 3 + 1];
                    mesh->verts[v * 3 + 2] = d.prevVerts[i * 3 + 2];
                }
            }
        }
        for (size_t i = 0; i < initialVerts.size(); ++i) {
            if (std::abs(mesh->verts[i] - initialVerts[i]) > 1e-5f) {
                std::cerr << "  [Error] Deserialized undo failed to restore initial state\n";
                g_undoManager.clear();
                return false;
            }
        }

        g_undoManager.clear();
        std::cout << "  [Detail] Undo/Redo exact (maxErr=" << maxUndoErr << ") & UndoSerializer v2 OK (" << buf.size() << " B)\n";
        return true;
    }

    // 4. Dynamic Topology + Radial 5 + Bit-for-Bit Undo
    static bool testDynTopoRadialSymmetry() {
        auto mesh = createUVSphere(35.0f, 24, 50); // 50 divisible by 5
        mesh->initFaceGroups();
        mesh->postInit(true);

        const int initV = mesh->nbVerts;
        const int initF = mesh->nbFaces;
        const std::vector<float> initVerts = mesh->verts;
        const std::vector<uint32_t> initFaces = mesh->faces;

        SymmetryParams sp;
        sp.useSym = true;
        sp.radialEnabled = true;
        sp.radialCount = 5;
        sp.radialAxis = 1;
        auto symCopies = buildSymmetryCopies(sp, mesh.get(), true);
        if (symCopies.size() != 5) return false;

        dynamic::DynamicTopology dyntopo;
        const glm::vec3 center = glm::normalize(glm::vec3(0.0f, 10.0f, 35.0f)) * 35.0f;
        const float radius = 8.0f;
        const float targetEdge = 1.0f;

        if (!dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, symCopies)) {
            std::cerr << "  [Error] DynTopo remesh returned false\n";
            return false;
        }
        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        if (!undo) {
            std::cerr << "  [Error] DynTopo finish returned null undo\n";
            return false;
        }
        if (mesh->nbVerts <= initV) {
            std::cerr << "  [Error] DynTopo expected vertex growth, stayed " << mesh->nbVerts << "\n";
            return false;
        }

        // Verify vertices exist in all 5 rotated copies around Y
        for (int k = 1; k < 5; ++k) {
            glm::vec3 rotCenter = symCopies[k].point(center);
            int nearby = 0;
            for (int v = 0; v < mesh->nbVerts; ++v) {
                glm::vec3 vp(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
                if (glm::distance(vp, rotCenter) < 3.0f) nearby++;
            }
            if (nearby < 5) {
                std::cerr << "  [Error] DynTopo copy " << k << " had only " << nearby << " refined verts\n";
                return false;
            }
        }

        std::string bvhErr;
        if (!mesh->bvh.validateTree(*mesh, bvhErr)) {
            std::cerr << "  [Error] BVH invalid after radial DynTopo: " << bvhErr << "\n";
            return false;
        }

        // Serialize + Deserialize DynamicTopologyUndoEntry via UndoSerializer v2
        {
            std::deque<std::unique_ptr<UndoEntry>> uStack, rStack;
            uStack.push_back(std::make_unique<DynamicTopologyUndoEntry>(mesh->getID(), std::move(undo)));

            std::vector<uint8_t> sBuf;
            if (!UndoSerializer::serializeStacks(uStack, rStack, sBuf)) {
                std::cerr << "  [Error] DynamicTopologyUndoEntry serialize failed\n";
                return false;
            }
            std::deque<std::unique_ptr<UndoEntry>> dUndo, dRedo;
            std::unordered_map<uint32_t, Mesh*> idMap = { { mesh->getID(), mesh.get() } };
            size_t off = 0;
            if (!UndoSerializer::deserializeStacks(sBuf.data(), sBuf.size(), off, dUndo, dRedo, idMap) || dUndo.empty()) {
                std::cerr << "  [Error] DynamicTopologyUndoEntry deserialize failed\n";
                return false;
            }

            auto* dtEntry = dynamic_cast<DynamicTopologyUndoEntry*>(dUndo.back().get());
            if (!dtEntry || !dtEntry->record) {
                std::cerr << "  [Error] Deserialized DynamicTopologyUndoEntry missing record\n";
                return false;
            }
            dtEntry->record->swapWith(*mesh);
            mesh->initTopology();
        }

        if (mesh->nbVerts != initV || mesh->nbFaces != initF || mesh->verts != initVerts || mesh->faces != initFaces) {
            std::cerr << "  [Error] DynTopo radial undo did not match bit-for-bit\n";
            return false;
        }

        std::cout << "  [Detail] DynTopo Radial 5 grew V=" << initV << " -> refined, serialized v2, undone bit-for-bit\n";
        return true;
    }

    // 5. TrimTool & SplitTool with Radial Symmetry
    static bool testTrimAndSplitRadial() {
        // Part A: TrimTool with Radial 6
        {
            Scene scene;
            auto ptr = createUVSphere(2.0f, 24, 48);
            Mesh* mesh = ptr.release();
            mesh->initFaceGroups();
            mesh->postInit(true);
            scene.addMesh(mesh);
            scene.selectMesh(mesh);

            Camera& cam = scene.getCamera();
            cam.onResize(1280, 720);
            cam.resetViewToMesh(mesh, false);
            cam.update(0.016f);

            SymmetryParams sp;
            sp.useSym = true;
            sp.radialEnabled = true;
            sp.radialCount = 6;
            sp.radialAxis = 1;
            auto copies = buildSymmetryCopies(sp, mesh, false);

            // Screen-space lasso notch cutting an upper-front edge
            std::vector<glm::vec2> lasso = {
                { 610.0f, 160.0f },
                { 670.0f, 160.0f },
                { 670.0f, 260.0f },
                { 610.0f, 260.0f }
            };

            const int beforeVerts = mesh->nbVerts;
            TrimConfig cfg;
            cfg.fillHole = true;
            if (!TrimTool::execute(mesh, cam, lasso, false, copies, cfg)) {
                std::cerr << "  [Error] TrimTool::execute returned false\n";
                return false;
            }
            if (mesh->nbFaces <= 0 || mesh->nbVerts <= 0 || mesh->nbVerts == beforeVerts) {
                std::cerr << "  [Error] TrimTool radial produced unexpected mesh V=" << mesh->nbVerts << " F=" << mesh->nbFaces << "\n";
                return false;
            }
        }

        // Part B: SplitTool::splitByLasso with Radial 4
        {
            Scene scene;
            auto ptr = createUVSphere(2.0f, 20, 40);
            Mesh* mesh = ptr.release();
            mesh->initFaceGroups();
            mesh->postInit(true);
            scene.addMesh(mesh);
            scene.selectMesh(mesh);

            Camera& cam = scene.getCamera();
            cam.onResize(1280, 720);
            cam.resetViewToMesh(mesh, false);
            cam.update(0.016f);

            SymmetryParams sp;
            sp.useSym = true;
            sp.radialEnabled = true;
            sp.radialCount = 4;
            sp.radialAxis = 1;
            auto copies = buildSymmetryCopies(sp, mesh, false);

            std::vector<glm::vec2> lasso = {
                { 600.0f, 170.0f },
                { 680.0f, 170.0f },
                { 680.0f, 270.0f },
                { 600.0f, 270.0f }
            };

            if (!sculpt::SplitTool::splitByLasso(scene, mesh, cam, lasso, false, copies)) {
                std::cerr << "  [Error] SplitTool::splitByLasso returned false\n";
                return false;
            }
            if (scene.getMeshes().size() < 2) {
                std::cerr << "  [Error] SplitTool expected >= 2 meshes in scene, got " << scene.getMeshes().size() << "\n";
                return false;
            }
        }

        g_undoManager.clear();
        std::cout << "  [Detail] TrimTool (Radial 6) & SplitTool (Radial 4) CSG cuts succeeded cleanly\n";
        return true;
    }

    // 6. INI settings & .spsculpt v21 Export/Import roundtrip
    static bool testSettingsIniAndSglV21Persistence() {
        // INI test
        {
            SculptManager src;
            src.setUseSym(true);
            src.setSymX(false);
            src.setSymY(true);
            src.setSymZ(false);
            src.setSymRadialEnabled(true);
            src.setSymRadialCount(12);
            src.setSymRadialAxis(2);
            src.setSymmetryMode(SymmetryMode::World);

            IniFile ini;
            if (!src.saveSettings(ini)) return false;

            SculptManager dst;
            if (!dst.loadSettings(ini)) return false;

            if (!dst.getUseSym() || dst.getSymX() || !dst.getSymY() || dst.getSymZ() ||
                !dst.getSymRadialEnabled() || dst.getSymRadialCount() != 12 ||
                dst.getSymRadialAxis() != 2 || dst.getSymmetryMode() != SymmetryMode::World) {
                std::cerr << "  [Error] INI settings roundtrip mismatch\n";
                return false;
            }
        }

        // .spsculpt v21 Export/Import test
        {
            static_assert(Format::CURRENT_VERSION >= 21, "Expected .spsculpt CURRENT_VERSION >= 21");

            Scene sceneOut;
            auto ptr = createUVSphere(2.0f, 12, 24);
            Mesh* meshOut = ptr.release();
            meshOut->outlinerName = "RadialTestSphere";
            meshOut->symX = false;
            meshOut->symY = false;
            meshOut->symZ = true;
            meshOut->symRadialEnabled = true;
            meshOut->symRadialCount = 16;
            meshOut->symRadialAxis = 0;
            meshOut->symEnabled = true;
            meshOut->postInit(true);
            sceneOut.addMesh(meshOut);
            sceneOut.selectMesh(meshOut);

            SculptManager sculptOut;
            sculptOut.setRenderer(nullptr);
            sculptOut.loadSymmetryFromMesh(meshOut);

            AngleRenderer rendererOut;
            std::vector<uint8_t> sglBytes = ExportSGL::exportSGL({ meshOut }, sceneOut, rendererOut, sculptOut);
            if (sglBytes.size() < 16) {
                std::cerr << "  [Error] exportSGL produced tiny buffer\n";
                return false;
            }

            Scene sceneIn;
            SculptManager sculptIn;
            sculptIn.setRenderer(nullptr);
            AngleRenderer rendererIn;
            std::vector<Mesh*> loaded = ImportSGL::importSGL(sglBytes, sceneIn, rendererIn, &sculptIn);
            if (loaded.size() != 1 || !loaded[0]) {
                std::cerr << "  [Error] importSGL failed to load mesh\n";
                return false;
            }

            Mesh* mIn = loaded[0];
            if (!mIn->symEnabled || mIn->symX || mIn->symY || !mIn->symZ ||
                !mIn->symRadialEnabled || mIn->symRadialCount != 16 || mIn->symRadialAxis != 0) {
                std::cerr << "  [Error] SGL v21 mesh symmetry fields mismatch after import\n";
                return false;
            }
            if (!sculptIn.getSymRadialEnabled() || sculptIn.getSymRadialCount() != 16 || sculptIn.getSymRadialAxis() != 0) {
                std::cerr << "  [Error] SGL v21 SculptManager radial sync mismatch after import\n";
                return false;
            }
        }

        std::cout << "  [Detail] INI & .spsculpt v" << Format::CURRENT_VERSION << " roundtrip verified\n";
        return true;
    }

    // 7. 1M vertex mesh stroke timing benchmark (Radial 8 & Radial 32)
    static bool testPerformance1MRadial() {
        const int rings = 1000;
        const int sectors = 1000;
        auto ptr = createUVSphere(35.0f, rings, sectors);
        Mesh* mesh = ptr.release();
        mesh->postInit(true);

        Scene scene;
        scene.addMesh(mesh);
        scene.selectMesh(mesh);

        Camera& cam = scene.getCamera();
        cam.onResize(1280, 720);
        cam.resetViewToMesh(mesh, false);
        cam.update(0.016f);

        SculptManager sculpt;
        sculpt.setRenderer(nullptr);
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        sculpt.getSettings(BRUSH_CLAYBUILDUP).radius = 50.0f;
        sculpt.getSettings(BRUSH_CLAYBUILDUP).intensity = 0.5f;
        sculpt.setUseSym(true);
        sculpt.setSymX(false);
        sculpt.setSymY(false);
        sculpt.setSymZ(false);
        sculpt.setSymRadialEnabled(true);
        sculpt.setSymRadialAxis(1);
        sculpt.warmupMeshCaches(mesh);

        const glm::vec3 s0 = cam.project(glm::vec3(0.0f, 5.0f, 35.0f));
        const glm::vec3 s1 = cam.project(glm::vec3(4.0f, 8.0f, 35.0f));

        auto runTimed = [&](int radialN) {
            sculpt.setSymRadialCount(radialN);
            g_undoManager.clear();

            SDL_Event evDown;
            std::memset(&evDown, 0, sizeof(evDown));
            evDown.type = SDL_MOUSEBUTTONDOWN;
            evDown.button.button = SDL_BUTTON_LEFT;
            evDown.button.x = static_cast<int>(std::round(s0.x));
            evDown.button.y = static_cast<int>(std::round(s0.y));

            auto t0 = std::chrono::high_resolution_clock::now();
            sculpt.handleEvent(evDown, scene);
            auto t1 = std::chrono::high_resolution_clock::now();
            const SculptFrameProfile profDown = sculpt.getLastFrameProfile();

            SDL_Event evMove;
            std::memset(&evMove, 0, sizeof(evMove));
            evMove.type = SDL_MOUSEMOTION;
            evMove.motion.state = SDL_BUTTON_LMASK;
            evMove.motion.x = static_cast<int>(std::round(s1.x));
            evMove.motion.y = static_cast<int>(std::round(s1.y));
            auto t2 = std::chrono::high_resolution_clock::now();
            sculpt.handleEvent(evMove, scene);
            auto t3 = std::chrono::high_resolution_clock::now();
            const SculptFrameProfile profMove = sculpt.getLastFrameProfile();

            SDL_Event evUp;
            std::memset(&evUp, 0, sizeof(evUp));
            evUp.type = SDL_MOUSEBUTTONUP;
            evUp.button.button = SDL_BUTTON_LEFT;
            evUp.button.x = static_cast<int>(std::round(s1.x));
            evUp.button.y = static_cast<int>(std::round(s1.y));
            sculpt.handleEvent(evUp, scene);

            double msDown = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double msMove = std::chrono::duration<double, std::milli>(t3 - t2).count();
            std::cout << "  [Bench] " << mesh->nbVerts << " verts | Radial " << std::setw(2) << radialN
                      << " | Step0=" << std::fixed << std::setprecision(2) << msDown
                      << "ms (sym=" << profDown.symmetryMs << "ms)"
                      << " | Move=" << msMove
                      << "ms (sym=" << profMove.symmetryMs << "ms, affected=" << profMove.affectedVertCount << ")\n";
        };

        runTimed(8);
        runTimed(32);

        g_undoManager.clear();
        return true;
    }
};

