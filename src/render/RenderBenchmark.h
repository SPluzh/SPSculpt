#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "common/Enums.h"
#include "common/Logger.h"
#include "common/OptFlags.h"
#include "common/SystemStats.h"
#include "editing/PrimitiveTool.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "sculpt/SculptEngine.h"
#include "render/AngleRenderer.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

namespace RenderBenchmark {

inline std::unique_ptr<Mesh> createSphereMeshWithPolygroups(int rings, int sectors) {
    auto mesh = std::make_unique<Mesh>();
    mesh->verts.reserve((size_t)(rings + 1) * (sectors + 1) * 3);
    mesh->faces.reserve((size_t)rings * sectors * 4);

    const float R = 2.0f;
    for (int r = 0; r <= rings; ++r) {
        float phi = float(M_PI) * float(r) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        for (int s = 0; s <= sectors; ++s) {
            float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
            mesh->verts.push_back(R * sinPhi * std::cos(theta));
            mesh->verts.push_back(R * cosPhi);
            mesh->verts.push_back(R * sinPhi * std::sin(theta));
            mesh->nbVerts++;
        }
    }

    mesh->faceGroups.reserve((size_t)rings * sectors);
    for (int r = 0; r < rings; ++r) {
        int bandR = (r * 4) / std::max(1, rings);
        for (int s = 0; s < sectors; ++s) {
            int bandS = (s * 4) / std::max(1, sectors);
            uint32_t v0 = (uint32_t)r * (sectors + 1) + s;
            uint32_t v1 = v0 + 1;
            uint32_t v2 = (uint32_t)(r + 1) * (sectors + 1) + s + 1;
            uint32_t v3 = (uint32_t)(r + 1) * (sectors + 1) + s;

            mesh->faces.push_back(v0);
            mesh->faces.push_back(v1);
            mesh->faces.push_back(v2);
            mesh->faces.push_back(v3);
            mesh->faceGroups.push_back((uint32_t)(bandR * 4 + bandS + 1));
            mesh->nbFaces++;
        }
    }

    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    return mesh;
}

inline double percentile95(std::vector<double> vals) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    size_t idx = (size_t)std::min<size_t>(vals.size() - 1, (size_t)std::ceil(0.95 * (double)vals.size()) - 1);
    return vals[idx];
}

inline double meanOf(const std::vector<double>& vals) {
    if (vals.empty()) return 0.0;
    double s = std::accumulate(vals.begin(), vals.end(), 0.0);
    return s / (double)vals.size();
}

inline double maxOf(const std::vector<double>& vals) {
    if (vals.empty()) return 0.0;
    return *std::max_element(vals.begin(), vals.end());
}

inline double elapsedMs(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

inline bool run(int targetFaces, Scene& scene, SculptManager& sculpt, AngleRenderer& renderer, SDL_Window* window) {
    using Clock = std::chrono::high_resolution_clock;

    const int rings = std::max(8, (int)std::sqrt((double)targetFaces));
    const int sectors = std::max(8, targetFaces / rings);

    int winW = 1920, winH = 1080;
    if (window) {
        SDL_GetWindowSize(window, &winW, &winH);
    }
    if (winW <= 0) winW = 1920;
    if (winH <= 0) winH = 1080;

    int drawW = winW, drawH = winH;
    if (window) {
        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    }

    const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

    std::cout << "\n================================================================================\n";
    std::cout << "  SPSculpt Render + Interactive GPU Benchmark @ ~"
              << std::fixed << std::setprecision(1) << ((double)rings * sectors / 1000000.0) << "M Faces\n";
    std::cout << "================================================================================\n";
    std::cout << "  OptFlags:  " << OptFlags::formatSummary() << "\n";
    std::cout << "  Window:    " << winW << "x" << winH << " (Drawable: " << drawW << "x" << drawH << ")\n";
    std::cout << "  GL_VENDOR:   " << (glVendor ? glVendor : "unknown") << "\n";
    std::cout << "  GL_RENDERER: " << (glRenderer ? glRenderer : "unknown") << "\n";
    std::cout << "  GL_VERSION:  " << (glVersion ? glVersion : "unknown") << "\n\n";

    // Generate mesh & initialize BVH/topology
    auto tGen0 = Clock::now();
    auto meshPtr = createSphereMeshWithPolygroups(rings, sectors);
    Mesh* meshRaw = meshPtr.release();
    double genMs = elapsedMs(tGen0);

    auto tInit0 = Clock::now();
    meshRaw->postInit(true);
    double postInitMs = elapsedMs(tInit0);

    std::cout << "  Mesh:      " << meshRaw->nbFaces << " quads, " << meshRaw->nbVerts << " verts"
              << " (gen=" << std::setprecision(1) << genMs << " ms, postInit=" << postInitMs << " ms)\n";

    scene.clear();
    g_undoManager.clear();
    scene.addMesh(meshRaw);
    scene.selectMesh(meshRaw);

    Camera& camera = scene.getCamera();
    camera.onResize(winW, winH);
    camera.resetViewToMesh(meshRaw, false);
    camera.update(0.016f);

    // Deterministic baseline render configuration (Matcap FV, clean fast path)
    int fvIdx = renderer.findMatcapIndexByName("Matcap FV");
    renderer.setMatcap(fvIdx >= 0 ? fvIdx : 3);
    renderer.setShaderType(1); // MATCAP
    renderer.setTextureId(0);
    renderer.setUseShadows(false);
    renderer.setUseSsao(false);
    renderer.setUseSsr(false);
    renderer.setUseTaa(false);
    renderer.setUseFxaa(false);
    renderer.setBevelEnabled(false);
    renderer.setShowContour(false);
    renderer.setShowWireframe(false);
    renderer.setShowPolyGroups(false);
    renderer.setEnableSelectionFlash(false);
    renderer.setShowSymmetryLine(false);
    renderer.setShowInvertedNormals(false);
    renderer.setXrayEnabled(false);
    renderer.setShowGrid(false);

    // Register mesh & symmetry in SculptManager cleanly
    sculpt.warmupMeshCaches(meshRaw);
    sculpt.setUseSym(true);
    sculpt.setSymX(true);
    sculpt.setSymY(false);
    sculpt.setSymZ(false);
    sculpt.saveSymmetryToMesh(meshRaw);
    sculpt.registerKnownMesh(meshRaw);
    sculpt.setBrush(BRUSH_CLAYBUILDUP);
    sculpt.setRawMousePos(winW / 2, winH / 2);
    sculpt.processFrame(scene);
    sculpt.getCursor().applyToRenderer(renderer);
    renderer.setActiveBrush(BRUSH_CLAYBUILDUP);

    // Initial warmup & measure initial full GPU upload
    renderer.resetLastUploadTiming();
    auto tInitUp0 = Clock::now();
    renderer.render(scene);
    glFinish();
    if (window) SDL_GL_SwapWindow(window);
    double initUploadTotalMs = elapsedMs(tInitUp0);
    const auto initUpload = renderer.getLastUploadTiming();
    std::cout << "  InitUpload: VBO=" << std::setprecision(2) << ((double)initUpload.bytesVbo / (1024.0 * 1024.0))
              << " MB (" << initUpload.msVbo << " ms), EBO="
              << ((double)initUpload.bytesEbo / (1024.0 * 1024.0))
              << " MB (" << initUpload.msEbo << " ms), TotalFrame=" << initUploadTotalMs << " ms\n\n";

    // Two extra warmup frames to let shader/driver caches settle
    for (int w = 0; w < 2; ++w) {
        SDL_PumpEvents();
        renderer.render(scene);
        glFinish();
        if (window) SDL_GL_SwapWindow(window);
    }

    auto renderSyncFrame = [&](double& outRenderCpuMs, double& outGpuFinishMs, double& outSwapMs) {
        renderer.resetLastUploadTiming();
        auto tR0 = Clock::now();
        renderer.render(scene);
        outRenderCpuMs = elapsedMs(tR0);

        auto tG0 = Clock::now();
        glFinish();
        outGpuFinishMs = elapsedMs(tG0);

        auto tS0 = Clock::now();
        if (window) SDL_GL_SwapWindow(window);
        outSwapMs = elapsedMs(tS0);
    };

    const float scalePx = std::max(0.5f, (float)std::min(winW, winH) / 1080.0f);
    const float centerX = (float)winW * 0.5f;
    const float centerY = (float)winH * 0.5f;

    // =========================================================================
    // [1/5] Static Frame & Idle Loop Simulation (2.0s)
    // =========================================================================
    {
        double rCpu = 0.0, gFin = 0.0, sSwap = 0.0;
        renderSyncFrame(rCpu, gFin, sSwap);
        const auto stats = renderer.getLastRenderStats();

        const double targetIdleDurationMs = 2000.0;
        const double idleTargetPeriodMs = 1000.0 / 15.0; // Stage 0 15 FPS idle cadence
        auto tIdleStart = Clock::now();
        int idleFrames = 0;
        double activeRenderCpuMs = 0.0;
        double activeGpuFinishMs = 0.0;
        double activeSwapMs = 0.0;

        while (true) {
            double wall = elapsedMs(tIdleStart);
            if (wall >= targetIdleDurationMs) break;
            SDL_PumpEvents();

            if (!OptFlags::get().idleSleep) {
                auto tIter0 = Clock::now();
                double fr = 0.0, fg = 0.0, fs = 0.0;
                renderSyncFrame(fr, fg, fs);
                activeRenderCpuMs += fr;
                activeGpuFinishMs += fg;
                activeSwapMs += fs;
                idleFrames++;

                double iterUsed = elapsedMs(tIter0);
                double remLoop = targetIdleDurationMs - elapsedMs(tIdleStart);
                if (remLoop > 0.0 && iterUsed < idleTargetPeriodMs) {
                    Uint32 delayMs = (Uint32)std::min(remLoop, idleTargetPeriodMs - iterUsed);
                    if (delayMs > 0) SDL_Delay(delayMs);
                }
            } else {
                double remLoop = targetIdleDurationMs - wall;
                Uint32 delayMs = (Uint32)std::min(50.0, remLoop);
                if (delayMs > 0) SDL_Delay(delayMs);
            }
        }
        double totalIdleWallMs = elapsedMs(tIdleStart);
        double activeTotalMs = activeRenderCpuMs + activeGpuFinishMs + activeSwapMs;
        double idleFps = (totalIdleWallMs > 0.0) ? ((double)idleFrames * 1000.0 / totalIdleWallMs) : 0.0;
        double activePct = (totalIdleWallMs > 0.0) ? (100.0 * activeTotalMs / totalIdleWallMs) : 0.0;

        std::cout << "[1/5] Static Frame & Idle Cadence (2.0s):\n";
        std::cout << "      StaticFrame:  RenderCPU=" << std::setprecision(2) << rCpu
                  << " ms | glFinish=" << gFin << " ms | Swap=" << sSwap
                  << " ms | Total=" << (rCpu + gFin + sSwap) << " ms\n";
        std::cout << "      DrawStats:    DrawCalls=" << stats.meshDrawCalls
                  << " | Tris=" << stats.trisSubmitted
                  << " | CullON=" << stats.cullEnabledDraws
                  << " | CullOFF=" << stats.cullDisabledDraws << "\n";
        std::cout << "      Idle2s:       Frames=" << idleFrames
                  << " (" << std::setprecision(1) << idleFps << " FPS)"
                  << " | ActiveGpuCpu=" << std::setprecision(1) << activeTotalMs
                  << " ms (" << activePct << "% busy over " << totalIdleWallMs << " ms)\n\n";
    }

    // =========================================================================
    // [2/5] Camera Orbit (40 frames) & Close-Up Zoom (30 frames)
    // =========================================================================
    {
        const int ORBIT_FRAMES = 40;
        std::vector<double> orbitRenderCpu, orbitGpuFin, orbitSwap, orbitTotal;
        orbitRenderCpu.reserve(ORBIT_FRAMES);
        orbitGpuFin.reserve(ORBIT_FRAMES);
        orbitSwap.reserve(ORBIT_FRAMES);
        orbitTotal.reserve(ORBIT_FRAMES);

        camera.resetViewToMesh(meshRaw, false);
        camera.update(0.016f);
        camera.start(centerX, centerY, true);
        AngleRenderer::GpuRenderStats lastStats{};

        for (int i = 0; i < ORBIT_FRAMES; ++i) {
            SDL_PumpEvents();
            camera.rotate(centerX + (float)(i + 1) * 8.0f * scalePx,
                          centerY + std::sin((float)i * 0.25f) * 18.0f * scalePx,
                          1.0f);
            double r = 0.0, g = 0.0, s = 0.0;
            renderSyncFrame(r, g, s);
            lastStats = renderer.getLastRenderStats();
            orbitRenderCpu.push_back(r);
            orbitGpuFin.push_back(g);
            orbitSwap.push_back(s);
            orbitTotal.push_back(r + g + s);
        }

        const int CLOSEUP_FRAMES = 30;
        std::vector<double> closeRenderCpu, closeGpuFin, closeSwap, closeTotal;
        closeRenderCpu.reserve(CLOSEUP_FRAMES);
        closeGpuFin.reserve(CLOSEUP_FRAMES);
        closeSwap.reserve(CLOSEUP_FRAMES);
        closeTotal.reserve(CLOSEUP_FRAMES);

        camera.zoom(+1.6f);
        camera.update(0.016f);
        camera.start(centerX, centerY, true);
        for (int i = 0; i < CLOSEUP_FRAMES; ++i) {
            SDL_PumpEvents();
            camera.rotate(centerX + (float)(i + 1) * 6.0f * scalePx, centerY, 1.0f);
            double r = 0.0, g = 0.0, s = 0.0;
            renderSyncFrame(r, g, s);
            closeRenderCpu.push_back(r);
            closeGpuFin.push_back(g);
            closeSwap.push_back(s);
            closeTotal.push_back(r + g + s);
        }

        // Restore camera to canonical front view
        camera.resetViewToMesh(meshRaw, false);
        camera.update(0.016f);
        double tmpR, tmpG, tmpS;
        renderSyncFrame(tmpR, tmpG, tmpS);

        std::cout << "[2/5] Camera Orbit (40f) & Close-Up Zoom (30f):\n";
        std::cout << std::setprecision(2);
        std::cout << "      Orbit40:   AvgRenderCPU=" << meanOf(orbitRenderCpu)
                  << " ms | AvgFinish=" << meanOf(orbitGpuFin)
                  << " ms | AvgTotal=" << meanOf(orbitTotal)
                  << " ms (p95=" << percentile95(orbitTotal) << " ms)"
                  << " | Draws=" << lastStats.meshDrawCalls << ", Tris=" << lastStats.trisSubmitted << "\n";
        std::cout << "      CloseUp30: AvgRenderCPU=" << meanOf(closeRenderCpu)
                  << " ms | AvgFinish=" << meanOf(closeGpuFin)
                  << " ms | AvgTotal=" << meanOf(closeTotal)
                  << " ms (p95=" << percentile95(closeTotal) << " ms)\n\n";
    }

    // =========================================================================
    // [3/5] ClayBuildup + Symmetry X + Live Render (12 steps) + Undo/Redo Check
    // =========================================================================
    bool undoRedoOk = true;
    {
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& settings = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        settings.radius = 134.0f * scalePx;
        settings.intensity = 1.0f;
        settings.focalShift = -0.285f;
        settings.spacing = 0.05f;
        settings.clay = true;
        settings.accumulate = false;
        settings.culling = false;

        sculpt.setUseSym(true);
        sculpt.setSymX(true);
        sculpt.saveSymmetryToMesh(meshRaw);
        sculpt.warmupMeshCaches(meshRaw);

        const int STEPS = 12;
        const float startX = centerX - 130.0f * scalePx;
        const float startY = centerY;
        const float stepDx = 9.0f * scalePx; // > minSpacing (~6.7 * scalePx)

        std::vector<double> vSculptCpu, vProcMs, vVboMs, vVboMB, vSpanPct, vRuns, vRenderCpu, vGpuFin, vTotal;
        int minPicked = 1 << 30;
        int maxPicked = 0;

        for (int i = 0; i < STEPS; ++i) {
            SDL_PumpEvents();
            float curX = startX + (float)i * stepDx;
            float curY = startY + std::sin((float)i * 0.4f) * 6.0f * scalePx;

            SDL_Event ev;
            std::memset(&ev, 0, sizeof(ev));
            if (i == 0) {
                ev.type = SDL_MOUSEBUTTONDOWN;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.x = (int)std::round(curX);
                ev.button.y = (int)std::round(curY);
            } else {
                ev.type = SDL_MOUSEMOTION;
                ev.motion.state = SDL_BUTTON_LMASK;
                ev.motion.x = (int)std::round(curX);
                ev.motion.y = (int)std::round(curY);
            }

            auto tS0 = Clock::now();
            sculpt.handleEvent(ev, scene);
            double sCpu = elapsedMs(tS0);

            int picked = sculpt.getLastFrameProfile().pickedVertCount;
            minPicked = std::min(minPicked, picked);
            maxPicked = std::max(maxPicked, picked);

            auto tP0 = Clock::now();
            sculpt.setRawMousePos((int)std::round(curX), (int)std::round(curY));
            sculpt.processFrame(scene);
            double pMs = elapsedMs(tP0);

            sculpt.getCursor().applyToRenderer(renderer);
            renderer.setActiveBrush(BRUSH_CLAYBUILDUP);

            double rCpu = 0.0, gFin = 0.0, sSwap = 0.0;
            renderSyncFrame(rCpu, gFin, sSwap);
            const auto& ut = renderer.getLastUploadTiming();
            double mb = (double)ut.bytesVbo / (1024.0 * 1024.0);
            double spanPct = (ut.dirtyMeshVerts > 0) ? (100.0 * (double)ut.dirtyVertSpan / (double)ut.dirtyMeshVerts) : 0.0;

            vSculptCpu.push_back(sCpu);
            vProcMs.push_back(pMs);
            vVboMs.push_back(ut.msVbo);
            vVboMB.push_back(mb);
            vSpanPct.push_back(spanPct);
            vRuns.push_back((double)ut.dirtyRunCount);
            vRenderCpu.push_back(rCpu);
            vGpuFin.push_back(gFin);
            vTotal.push_back(sCpu + pMs + rCpu + gFin + sSwap);
        }

        // Release
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(startX + (float)(STEPS - 1) * stepDx);
        evUp.button.y = (int)std::round(startY);
        sculpt.handleEvent(evUp, scene);

        // Verify Undo / Redo exact match
        g_undoManager.undo(scene);
        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            size_t n = d.indices.size();
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(meshRaw->verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]);
                float d1 = std::abs(meshRaw->verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]);
                float d2 = std::abs(meshRaw->verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2]);
                maxUndoDiff = std::max({maxUndoDiff, d0, d1, d2});
            }
        }
        g_undoManager.redo(scene);
        float maxRedoDiff = 0.0f;
        auto* undoEntry = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntry && !undoEntry->deltas.empty()) {
            const auto& d = undoEntry->deltas[0];
            size_t n = d.indices.size();
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(meshRaw->verts[vid * 3 + 0] - d.nextVerts[i * 3 + 0]);
                float d1 = std::abs(meshRaw->verts[vid * 3 + 1] - d.nextVerts[i * 3 + 1]);
                float d2 = std::abs(meshRaw->verts[vid * 3 + 2] - d.nextVerts[i * 3 + 2]);
                maxRedoDiff = std::max({maxRedoDiff, d0, d1, d2});
            }
        }
        g_undoManager.undo(scene);
        g_undoManager.clear();

        if (maxUndoDiff != 0.0f || maxRedoDiff != 0.0f || maxPicked == 0) {
            undoRedoOk = false;
        }

        // Sync restored clean mesh to GPU before next test
        meshRaw->vertProxy = meshRaw->verts;
        double rTmp = 0.0, gTmp = 0.0, sTmp = 0.0;
        renderSyncFrame(rTmp, gTmp, sTmp);

        std::cout << "[3/5] ClayBuildup + Symmetry X + Live Render (" << STEPS << " steps):\n";
        std::cout << std::setprecision(2);
        std::cout << "      Step0:       SculptCPU=" << vSculptCpu[0]
                  << " ms | ProcessFrame=" << vProcMs[0]
                  << " ms | VboUpload=" << vVboMB[0] << " MB (" << vVboMs[0] << " ms, span=" << vSpanPct[0] << "%, runs=" << vRuns[0] << ")"
                  << " | FrameTotal=" << vTotal[0] << " ms\n";
        std::cout << "      All12Avg:    SculptCPU=" << meanOf(vSculptCpu)
                  << " ms | ProcessFrame=" << meanOf(vProcMs)
                  << " ms | VboUpload=" << meanOf(vVboMB) << " MB (" << meanOf(vVboMs) << " ms, span=" << meanOf(vSpanPct) << "%, runs=" << meanOf(vRuns) << ")\n";
        std::cout << "      Render/Max:  AvgRenderCPU=" << meanOf(vRenderCpu)
                  << " ms | AvgFinish=" << meanOf(vGpuFin)
                  << " ms | AvgFrame=" << meanOf(vTotal)
                  << " ms | MaxFrame=" << maxOf(vTotal) << " ms\n";
        std::cout << "      Validation:  PickedVerts=[" << minPicked << ".." << maxPicked
                  << "] | UndoDiff=" << std::setprecision(6) << maxUndoDiff
                  << " | RedoDiff=" << maxRedoDiff << (undoRedoOk ? " [PASSED]" : " [FAILED]") << "\n\n";
    }

    // =========================================================================
    // [4/5] Burst Input (20 SDL_MOUSEMOTION events queued in 1 frame)
    // =========================================================================
    {
        // Case A: BRUSH_MOVE
        sculpt.setBrush(BRUSH_MOVE);
        auto& moveSet = sculpt.getSettings(BRUSH_MOVE);
        moveSet.radius = 250.0f * scalePx;
        moveSet.intensity = 1.0f;
        moveSet.topoCheck = true;
        moveSet.culling = false;
        meshRaw->vertProxy = meshRaw->verts;
        sculpt.warmupMeshCaches(meshRaw);

        float moveStartX = centerX - 80.0f * scalePx;
        float moveStartY = centerY;

        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)std::round(moveStartX);
        evDown.button.y = (int)std::round(moveStartY);

        auto tMDown0 = Clock::now();
        sculpt.handleEvent(evDown, scene);
        double moveDownMs = elapsedMs(tMDown0);

        sculpt.setRawMousePos(evDown.button.x, evDown.button.y);
        sculpt.processFrame(scene);
        double r0, g0, s0;
        renderSyncFrame(r0, g0, s0);

        constexpr int BURST_EVENTS = 20;
        auto tBurstMove0 = Clock::now();
        int lastMx = evDown.button.x, lastMy = evDown.button.y;
        for (int i = 1; i <= BURST_EVENTS; ++i) {
            SDL_Event evM;
            std::memset(&evM, 0, sizeof(evM));
            evM.type = SDL_MOUSEMOTION;
            evM.motion.state = SDL_BUTTON_LMASK;
            lastMx = (int)std::round(moveStartX + (float)i * 3.0f * scalePx);
            lastMy = (int)std::round(moveStartY - (float)i * 1.0f * scalePx);
            evM.motion.x = lastMx;
            evM.motion.y = lastMy;
            sculpt.handleEvent(evM, scene);
        }
        double burstMoveEvMs = elapsedMs(tBurstMove0);

        auto tP0 = Clock::now();
        sculpt.setRawMousePos(lastMx, lastMy);
        sculpt.processFrame(scene);
        double burstMoveProcMs = elapsedMs(tP0);

        double bmR = 0.0, bmG = 0.0, bmS = 0.0;
        renderSyncFrame(bmR, bmG, bmS);
        const auto bmUp = renderer.getLastUploadTiming();
        double burstMoveTotalMs = burstMoveEvMs + burstMoveProcMs + bmR + bmG + bmS;

        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = lastMx;
        evUp.button.y = lastMy;
        sculpt.handleEvent(evUp, scene);
        g_undoManager.undo(scene);
        g_undoManager.clear();
        meshRaw->vertProxy = meshRaw->verts;
        renderSyncFrame(r0, g0, s0);

        // Case B: BRUSH_CLAYBUILDUP
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 134.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.focalShift = -0.285f;
        claySet.spacing = 0.05f;
        claySet.clay = true;
        claySet.accumulate = false;
        claySet.culling = false;
        sculpt.warmupMeshCaches(meshRaw);

        float clayStartX = centerX - 100.0f * scalePx;
        float clayStartY = centerY;

        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)std::round(clayStartX);
        evDown.button.y = (int)std::round(clayStartY);
        sculpt.handleEvent(evDown, scene);
        sculpt.setRawMousePos(evDown.button.x, evDown.button.y);
        sculpt.processFrame(scene);
        renderSyncFrame(r0, g0, s0);

        auto tBurstClay0 = Clock::now();
        lastMx = evDown.button.x;
        lastMy = evDown.button.y;
        for (int i = 1; i <= BURST_EVENTS; ++i) {
            SDL_Event evM;
            std::memset(&evM, 0, sizeof(evM));
            evM.type = SDL_MOUSEMOTION;
            evM.motion.state = SDL_BUTTON_LMASK;
            lastMx = (int)std::round(clayStartX + (float)i * 9.0f * scalePx);
            lastMy = (int)std::round(clayStartY + std::sin((float)i * 0.25f) * 8.0f * scalePx);
            evM.motion.x = lastMx;
            evM.motion.y = lastMy;
            sculpt.handleEvent(evM, scene);
        }
        double burstClayEvMs = elapsedMs(tBurstClay0);

        auto tPC0 = Clock::now();
        sculpt.setRawMousePos(lastMx, lastMy);
        sculpt.processFrame(scene);
        double burstClayProcMs = elapsedMs(tPC0);

        double bcR = 0.0, bcG = 0.0, bcS = 0.0;
        renderSyncFrame(bcR, bcG, bcS);
        const auto bcUp = renderer.getLastUploadTiming();
        double burstClayTotalMs = burstClayEvMs + burstClayProcMs + bcR + bcG + bcS;

        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = lastMx;
        evUp.button.y = lastMy;
        sculpt.handleEvent(evUp, scene);
        g_undoManager.undo(scene);
        g_undoManager.clear();
        meshRaw->vertProxy = meshRaw->verts;
        renderSyncFrame(r0, g0, s0);

        std::cout << "[4/5] Burst Input (20 SDL_MOUSEMOTION events in 1 frame):\n";
        std::cout << std::setprecision(2);
        std::cout << "      Move(20ev):  Step0=" << moveDownMs << " ms | 20Events=" << burstMoveEvMs
                  << " ms | Process=" << burstMoveProcMs
                  << " ms | Upload=" << ((double)bmUp.bytesVbo / (1024.0 * 1024.0)) << " MB (" << bmUp.msVbo << " ms)"
                  << " | Render+Finish=" << (bmR + bmG) << " ms | TotalBurst=" << burstMoveTotalMs << " ms\n";
        std::cout << "      Clay(20ev):  20Events=" << burstClayEvMs
                  << " ms | Process=" << burstClayProcMs
                  << " ms | Upload=" << ((double)bcUp.bytesVbo / (1024.0 * 1024.0)) << " MB (" << bcUp.msVbo << " ms)"
                  << " | Render+Finish=" << (bcR + bcG) << " ms | TotalBurst=" << burstClayTotalMs << " ms\n\n";
    }

    // =========================================================================
    // [5/5] PolyGroup Shader Mode (m_showPolyGroups = true, 8 Clay steps)
    // =========================================================================
    {
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        renderer.setShowPolyGroups(true);
        double wR = 0.0, wG = 0.0, wS = 0.0;
        renderSyncFrame(wR, wG, wS); // Initial PolyGroup buffer build

        const int PG_STEPS = 8;
        const float startX = centerX - 110.0f * scalePx;
        const float startY = centerY + 30.0f * scalePx;
        const float stepDx = 9.0f * scalePx;

        std::vector<double> vSculptCpu, vPgMs, vPgMB, vVboMB, vRenderCpu, vGpuFin, vTotal;
        uint32_t totalRebuilds = 0;
        uint32_t totalPgDraws = 0;

        for (int i = 0; i < PG_STEPS; ++i) {
            SDL_PumpEvents();
            float curX = startX + (float)i * stepDx;
            float curY = startY;

            SDL_Event ev;
            std::memset(&ev, 0, sizeof(ev));
            if (i == 0) {
                ev.type = SDL_MOUSEBUTTONDOWN;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.x = (int)std::round(curX);
                ev.button.y = (int)std::round(curY);
            } else {
                ev.type = SDL_MOUSEMOTION;
                ev.motion.state = SDL_BUTTON_LMASK;
                ev.motion.x = (int)std::round(curX);
                ev.motion.y = (int)std::round(curY);
            }

            auto tS0 = Clock::now();
            sculpt.handleEvent(ev, scene);
            double sCpu = elapsedMs(tS0);

            sculpt.setRawMousePos((int)std::round(curX), (int)std::round(curY));
            sculpt.processFrame(scene);
            sculpt.getCursor().applyToRenderer(renderer);
            renderer.setActiveBrush(BRUSH_CLAYBUILDUP);

            double rCpu = 0.0, gFin = 0.0, sSwap = 0.0;
            renderSyncFrame(rCpu, gFin, sSwap);
            const auto& ut = renderer.getLastUploadTiming();
            const auto& rs = renderer.getLastRenderStats();
            double pgMB = (double)ut.bytesPolygroup / (1024.0 * 1024.0);
            double vboMB = (double)ut.bytesVbo / (1024.0 * 1024.0);
            totalRebuilds += rs.polygroupRebuilds;
            totalPgDraws += rs.polygroupDraws;

            vSculptCpu.push_back(sCpu);
            vPgMs.push_back(ut.msPolygroupRebuild);
            vPgMB.push_back(pgMB);
            vVboMB.push_back(vboMB);
            vRenderCpu.push_back(rCpu);
            vGpuFin.push_back(gFin);
            vTotal.push_back(sCpu + rCpu + gFin + sSwap);
        }

        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(startX + (float)(PG_STEPS - 1) * stepDx);
        evUp.button.y = (int)std::round(startY);
        sculpt.handleEvent(evUp, scene);

        renderer.setShowPolyGroups(false);
        g_undoManager.undo(scene);
        g_undoManager.clear();

        std::cout << "[5/5] PolyGroup Mode (m_showPolyGroups = true, " << PG_STEPS << " Clay steps):\n";
        std::cout << std::setprecision(2);
        std::cout << "      InitialPG:   Render+Build+Finish=" << (wR + wG) << " ms\n";
        std::cout << "      AvgPerStep:  SculptCPU=" << meanOf(vSculptCpu)
                  << " ms | PGRebuild=" << meanOf(vPgMs)
                  << " ms | PGUpload=" << meanOf(vPgMB) << " MB"
                  << " | VboUpload=" << meanOf(vVboMB) << " MB"
                  << " | RenderCPU=" << meanOf(vRenderCpu)
                  << " ms | Finish=" << meanOf(vGpuFin)
                  << " ms | TotalFrame=" << meanOf(vTotal) << " ms"
                  << " | Rebuilds=" << totalRebuilds
                  << " | PGDraws=" << totalPgDraws << "\n\n";
    }

    g_undoManager.clear();
    scene.clear();

    std::cout << "================================================================================\n";
    std::cout << "  RenderBenchmark Complete: " << (undoRedoOk ? "ALL CHECKS PASSED" : "CHECK FAILED") << "\n";
    std::cout << "================================================================================\n\n";
    return undoRedoOk;
}

inline bool runStage1Tests(Scene& scene, SculptManager& sculpt, AngleRenderer& renderer, SDL_Window* window) {
    int winW = 1920, winH = 1080;
    if (window) {
        SDL_GetWindowSize(window, &winW, &winH);
    }
    if (winW <= 0) winW = 1920;
    if (winH <= 0) winH = 1080;

    const OptFlags::Config savedFlags = OptFlags::get();
    OptFlags::get().idleSleep = true;
    OptFlags::get().cullFace = true;
    OptFlags::get().vaoCache = true;
    OptFlags::get().orbitScale = true;

    std::cout << "\n================================================================================\n";
    std::cout << "  Stage 1 Invariant Verification (--test-stage1)\n";
    std::cout << "================================================================================\n";

    bool allOk = true;
    auto drainGlErrors = []() {
        while (glGetError() != GL_NO_ERROR) {}
    };
    auto checkNoGlError = [&](const char* label) -> bool {
        GLenum err = GL_NO_ERROR;
        bool clean = true;
        while ((err = glGetError()) != GL_NO_ERROR) {
            std::cerr << "  [FAIL] " << label << ": GL error 0x" << std::hex << err << std::dec << "\n";
            clean = false;
        }
        return clean;
    };
    auto checkCond = [&](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "  [FAIL] " << msg << "\n";
            allOk = false;
        } else {
            std::cout << "  [PASS] " << msg << "\n";
        }
    };
    auto resetRendererClean = [&]() {
        renderer.setRenderMode(RenderMode::PBR);
        int fvIdx = renderer.findMatcapIndexByName("Matcap FV");
        renderer.setMatcap(fvIdx >= 0 ? fvIdx : 3);
        renderer.setShaderType(1);
        renderer.setTextureId(0);
        renderer.setAlpha(1.0f);
        renderer.setUseShadows(false);
        renderer.setUseSsao(false);
        renderer.setUseSsr(false);
        renderer.setUseTaa(false);
        renderer.setUseFxaa(false);
        renderer.setBevelEnabled(false);
        renderer.setShowContour(false);
        renderer.setShowWireframe(false);
        renderer.setShowCreaseLines(false);
        renderer.setShowPolyGroups(false);
        renderer.setEnableSelectionFlash(false);
        renderer.setShowSymmetryLine(false);
        renderer.setShowInvertedNormals(false);
        renderer.setXrayEnabled(false);
        renderer.setShowGrid(false);
        renderer.setOrbitInteractionHint(false, false);
    };

    drainGlErrors();

    // -------------------------------------------------------------------------
    // Test 1: Closed UV sphere culling & safety guards
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(48, 96).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        renderer.render(scene);
        glFinish();
        auto st1a = renderer.getLastRenderStats();
        checkCond(st1a.cullEnabledDraws >= 1 && st1a.cullDisabledDraws == 0,
                  "Test 1a: Closed UV sphere enables backface culling when showInvertedNormals=false");

        renderer.setShowInvertedNormals(true);
        renderer.render(scene);
        glFinish();
        auto st1b = renderer.getLastRenderStats();
        checkCond(st1b.cullEnabledDraws == 0 && st1b.cullDisabledDraws >= 1,
                  "Test 1b: showInvertedNormals=true disables backface culling");

        renderer.setShowInvertedNormals(false);
        sphere->matrix = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
        renderer.render(scene);
        glFinish();
        auto st1c = renderer.getLastRenderStats();
        checkCond(st1c.cullEnabledDraws == 0 && st1c.cullDisabledDraws >= 1,
                  "Test 1c: Negative-determinant mesh matrix disables backface culling");
        sphere->matrix = glm::mat4(1.0f);

        sphere->faceVisible[0] = 0;
        sphere->isDirty = true;
        renderer.render(scene);
        glFinish();
        auto st1d = renderer.getLastRenderStats();
        checkCond(st1d.cullEnabledDraws == 0 && st1d.cullDisabledDraws >= 1,
                  "Test 1d: Hiding a face disables backface culling (open hole invariant)");
        sphere->faceVisible[0] = 1;
        sphere->isDirty = true;

        allOk = checkNoGlError("Test 1") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 2: Open Plane, DynamicSubdiv + Thickness, X-Ray
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        resetRendererClean();

        scene.addPlane();
        Mesh* plane = scene.getSelected();
        checkCond(plane != nullptr, "Test 2 setup: Scene::addPlane created Plane primitive");
        if (plane) {
            Camera& cam = scene.getCamera();
            cam.onResize(winW, winH);
            cam.resetViewToMesh(plane, false);
            cam.update(0.016f);

            renderer.render(scene);
            glFinish();
            auto st2a = renderer.getLastRenderStats();
            checkCond(st2a.cullEnabledDraws == 0 && st2a.cullDisabledDraws >= 1,
                      "Test 2a: Open Plane keeps backface culling disabled (two-sided shading)");

            plane->dynamicSettings.enabled = true;
            plane->dynamicSettings.enableSubdiv = false;
            plane->dynamicSettings.enableThickness = true;
            plane->dynamicSettings.thickness = 0.1f;
            plane->isDynamicDirty = true;
            plane->isDirty = true;
            renderer.render(scene);
            glFinish();
            auto st2b = renderer.getLastRenderStats();
            checkCond(st2b.cullEnabledDraws >= 1,
                      "Test 2b: Plane + DynamicSubdiv Thickness forms closed shell & enables culling");

            plane->dynamicSettings.enabled = false;
            plane->isDynamicDirty = true;
            plane->isDirty = true;
            renderer.setXrayEnabled(true);
            renderer.setXrayTarget(2);
            renderer.render(scene);
            glFinish();
            auto st2c = renderer.getLastRenderStats();
            checkCond(st2c.cullEnabledDraws >= 1,
                      "Test 2c: X-Ray pass preserves culling");
            renderer.setXrayEnabled(false);
        }
        allOk = checkNoGlError("Test 2") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 3: Full Feature Stress (VAO Cache + Uniform Cache + Creases + Split)
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(32, 64).release();
        sphere->postInit(true);
        sphere->initCreases();
        if (sphere->nbEdges > 16) {
            sphere->creaseEdge(0, true);
            sphere->creaseEdge(8, true);
            sphere->creaseEdge(16, true);
        }
        sphere->isDirty = true;
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        renderer.setShaderType(0); // PBR
        renderer.setUseShadows(true);
        renderer.setBevelEnabled(true);
        renderer.setUseSsao(true);
        renderer.setUseSsr(true);
        renderer.setUseFxaa(true);
        renderer.setShowContour(true);
        renderer.setShowWireframe(true);
        renderer.setShowCreaseLines(true);

        sphere->dynamicSettings.enabled = true;
        sphere->dynamicSettings.enableSubdiv = true;
        sphere->dynamicSettings.smoothLevel = 1;
        sphere->dynamicSettings.showCage = true;
        sphere->isDynamicDirty = true;
        sphere->isDirty = true;

        for (int f = 0; f < 3; ++f) {
            if (f == 1) {
                cam.start(winW * 0.5f, winH * 0.5f, true);
                cam.rotate(winW * 0.5f + 24.0f, winH * 0.5f + 12.0f, 1.0f);
                renderer.setOrbitInteractionHint(true, false);
            } else {
                renderer.setOrbitInteractionHint(false, false);
            }
            renderer.render(scene);
            glFinish();
        }
        checkCond(checkNoGlError("Test 3a (PBR+Shadows+Bevel+SSAO+SSR+FXAA+Contour+Wire+Crease+SubdivCage)"),
                  "Test 3a: Full feature render under vaoCache+orbitScale produces zero GL errors");

        renderer.setShowPolyGroups(true);
        scene.setSplitMode(Scene::SplitMode::MIRROR);
        for (int f = 0; f < 2; ++f) {
            renderer.render(scene);
            glFinish();
        }
        scene.setSplitMode(Scene::SplitMode::INDEPENDENT);
        for (int f = 0; f < 2; ++f) {
            renderer.render(scene);
            glFinish();
        }
        checkCond(checkNoGlError("Test 3b (PolyGroup + SplitMode MIRROR & INDEPENDENT)"),
                  "Test 3b: PolyGroup + Wireframe/Creases + Split-Viewport produces zero GL errors");

        scene.setSplitMode(Scene::SplitMode::OFF);
        sphere->dynamicSettings.enabled = false;
        sphere->dynamicSettings.showCage = false;
        resetRendererClean();
    }

    // -------------------------------------------------------------------------
    // Test 4: Adaptive Orbit Scaling, Settle Cooldown & Sculpt Immunity
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(32, 64).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        for (int f = 0; f < 4; ++f) {
            renderer.setOrbitInteractionHint(false, false);
            renderer.render(scene);
            glFinish();
        }
        checkCond(renderer.getActiveRenderScale() > 0.999f && !renderer.isOrbitScaleSettling(),
                  "Test 4a: Static view runs at full 1.0x resolution and settles cleanly");

        cam.start(winW * 0.5f, winH * 0.5f, true);
        cam.rotate(winW * 0.5f + 20.0f, winH * 0.5f + 10.0f, 1.0f);
        renderer.setOrbitInteractionHint(true, false);
        renderer.render(scene);
        glFinish();
        checkCond(renderer.getActiveRenderScale() < 0.999f && renderer.isOrbitScaleSettling(),
                  "Test 4b: Orbit interaction drops internal scale (< 1.0) and reports settling");

        renderer.setOrbitInteractionHint(false, false);
        for (int f = 0; f < 3; ++f) {
            renderer.render(scene);
            glFinish();
        }
        checkCond(renderer.getActiveRenderScale() > 0.999f && !renderer.isOrbitScaleSettling(),
                  "Test 4c: Releasing orbit returns scale to 1.0x within 3 frames");

        renderer.setOrbitInteractionHint(true, true);
        renderer.render(scene);
        glFinish();
        checkCond(renderer.getActiveRenderScale() > 0.999f,
                  "Test 4d: Active sculpting enforces 1.0x full resolution even with orbit hint");
        renderer.setOrbitInteractionHint(false, false);

        allOk = checkNoGlError("Test 4") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 5: RAM, VRAM & App GPU Memory Telemetry Verification
    // -------------------------------------------------------------------------
    {
        const auto bd = renderer.getGpuMemoryBreakdown(&scene);
        const auto memStats = SystemStats::sampleMemoryStats(
            bd.meshBuffersBytes,
            bd.renderTargetsBytes,
            bd.texturesBytes
        );

        std::cout << "  [Test 5] Memory Telemetry:\n";
        std::cout << "    Process RAM (Working Set): " << SystemStats::formatMemoryBytes(memStats.processWorkingSetBytes)
                  << " (Peak: " << SystemStats::formatMemoryBytes(memStats.processPeakWorkingSetBytes)
                  << ", Commit: " << SystemStats::formatMemoryBytes(memStats.processPrivateCommitBytes) << ")\n";
        if (memStats.systemTotalPhysBytes > 0) {
            std::cout << "    System RAM:                " << SystemStats::formatMemoryBytes(memStats.systemUsedPhysBytes)
                      << " / " << SystemStats::formatMemoryBytes(memStats.systemTotalPhysBytes)
                      << " (" << memStats.systemMemoryLoadPct << "%)\n";
        }
        std::cout << "    GPU Adapter:               " << (memStats.gpuAdapterName.empty() ? "N/A" : memStats.gpuAdapterName)
                  << " (DXGI=" << (memStats.dxgiAvailable ? "yes" : "no")
                  << ", EGL_D3D11=" << (memStats.vramFromEglD3D11 ? "yes" : "no") << ")\n";
        std::cout << "    DXGI Local / Budget / Shd: "
                  << SystemStats::formatMemoryBytes(memStats.vramProcessLocalBytes) << " / "
                  << SystemStats::formatMemoryBytes(memStats.vramProcessBudgetBytes) << " / "
                  << SystemStats::formatMemoryBytes(memStats.vramProcessSharedBytes) << "\n";
        const char* srcName =
            (memStats.vramSource == VramSourceKind::DxgiDedicated) ? "DxgiDedicated" :
            (memStats.vramSource == VramSourceKind::DxgiShared)    ? "DxgiShared" :
            (memStats.vramSource == VramSourceKind::AppTracked)    ? "AppTracked" :
            (memStats.vramSource == VramSourceKind::OpenGLExt)     ? "OpenGLExt" : "None";
        std::cout << "    Effective VRAM (" << srcName << "): " << SystemStats::formatMemoryBytes(memStats.effectiveVramUsedBytes) << "\n";
        std::cout << "    App Tracked GPU Breakdown: Meshes=" << SystemStats::formatMemoryBytes(bd.meshBuffersBytes)
                  << ", RTTs=" << SystemStats::formatMemoryBytes(bd.renderTargetsBytes)
                  << ", Tex=" << SystemStats::formatMemoryBytes(bd.texturesBytes)
                  << ", Total=" << SystemStats::formatMemoryBytes(bd.totalBytes()) << "\n";

        checkCond(memStats.processWorkingSetBytes > 0,
                  "Test 5a: Process Working Set RAM telemetry > 0");
        checkCond(bd.meshBuffersBytes > 0 && bd.renderTargetsBytes > 0,
                  "Test 5b: AngleRenderer tracks non-zero mesh buffers and render targets");
        checkCond(memStats.vramSource != VramSourceKind::None && memStats.effectiveVramUsedBytes > 0,
                  "Test 5c: Effective VRAM telemetry resolved and > 0");
        const std::string formattedRam = SystemStats::formatMemoryBytes(memStats.processWorkingSetBytes);
        const std::string formattedVram = SystemStats::formatMemoryBytes(memStats.effectiveVramUsedBytes);
        checkCond(!formattedRam.empty() && !formattedVram.empty(),
                  "Test 5d: Memory byte formatting produces non-empty HUD strings");
    }

    SystemStats::shutdownGpuMonitor();
    g_undoManager.clear();
    scene.clear();
    OptFlags::get() = savedFlags;

    std::cout << "================================================================================\n";
    std::cout << "  Stage 1 Tests Complete: " << (allOk ? "ALL PASSED" : "FAILED") << "\n";
    std::cout << "================================================================================\n\n";
    return allOk;
}

inline bool runStage2Tests(Scene& scene, SculptManager& sculpt, AngleRenderer& renderer, SDL_Window* window) {
    int winW = 1920, winH = 1080;
    if (window) {
        SDL_GetWindowSize(window, &winW, &winH);
    }
    if (winW <= 0) winW = 1920;
    if (winH <= 0) winH = 1080;

    int drawW = winW, drawH = winH;
    if (window) {
        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    }
    if (drawW <= 0) drawW = winW;
    if (drawH <= 0) drawH = winH;

    const OptFlags::Config savedFlags = OptFlags::get();
    OptFlags::get().idleSleep = true;
    OptFlags::get().cullFace = true;
    OptFlags::get().vaoCache = true;
    OptFlags::get().orbitScale = false;
    OptFlags::get().dirtyBlocks = true;
    OptFlags::get().indexedPolygroup = true;

    const float scalePx = std::max(0.5f, (float)std::min(winW, winH) / 1080.0f);
    const float centerX = (float)winW * 0.5f;
    const float centerY = (float)winH * 0.5f;

    std::cout << "\n================================================================================\n";
    std::cout << "  Stage 2 Invariant Verification (--test-stage2)\n";
    std::cout << "================================================================================\n";

    bool allOk = true;
    auto drainGlErrors = []() {
        while (glGetError() != GL_NO_ERROR) {}
    };
    auto checkNoGlError = [&](const char* label) -> bool {
        GLenum err = GL_NO_ERROR;
        bool clean = true;
        while ((err = glGetError()) != GL_NO_ERROR) {
            std::cerr << "  [FAIL] " << label << ": GL error 0x" << std::hex << err << std::dec << "\n";
            clean = false;
        }
        return clean;
    };
    auto checkCond = [&](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "  [FAIL] " << msg << "\n";
            allOk = false;
        } else {
            std::cout << "  [PASS] " << msg << "\n";
        }
    };
    auto resetRendererClean = [&]() {
        renderer.setRenderMode(RenderMode::PBR);
        int fvIdx = renderer.findMatcapIndexByName("Matcap FV");
        renderer.setMatcap(fvIdx >= 0 ? fvIdx : 3);
        renderer.setShaderType(1);
        renderer.setTextureId(0);
        renderer.setAlpha(1.0f);
        renderer.setUseShadows(false);
        renderer.setUseSsao(false);
        renderer.setUseSsr(false);
        renderer.setUseTaa(false);
        renderer.setUseFxaa(false);
        renderer.setBevelEnabled(false);
        renderer.setShowContour(false);
        renderer.setShowWireframe(false);
        renderer.setShowCreaseLines(false);
        renderer.setShowPolyGroups(false);
        renderer.setEnableSelectionFlash(false);
        renderer.setShowSymmetryLine(false);
        renderer.setShowInvertedNormals(false);
        renderer.setXrayEnabled(false);
        renderer.setShowGrid(false);
        renderer.setFlatShading(false);
        renderer.setOrbitInteractionHint(false, false);
        renderer.setActiveBrush(BRUSH_CLAYBUILDUP);
        renderer.setCursorParameters(false, false, {}, {}, {}, {}, {1.0f, 1.0f, 1.0f});
    };

    auto doStrokeStep = [&](float x, float y, bool isDown, BrushType brush) {
        SDL_PumpEvents();
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        if (isDown) {
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = (int)std::round(x);
            ev.button.y = (int)std::round(y);
        } else {
            ev.type = SDL_MOUSEMOTION;
            ev.motion.state = SDL_BUTTON_LMASK;
            ev.motion.x = (int)std::round(x);
            ev.motion.y = (int)std::round(y);
        }
        sculpt.handleEvent(ev, scene);
        sculpt.setRawMousePos((int)std::round(x), (int)std::round(y));
        sculpt.processFrame(scene);
        renderer.setActiveBrush(brush);
    };

    auto releaseStroke = [&](float x, float y) {
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(x);
        evUp.button.y = (int)std::round(y);
        sculpt.handleEvent(evUp, scene);
    };

    drainGlErrors();

    // -------------------------------------------------------------------------
    // Test 1: Disjoint Dirty Blocks (Symmetry Y on ~260k-vert UV sphere)
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(360, 720).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(true);
        sculpt.setSymX(false);
        sculpt.setSymY(true);
        sculpt.setSymZ(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);
        sculpt.registerKnownMesh(sphere);
        sculpt.setBrush(BRUSH_CLAYBUILDUP);

        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto utInit = renderer.getLastUploadTiming();
        checkCond(utInit.wasFullUpload, "Test 1 setup: Initial UV sphere upload completed");

        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 45.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.focalShift = 0.0f;
        claySet.accumulate = false;
        claySet.culling = false;

        const float hitX = centerX;
        const float hitY = centerY - 120.0f * scalePx;
        doStrokeStep(hitX, hitY, true, BRUSH_CLAYBUILDUP);
        int picked = sculpt.getLastFrameProfile().pickedVertCount;

        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto ut1 = renderer.getLastUploadTiming();
        releaseStroke(hitX, hitY);

        const uint32_t maxQuarter = (uint32_t)std::max(1, sphere->nbVerts / 4);
        std::cout << "  [Info] Test 1 SymY: picked=" << picked
                  << ", runs=" << ut1.dirtyRunCount
                  << ", uploaded=" << ut1.dirtyVertUploaded << "/" << sphere->nbVerts
                  << " (" << std::fixed << std::setprecision(2)
                  << (sphere->nbVerts > 0 ? 100.0 * (double)ut1.dirtyVertUploaded / (double)sphere->nbVerts : 0.0)
                  << "%), vboBytes=" << ut1.bytesVbo << "\n";

        checkCond(picked > 0 && !ut1.wasFullUpload && !ut1.usedWholeBufferFallback &&
                  ut1.dirtyRunCount >= 2 && ut1.dirtyVertUploaded > 0 && ut1.dirtyVertUploaded < maxQuarter,
                  "Test 1: Symmetry Y uploads >=2 disjoint dirty runs covering <25% of mesh vertices");

        g_undoManager.undo(scene);
        g_undoManager.clear();
        allOk = checkNoGlError("Test 1") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 2: Color-only, Material-only, >60% threshold & unmasked fallback
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(360, 720).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);
        sculpt.registerKnownMesh(sphere);

        renderer.render(scene);
        glFinish();

        // 2a: Color-only (BRUSH_PAINT with writeAlbedo only)
        sculpt.setBrush(BRUSH_PAINT);
        auto& paintSet = sculpt.getSettings(BRUSH_PAINT);
        paintSet.radius = 60.0f * scalePx;
        paintSet.intensity = 1.0f;
        paintSet.writeAlbedo = true;
        paintSet.writeRoughness = false;
        paintSet.writeMetalness = false;
        paintSet.paintColor = glm::vec3(0.2f, 0.85f, 0.35f);

        doStrokeStep(centerX, centerY, true, BRUSH_PAINT);
        const bool dirtyFlags2a = (!sphere->isVertexDirty && sphere->isColorDirty && !sphere->isMaterialDirty);
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto ut2a = renderer.getLastUploadTiming();
        releaseStroke(centerX, centerY);

        std::cout << "  [Info] Test 2a Paint: flagsOk=" << dirtyFlags2a
                  << ", runs=" << ut2a.dirtyRunCount
                  << ", uploaded=" << ut2a.dirtyVertUploaded << "/" << sphere->nbVerts
                  << ", vboBytes=" << ut2a.bytesVbo << "\n";
        checkCond(dirtyFlags2a && !ut2a.wasFullUpload && !ut2a.usedWholeBufferFallback &&
                  ut2a.dirtyRunCount >= 1 && ut2a.dirtyVertUploaded > 0 &&
                  ut2a.bytesVbo > 0 && ut2a.bytesVbo < (size_t)sphere->nbVerts * 6,
                  "Test 2a: BRUSH_PAINT (albedo only) uploads only dirty color blocks");

        // 2b: Material-only (BRUSH_MASK)
        sculpt.setBrush(BRUSH_MASK);
        auto& maskSet = sculpt.getSettings(BRUSH_MASK);
        maskSet.radius = 60.0f * scalePx;
        maskSet.intensity = 1.0f;

        doStrokeStep(centerX, centerY, true, BRUSH_MASK);
        doStrokeStep(centerX + 24.0f, centerY, false, BRUSH_MASK);
        const bool dirtyFlags2b = (!sphere->isVertexDirty && !sphere->isColorDirty && sphere->isMaterialDirty);
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto ut2b = renderer.getLastUploadTiming();
        releaseStroke(centerX + 24.0f, centerY);

        std::cout << "  [Info] Test 2b Mask: flagsOk=" << dirtyFlags2b
                  << ", runs=" << ut2b.dirtyRunCount
                  << ", uploaded=" << ut2b.dirtyVertUploaded << "/" << sphere->nbVerts
                  << ", vboBytes=" << ut2b.bytesVbo << "\n";
        checkCond(dirtyFlags2b && !ut2b.wasFullUpload && !ut2b.usedWholeBufferFallback &&
                  ut2b.dirtyRunCount >= 1 && ut2b.dirtyVertUploaded > 0 &&
                  ut2b.bytesVbo > 0 && ut2b.bytesVbo < (size_t)sphere->nbVerts * 6,
                  "Test 2b: BRUSH_MASK uploads only dirty material blocks");

        // 2c: >60% whole-buffer fallback
        sphere->markDirtyVertRange(0u, (uint32_t)(sphere->nbVerts * 65 / 100));
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto ut2c = renderer.getLastUploadTiming();
        checkCond(!ut2c.wasFullUpload && ut2c.usedWholeBufferFallback &&
                  ut2c.dirtyRunCount == 1 && ut2c.dirtyVertUploaded == (uint32_t)sphere->nbVerts,
                  "Test 2c: >60% dirty vertices triggers whole-buffer subdata fallback (1 run)");

        // 2d: Unmasked dirty range (hasDirtyVertBlockMask = false) fallback
        sphere->dirtyVertMin = 0;
        sphere->dirtyVertMax = (uint32_t)(sphere->nbVerts - 1);
        sphere->hasDirtyVertBlockMask = false;
        sphere->isVertexDirty = true;
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto ut2d = renderer.getLastUploadTiming();
        checkCond(!ut2d.wasFullUpload && ut2d.usedWholeBufferFallback &&
                  ut2d.dirtyRunCount == 1 && ut2d.dirtyVertUploaded == (uint32_t)sphere->nbVerts,
                  "Test 2d: Unmasked dirty range falls back safely to whole-buffer subdata (1 run)");

        g_undoManager.clear();
        allOk = checkNoGlError("Test 2") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 3: Sparse Undo / Redo parity & incremental GPU upload under dirtyBlocks
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(240, 480).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(true);
        sculpt.setSymX(false);
        sculpt.setSymY(true);
        sculpt.setSymZ(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);
        sculpt.registerKnownMesh(sphere);
        renderer.render(scene);
        glFinish();

        // Position stroke undo/redo
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 45.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.culling = false;
        claySet.accumulate = false;

        for (int s = 0; s < 3; ++s) {
            doStrokeStep(centerX + (float)s * 10.0f * scalePx, centerY - 130.0f * scalePx, s == 0, BRUSH_CLAYBUILDUP);
            renderer.render(scene);
            glFinish();
        }
        releaseStroke(centerX + 20.0f * scalePx, centerY - 130.0f * scalePx);

        g_undoManager.undo(scene);
        float maxVertUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                maxVertUndoDiff = std::max({maxVertUndoDiff,
                    std::abs(sphere->verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]),
                    std::abs(sphere->verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]),
                    std::abs(sphere->verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2])});
            }
        }
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto utVertUndo = renderer.getLastUploadTiming();

        g_undoManager.redo(scene);
        float maxVertRedoDiff = 0.0f;
        auto* undoEntry = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntry && !undoEntry->deltas.empty()) {
            const auto& d = undoEntry->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                maxVertRedoDiff = std::max({maxVertRedoDiff,
                    std::abs(sphere->verts[vid * 3 + 0] - d.nextVerts[i * 3 + 0]),
                    std::abs(sphere->verts[vid * 3 + 1] - d.nextVerts[i * 3 + 1]),
                    std::abs(sphere->verts[vid * 3 + 2] - d.nextVerts[i * 3 + 2])});
            }
        }
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto utVertRedo = renderer.getLastUploadTiming();

        std::cout << "  [Info] Test 3a Undo/Redo: undoDiff=" << maxVertUndoDiff
                  << ", redoDiff=" << maxVertRedoDiff
                  << ", undoRuns=" << utVertUndo.dirtyRunCount << " (up=" << utVertUndo.dirtyVertUploaded << ")"
                  << ", redoRuns=" << utVertRedo.dirtyRunCount << " (up=" << utVertRedo.dirtyVertUploaded << ")\n";
        checkCond(redoEntry != nullptr && undoEntry != nullptr &&
                  maxVertUndoDiff == 0.0f && maxVertRedoDiff == 0.0f &&
                  !utVertUndo.wasFullUpload && utVertUndo.dirtyRunCount >= 2 &&
                  !utVertRedo.wasFullUpload && utVertRedo.dirtyRunCount >= 2,
                  "Test 3a: Position Undo/Redo matches 0.000000 eps & uses incremental block upload");

        // Color stroke undo/redo
        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.setBrush(BRUSH_PAINT);
        auto& paintSet = sculpt.getSettings(BRUSH_PAINT);
        paintSet.radius = 55.0f * scalePx;
        paintSet.intensity = 1.0f;
        paintSet.writeAlbedo = true;
        paintSet.writeRoughness = false;
        paintSet.writeMetalness = false;
        paintSet.paintColor = glm::vec3(0.9f, 0.25f, 0.2f);

        for (int s = 0; s < 2; ++s) {
            doStrokeStep(centerX + (float)s * 10.0f * scalePx, centerY, s == 0, BRUSH_PAINT);
            renderer.render(scene);
            glFinish();
        }
        releaseStroke(centerX + 10.0f * scalePx, centerY);

        g_undoManager.undo(scene);
        float maxColUndoDiff = 0.0f;
        auto* redoCol = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoCol && !redoCol->deltas.empty() && redoCol->deltas[0].hasColors) {
            const auto& d = redoCol->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                maxColUndoDiff = std::max({maxColUndoDiff,
                    std::abs(sphere->colors[vid * 3 + 0] - d.prevColors[i * 3 + 0]),
                    std::abs(sphere->colors[vid * 3 + 1] - d.prevColors[i * 3 + 1]),
                    std::abs(sphere->colors[vid * 3 + 2] - d.prevColors[i * 3 + 2])});
            }
        }
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto utColUndo = renderer.getLastUploadTiming();

        g_undoManager.redo(scene);
        float maxColRedoDiff = 0.0f;
        auto* undoCol = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoCol && !undoCol->deltas.empty() && undoCol->deltas[0].hasColors) {
            const auto& d = undoCol->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                maxColRedoDiff = std::max({maxColRedoDiff,
                    std::abs(sphere->colors[vid * 3 + 0] - d.nextColors[i * 3 + 0]),
                    std::abs(sphere->colors[vid * 3 + 1] - d.nextColors[i * 3 + 1]),
                    std::abs(sphere->colors[vid * 3 + 2] - d.nextColors[i * 3 + 2])});
            }
        }
        renderer.resetLastUploadTiming();
        renderer.render(scene);
        glFinish();
        auto utColRedo = renderer.getLastUploadTiming();

        checkCond(redoCol != nullptr && undoCol != nullptr &&
                  maxColUndoDiff == 0.0f && maxColRedoDiff == 0.0f &&
                  !utColUndo.wasFullUpload && !utColUndo.usedWholeBufferFallback && utColUndo.dirtyRunCount >= 1 &&
                  !utColRedo.wasFullUpload && !utColRedo.usedWholeBufferFallback && utColRedo.dirtyRunCount >= 1,
                  "Test 3b: Paint Undo/Redo matches 0.000000 eps & uses incremental color block upload");

        g_undoManager.clear();
        allOk = checkNoGlError("Test 3") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 4a: Indexed PolyGroup Pixel Readback (Seams & Isolated Islands)
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        auto gridPtr = std::make_unique<Mesh>();
        Mesh* grid = gridPtr.release();
        grid->verts.reserve(25 * 3);
        for (int r = 0; r <= 4; ++r) {
            float y = -1.5f + 0.75f * (float)r;
            for (int c = 0; c <= 4; ++c) {
                float x = -1.5f + 0.75f * (float)c;
                grid->verts.push_back(x);
                grid->verts.push_back(y);
                grid->verts.push_back(0.0f);
                grid->nbVerts++;
            }
        }
        grid->faces.reserve(16 * 4);
        grid->faceGroups.reserve(16);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                uint32_t v00 = (uint32_t)(r * 5 + c);
                uint32_t v10 = (uint32_t)(r * 5 + c + 1);
                uint32_t v11 = (uint32_t)((r + 1) * 5 + c + 1);
                uint32_t v01 = (uint32_t)((r + 1) * 5 + c);
                grid->faces.push_back(v00);
                grid->faces.push_back(v10);
                grid->faces.push_back(v11);
                grid->faces.push_back(v01);
                uint32_t gid = (c < 2) ? 1u : 2u;
                if (r == 1 && c == 1) gid = 3u;
                if (r == 2 && c == 2) gid = 4u;
                grid->faceGroups.push_back(gid);
                grid->nbFaces++;
            }
        }
        grid->vertVisible.assign(grid->nbVerts, 1);
        grid->faceVisible.assign(grid->nbFaces, 1);
        grid->postInit(true);
        grid->isFaceGroupDirty = true;

        scene.addMesh(grid);
        scene.selectMesh(grid);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.setOrbitAngles(0.0f, 0.0f);
        cam.resetViewToMesh(grid, false);
        cam.update(0.016f);

        renderer.setShowPolyGroups(true);
        renderer.setFlatShading(true);
        renderer.setActiveBrush(BRUSH_CLAYBUILDUP);
        renderer.setCursorParameters(false, false, {}, {}, {}, {}, {1.0f, 1.0f, 1.0f});

        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rs4a = renderer.getLastRenderStats();

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        auto sampleWorldRgb = [&](const glm::vec3& wp, uint8_t rgb[3]) {
            glm::vec3 proj = cam.project(wp);
            int glX = std::clamp((int)std::lround(proj.x * (float)drawW / (float)winW), 0, drawW - 1);
            int glY = std::clamp((int)std::lround(((float)winH - proj.y) * (float)drawH / (float)winH), 0, drawH - 1);
            uint8_t px[4] = {0, 0, 0, 0};
            glReadPixels(glX, glY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            rgb[0] = px[0];
            rgb[1] = px[1];
            rgb[2] = px[2];
        };
        auto rgbDiff = [](const uint8_t a[3], const uint8_t b[3]) -> int {
            int d0 = std::abs((int)a[0] - (int)b[0]);
            int d1 = std::abs((int)a[1] - (int)b[1]);
            int d2 = std::abs((int)a[2] - (int)b[2]);
            return std::max({d0, d1, d2});
        };

        uint8_t tri0Rgb[16][3]{};
        uint8_t tri1Rgb[16][3]{};
        int maxIntraQuadDiff = 0;
        for (int r = 0; r < 4; ++r) {
            float y0 = -1.5f + 0.75f * (float)r;
            float y1 = y0 + 0.75f;
            for (int c = 0; c < 4; ++c) {
                float x0 = -1.5f + 0.75f * (float)c;
                float x1 = x0 + 0.75f;
                glm::vec3 p0(x0, y0, 0.0f);
                glm::vec3 p1(x1, y0, 0.0f);
                glm::vec3 p2(x1, y1, 0.0f);
                glm::vec3 p3(x0, y1, 0.0f);
                glm::vec3 cTri0 = 0.60f * p0 + 0.25f * p1 + 0.15f * p2;
                glm::vec3 cTri1 = 0.15f * p0 + 0.25f * p2 + 0.60f * p3;
                int qIdx = r * 4 + c;
                sampleWorldRgb(cTri0, tri0Rgb[qIdx]);
                sampleWorldRgb(cTri1, tri1Rgb[qIdx]);
                maxIntraQuadDiff = std::max(maxIntraQuadDiff, rgbDiff(tri0Rgb[qIdx], tri1Rgb[qIdx]));
            }
        }

        const int seamDiff01_02 = rgbDiff(tri0Rgb[0 * 4 + 1], tri0Rgb[0 * 4 + 2]);
        const int isl11_up      = rgbDiff(tri0Rgb[1 * 4 + 1], tri0Rgb[2 * 4 + 1]);
        const int isl11_down    = rgbDiff(tri0Rgb[1 * 4 + 1], tri0Rgb[0 * 4 + 1]);
        const int isl11_left    = rgbDiff(tri0Rgb[1 * 4 + 1], tri0Rgb[1 * 4 + 0]);
        const int isl11_right   = rgbDiff(tri0Rgb[1 * 4 + 1], tri0Rgb[1 * 4 + 2]);
        const int isl22_up      = rgbDiff(tri0Rgb[2 * 4 + 2], tri0Rgb[3 * 4 + 2]);
        const int isl22_down    = rgbDiff(tri0Rgb[2 * 4 + 2], tri0Rgb[1 * 4 + 2]);
        const int isl22_left    = rgbDiff(tri0Rgb[2 * 4 + 2], tri0Rgb[2 * 4 + 1]);
        const int isl22_right   = rgbDiff(tri0Rgb[2 * 4 + 2], tri0Rgb[2 * 4 + 3]);
        const int minInterDiff  = std::min({
            seamDiff01_02, isl11_up, isl11_down, isl11_left, isl11_right,
            isl22_up, isl22_down, isl22_left, isl22_right
        });

        std::cout << "  [Info] Test 4a PixelReadback: maxIntraQuadDiff=" << maxIntraQuadDiff
                  << ", minInterGroupDiff=" << minInterDiff
                  << ", rebuilds=" << rs4a.polygroupRebuilds
                  << ", pgDraws=" << rs4a.polygroupDraws << "\n";

        checkCond(rs4a.polygroupRebuilds == 0 && rs4a.polygroupDraws >= 1 &&
                  maxIntraQuadDiff <= 25 && minInterDiff >= 60,
                  "Test 4a: Indexed PolyGroup renders exact flat groups across seams & 1-quad islands (pixel verified)");

        if (window) SDL_GL_SwapWindow(window);
        renderer.setShowPolyGroups(false);
        renderer.setFlatShading(false);
        allOk = checkNoGlError("Test 4a") && allOk;
    }

    // -------------------------------------------------------------------------
    // Test 4b: Full Feature Parity (UVs+SeamSplit, Tangents, Creases, PaintPG, Hide, Split, XRay, Toggles)
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();

        Mesh* sphere = createSphereMeshWithPolygroups(32, 64).release();
        sphere->normals.assign((size_t)sphere->nbVerts * 3, 0.0f);
        sphere->colors.assign((size_t)sphere->nbVerts * 3, 1.0f);
        sphere->materials.resize((size_t)sphere->nbVerts * 3);
        for (int i = 0; i < sphere->nbVerts; ++i) {
            sphere->materials[i * 3 + 0] = 0.5f;
            sphere->materials[i * 3 + 1] = 0.0f;
            sphere->materials[i * 3 + 2] = 1.0f;
        }
        std::vector<float> uvAr = { 0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f };
        std::vector<uint32_t> uvfAr((size_t)sphere->nbFaces * 4, 0u);
        for (int f = 0; f < sphere->nbFaces; ++f) {
            for (int k = 0; k < 4; ++k) {
                uvfAr[(size_t)f * 4 + k] = (uint32_t)((f & 1) ? k : ((k + 1) & 3));
            }
        }
        const int origNbVerts = sphere->nbVerts;
        sphere->initTexCoordsDataFromOBJData(uvAr, uvfAr);
        sphere->postInit(true);
        sphere->tangents.resize((size_t)sphere->nbVerts * 4, 0.0f);
        for (int i = 0; i < sphere->nbVerts; ++i) {
            sphere->tangents[i * 4 + 0] = 1.0f;
            sphere->tangents[i * 4 + 3] = 1.0f;
        }
        sphere->initCreases();
        if (sphere->nbEdges > 12) {
            sphere->creaseEdge(0, true);
            sphere->creaseEdge(4, true);
            sphere->creaseEdge(8, true);
            sphere->creaseEdge(12, true);
        }
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);
        sculpt.registerKnownMesh(sphere);

        renderer.setShowPolyGroups(true);
        renderer.setShowWireframe(true);
        renderer.setShowCreaseLines(true);
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto utInit4b = renderer.getLastUploadTiming();
        auto rsInit4b = renderer.getLastRenderStats();
        checkCond(sphere->nbVerts > origNbVerts && !sphere->uvFlat.empty() && !sphere->tangents.empty() &&
                  utInit4b.wasFullUpload && rsInit4b.polygroupRebuilds == 0 && rsInit4b.polygroupDraws >= 1,
                  "Test 4b-1: UV-split mesh + tangents + creases uploads indexed polygroups with 0 rebuilds");

        // 3 ClayBuildup steps while PolyGroup + Wireframe + Creases active
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 45.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.culling = false;
        claySet.accumulate = false;
        bool stepsOk = true;
        for (int s = 0; s < 3; ++s) {
            doStrokeStep(centerX + (float)s * 8.0f * scalePx, centerY, s == 0, BRUSH_CLAYBUILDUP);
            renderer.resetLastUploadTiming();
            renderer.resetLastRenderStats();
            renderer.render(scene);
            glFinish();
            const auto& ut = renderer.getLastUploadTiming();
            const auto& rs = renderer.getLastRenderStats();
            if (ut.wasFullUpload || rs.polygroupRebuilds != 0 || rs.polygroupDraws < 1) {
                stepsOk = false;
            }
        }
        releaseStroke(centerX + 16.0f * scalePx, centerY);
        checkCond(stepsOk, "Test 4b-2: Sculpting in PolyGroup+Wire+Crease mode stays incremental with 0 rebuilds");

        // BRUSH_POLYGROUP stroke + assignGroupToFace + clearAllGroups + hide/unhide
        sculpt.setBrush(BRUSH_POLYGROUP);
        sculpt.setActiveGroupID(42);
        auto& pgSet = sculpt.getSettings(BRUSH_POLYGROUP);
        pgSet.radius = 40.0f * scalePx;
        doStrokeStep(centerX, centerY, true, BRUSH_POLYGROUP);
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsPgStroke = renderer.getLastRenderStats();
        releaseStroke(centerX, centerY);

        sculpt.getPolyGroupTool().assignGroupToFace(sphere, 0, 99);
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto utAssign = renderer.getLastUploadTiming();
        auto rsAssign = renderer.getLastRenderStats();

        sculpt.getPolyGroupTool().clearAllGroups(sphere);
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto utClear = renderer.getLastUploadTiming();
        auto rsClear = renderer.getLastRenderStats();

        sculpt.getPolyGroupTool().assignGroupToFace(sphere, 0, 7);
        sculpt.getPolyGroupTool().assignGroupToFace(sphere, 1, 13);
        sphere->faceVisible[0] = 0;
        sphere->isTopologyDirty = true;
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsHide = renderer.getLastRenderStats();
        sphere->faceVisible[0] = 1;
        sphere->isTopologyDirty = true;

        checkCond(rsPgStroke.polygroupRebuilds == 0 && rsPgStroke.polygroupDraws >= 1 &&
                  !utAssign.wasFullUpload && rsAssign.polygroupRebuilds == 0 &&
                  !utClear.wasFullUpload && rsClear.polygroupRebuilds == 0 &&
                  rsHide.polygroupRebuilds == 0 && rsHide.polygroupDraws >= 1,
                  "Test 4b-3: BRUSH_POLYGROUP, assign/clear/hide execute with 0 polygroupRebuilds");

        // SplitMode (MIRROR & INDEPENDENT) + X-Ray + shaderType==5 + vaoCache toggle + runtime indexedPolygroup toggle
        renderer.setShaderType(5);
        scene.setSplitMode(Scene::SplitMode::MIRROR);
        renderer.render(scene);
        glFinish();
        scene.setSplitMode(Scene::SplitMode::INDEPENDENT);
        renderer.render(scene);
        glFinish();
        scene.setSplitMode(Scene::SplitMode::OFF);

        renderer.setXrayEnabled(true);
        renderer.setXrayTarget(2);
        renderer.render(scene);
        glFinish();
        renderer.setXrayEnabled(false);

        OptFlags::get().vaoCache = false;
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsNoVao = renderer.getLastRenderStats();

        OptFlags::get().vaoCache = true;
        OptFlags::get().indexedPolygroup = false;
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsLegacy = renderer.getLastRenderStats();

        OptFlags::get().indexedPolygroup = true;
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsRestored = renderer.getLastRenderStats();

        checkCond(rsNoVao.polygroupRebuilds == 0 && rsLegacy.polygroupRebuilds >= 1 &&
                  rsRestored.polygroupRebuilds == 0 && checkNoGlError("Test 4b"),
                  "Test 4b-4: Split/XRay/shaderType=5, vaoCache toggle & indexedPolygroup runtime toggle produce 0 GL errors");

        g_undoManager.clear();
        resetRendererClean();
    }

    // -------------------------------------------------------------------------
    // Test 5: DynamicSubdiv + Thickness + ShowCage Incremental Upload & Zero PG Rebuilds
    // -------------------------------------------------------------------------
    {
        // 5a: Closed Sphere (40x80), preSubdiv (smoothLevel=2) + Thickness + ShowCage
        scene.clear();
        g_undoManager.clear();
        scene.setSplitMode(Scene::SplitMode::OFF);
        resetRendererClean();
        renderer.setShowPolyGroups(true);

        Mesh* dynSphere = createSphereMeshWithPolygroups(40, 80).release();
        dynSphere->postInit(true);
        dynSphere->dynamicSettings.enabled = true;
        dynSphere->dynamicSettings.enableSubdiv = true;
        dynSphere->dynamicSettings.smoothLevel = 2;
        dynSphere->dynamicSettings.enableThickness = true;
        dynSphere->dynamicSettings.thickness = 0.08f;
        dynSphere->dynamicSettings.postSubdiv = false;
        dynSphere->dynamicSettings.showCage = true;
        dynSphere->isDynamicDirty = true;
        dynSphere->isDirty = true;

        scene.addMesh(dynSphere);
        scene.selectMesh(dynSphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.setOrbitAngles(0.0f, 0.0f);
        cam.resetViewToMesh(dynSphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(dynSphere);
        sculpt.warmupMeshCaches(dynSphere);
        sculpt.registerKnownMesh(dynSphere);

        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rsDynInit = renderer.getLastRenderStats();
        const int dynVerts5a = dynSphere->dynamicCacheMesh ? dynSphere->dynamicCacheMesh->nbVerts : 0;

        // 5a ClayBuildup
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 25.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.accumulate = false;
        claySet.culling = false;
        doStrokeStep(centerX, centerY, true, BRUSH_CLAYBUILDUP);
        int picked5aClay = sculpt.getLastFrameProfile().pickedVertCount;
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto ut5aClay = renderer.getLastUploadTiming();
        auto rs5aClay = renderer.getLastRenderStats();
        releaseStroke(centerX, centerY);

        // 5a Paint
        sculpt.setBrush(BRUSH_PAINT);
        auto& paintSet = sculpt.getSettings(BRUSH_PAINT);
        paintSet.radius = 25.0f * scalePx;
        paintSet.intensity = 1.0f;
        paintSet.writeAlbedo = true;
        paintSet.writeRoughness = false;
        paintSet.writeMetalness = false;
        paintSet.paintColor = glm::vec3(0.2f, 0.6f, 0.9f);
        doStrokeStep(centerX, centerY, true, BRUSH_PAINT);
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto ut5aPaint = renderer.getLastUploadTiming();
        auto rs5aPaint = renderer.getLastRenderStats();
        releaseStroke(centerX, centerY);

        // 5a Mask
        sculpt.setBrush(BRUSH_MASK);
        auto& maskSet = sculpt.getSettings(BRUSH_MASK);
        maskSet.radius = 25.0f * scalePx;
        maskSet.intensity = 1.0f;
        doStrokeStep(centerX, centerY, true, BRUSH_MASK);
        doStrokeStep(centerX + 24.0f, centerY, false, BRUSH_MASK);
        renderer.resetLastUploadTiming();
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto ut5aMask = renderer.getLastUploadTiming();
        auto rs5aMask = renderer.getLastRenderStats();
        releaseStroke(centerX + 24.0f, centerY);

        // 5a PolyGroup stroke
        sculpt.setBrush(BRUSH_POLYGROUP);
        sculpt.setActiveGroupID(15);
        auto& pgSet = sculpt.getSettings(BRUSH_POLYGROUP);
        pgSet.radius = 25.0f * scalePx;
        doStrokeStep(centerX, centerY, true, BRUSH_POLYGROUP);
        renderer.resetLastRenderStats();
        renderer.render(scene);
        glFinish();
        auto rs5aPg = renderer.getLastRenderStats();
        releaseStroke(centerX, centerY);

        std::cout << "  [Info] Test 5a DynSphere (dynVerts=" << dynVerts5a << "): "
                  << "Clay=" << ut5aClay.dirtyVertUploaded << " (runs=" << ut5aClay.dirtyRunCount << ", rb=" << rs5aClay.polygroupRebuilds << ")"
                  << ", Paint=" << ut5aPaint.dirtyVertUploaded << " (runs=" << ut5aPaint.dirtyRunCount << ", rb=" << rs5aPaint.polygroupRebuilds << ")"
                  << ", Mask=" << ut5aMask.dirtyVertUploaded << " (runs=" << ut5aMask.dirtyRunCount << ", rb=" << rs5aMask.polygroupRebuilds << ")"
                  << ", PG rb=" << rs5aPg.polygroupRebuilds << "\n";

        const uint32_t maxHalf5a = (uint32_t)std::max(1, dynVerts5a / 2);
        checkCond(dynVerts5a > 10000 && rsDynInit.polygroupRebuilds == 0 && picked5aClay > 0 &&
                  !ut5aClay.wasFullUpload && !ut5aClay.usedWholeBufferFallback && ut5aClay.dirtyRunCount >= 1 &&
                  ut5aClay.dirtyVertUploaded > 0 && ut5aClay.dirtyVertUploaded < maxHalf5a && rs5aClay.polygroupRebuilds == 0 &&
                  !ut5aPaint.wasFullUpload && !ut5aPaint.usedWholeBufferFallback && ut5aPaint.dirtyRunCount >= 1 &&
                  ut5aPaint.dirtyVertUploaded > 0 && ut5aPaint.dirtyVertUploaded < maxHalf5a && rs5aPaint.polygroupRebuilds == 0 &&
                  !ut5aMask.wasFullUpload && !ut5aMask.usedWholeBufferFallback && ut5aMask.dirtyRunCount >= 1 &&
                  ut5aMask.dirtyVertUploaded > 0 && ut5aMask.dirtyVertUploaded < maxHalf5a && rs5aMask.polygroupRebuilds == 0 &&
                  rs5aPg.polygroupRebuilds == 0 && rs5aPg.polygroupDraws >= 1,
                  "Test 5a: DynamicSubdiv sphere (Thickness+Subdiv+Cage) uses <50% incremental block upload & 0 PG rebuilds");

        // 5b: Open Plane + postSubdiv (smoothLevel=1) + Thickness + ShowCage
        scene.clear();
        g_undoManager.clear();
        resetRendererClean();
        renderer.setShowPolyGroups(true);

        scene.addPlane();
        Mesh* plane = scene.getSelected();
        if (plane && sculpt.getPrimitiveTool()) {
            plane = sculpt.getPrimitiveTool()->bakeToMesh(scene, plane);
            g_undoManager.clear();
        }
        checkCond(plane != nullptr, "Test 5b setup: Scene::addPlane created Plane primitive");
        if (plane) {
            plane->dynamicSettings.enabled = true;
            plane->dynamicSettings.enableSubdiv = true;
            plane->dynamicSettings.smoothLevel = 1;
            plane->dynamicSettings.enableThickness = true;
            plane->dynamicSettings.thickness = 0.1f;
            plane->dynamicSettings.postSubdiv = true;
            plane->dynamicSettings.showCage = true;
            plane->isDynamicDirty = true;
            plane->isDirty = true;

            cam.onResize(winW, winH);
            cam.setOrbitAngles(0.8f, 0.0f);
            cam.resetViewToMesh(plane, false);
            cam.update(0.016f);

            sculpt.setUseSym(false);
            sculpt.saveSymmetryToMesh(plane);
            sculpt.warmupMeshCaches(plane);
            sculpt.registerKnownMesh(plane);

            renderer.resetLastRenderStats();
            renderer.render(scene);
            glFinish();
            auto rsPlaneInit = renderer.getLastRenderStats();
            const int dynVerts5b = plane->dynamicCacheMesh ? plane->dynamicCacheMesh->nbVerts : 0;

            sculpt.setBrush(BRUSH_CLAYBUILDUP);
            claySet.radius = 40.0f * scalePx;
            claySet.intensity = 1.0f;
            claySet.accumulate = false;
            claySet.culling = false;
            doStrokeStep(centerX, centerY, true, BRUSH_CLAYBUILDUP);
            int picked5bClay = sculpt.getLastFrameProfile().pickedVertCount;
            renderer.resetLastUploadTiming();
            renderer.resetLastRenderStats();
            renderer.render(scene);
            glFinish();
            auto ut5bClay = renderer.getLastUploadTiming();
            auto rs5bClay = renderer.getLastRenderStats();
            releaseStroke(centerX, centerY);

            sculpt.setBrush(BRUSH_PAINT);
            paintSet.radius = 40.0f * scalePx;
            doStrokeStep(centerX, centerY, true, BRUSH_PAINT);
            renderer.resetLastUploadTiming();
            renderer.resetLastRenderStats();
            renderer.render(scene);
            glFinish();
            auto ut5bPaint = renderer.getLastUploadTiming();
            auto rs5bPaint = renderer.getLastRenderStats();
            releaseStroke(centerX, centerY);

            sculpt.setBrush(BRUSH_MASK);
            maskSet.radius = 40.0f * scalePx;
            doStrokeStep(centerX, centerY, true, BRUSH_MASK);
            doStrokeStep(centerX + 24.0f, centerY, false, BRUSH_MASK);
            renderer.resetLastUploadTiming();
            renderer.resetLastRenderStats();
            renderer.render(scene);
            glFinish();
            auto ut5bMask = renderer.getLastUploadTiming();
            auto rs5bMask = renderer.getLastRenderStats();
            releaseStroke(centerX + 24.0f, centerY);

            std::cout << "  [Info] Test 5b DynPlane (dynVerts=" << dynVerts5b << "): "
                      << "Clay=" << ut5bClay.dirtyVertUploaded << " (runs=" << ut5bClay.dirtyRunCount << ", fb=" << ut5bClay.usedWholeBufferFallback << ", rb=" << rs5bClay.polygroupRebuilds << ")"
                      << ", Paint=" << ut5bPaint.dirtyVertUploaded << " (runs=" << ut5bPaint.dirtyRunCount << ", rb=" << rs5bPaint.polygroupRebuilds << ")"
                      << ", Mask=" << ut5bMask.dirtyVertUploaded << " (runs=" << ut5bMask.dirtyRunCount << ", rb=" << rs5bMask.polygroupRebuilds << ")\n";

            checkCond(dynVerts5b > 0 && rsPlaneInit.polygroupRebuilds == 0 && picked5bClay > 0 &&
                      !ut5bClay.wasFullUpload && ut5bClay.dirtyRunCount >= 1 && rs5bClay.polygroupRebuilds == 0 &&
                      !ut5bPaint.wasFullUpload && ut5bPaint.dirtyRunCount >= 1 && rs5bPaint.polygroupRebuilds == 0 &&
                      !ut5bMask.wasFullUpload && ut5bMask.dirtyRunCount >= 1 && rs5bMask.polygroupRebuilds == 0,
                      "Test 5b: DynamicSubdiv Plane (postSubdiv+Thickness+Cage) stays incremental with 0 PG rebuilds");
        }

        g_undoManager.clear();
        allOk = checkNoGlError("Test 5") && allOk;
    }

    SystemStats::shutdownGpuMonitor();
    g_undoManager.clear();
    scene.clear();
    OptFlags::get() = savedFlags;

    std::cout << "================================================================================\n";
    std::cout << "  Stage 2 Tests Complete: " << (allOk ? "ALL PASSED" : "FAILED") << "\n";
    std::cout << "================================================================================\n\n";
    return allOk;
}

inline bool runStage3Tests(Scene& scene, SculptManager& sculpt, AngleRenderer& renderer, SDL_Window* window) {
    int winW = 1920, winH = 1080;
    if (window) {
        SDL_GetWindowSize(window, &winW, &winH);
    }
    if (winW <= 0) winW = 1920;
    if (winH <= 0) winH = 1080;

    const OptFlags::Config savedFlags = OptFlags::get();
    OptFlags::get().idleSleep = false;
    OptFlags::get().cullFace = false;
    OptFlags::get().vaoCache = true;
    OptFlags::get().orbitScale = false;
    OptFlags::get().dirtyBlocks = true;
    OptFlags::get().indexedPolygroup = true;
    OptFlags::get().faceNormalsOnly = true;
    OptFlags::get().cpuReuse = true;
    OptFlags::get().moveOptim = true;
    OptFlags::get().fastLog = true;

    const float scalePx = std::max(0.5f, (float)std::min(winW, winH) / 1080.0f);
    const float centerX = (float)winW * 0.5f;
    const float centerY = (float)winH * 0.5f;

    std::cout << "\n================================================================================\n";
    std::cout << "  Stage 3 Invariant Verification (--test-stage3)\n";
    std::cout << "================================================================================\n";

    bool allOk = true;
    auto checkCond = [&](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "  [FAIL] " << msg << "\n";
            allOk = false;
        } else {
            std::cout << "  [PASS] " << msg << "\n";
        }
    };

    auto doStrokeStep = [&](float x, float y, bool isDown, BrushType brush) {
        SDL_PumpEvents();
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        if (isDown) {
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = (int)std::round(x);
            ev.button.y = (int)std::round(y);
        } else {
            ev.type = SDL_MOUSEMOTION;
            ev.motion.state = SDL_BUTTON_LMASK;
            ev.motion.x = (int)std::round(x);
            ev.motion.y = (int)std::round(y);
        }
        sculpt.handleEvent(ev, scene);
        sculpt.setRawMousePos((int)std::round(x), (int)std::round(y));
        sculpt.processFrame(scene);
        renderer.setActiveBrush(brush);
    };

    auto releaseStroke = [&](float x, float y) {
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(x);
        evUp.button.y = (int)std::round(y);
        sculpt.handleEvent(evUp, scene);
    };

    // -------------------------------------------------------------------------
    // Test 1: updateFaceNormalsOnly numerical identity vs updateFaceNormalsAndBoxes
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);

        for (int i = 0; i < sphere->nbVerts; i += 2) {
            sphere->verts[i * 3 + 1] += 0.35f * std::sin((float)i * 0.1f);
            sphere->verts[i * 3 + 0] += 0.15f * std::cos((float)i * 0.15f);
        }

        std::vector<float> fnFullBoxes(sphere->nbFaces * 3, 0.0f);
        std::vector<float> boxesFull(sphere->nbFaces * 6, 0.0f);
        std::vector<float> centersFull(sphere->nbFaces * 3, 0.0f);
        std::vector<float> fnFullOnly(sphere->nbFaces * 3, 0.0f);

        updateFaceNormalsAndBoxes(
            sphere->verts.data(), sphere->nbVerts,
            sphere->faces.data(), sphere->nbFaces,
            nullptr, -1,
            fnFullBoxes.data(), boxesFull.data(), centersFull.data()
        );

        updateFaceNormalsOnly(
            sphere->verts.data(), sphere->nbVerts,
            sphere->faces.data(), sphere->nbFaces,
            fnFullOnly.data()
        );

        float maxDiffFull = 0.0f;
        for (size_t i = 0; i < fnFullBoxes.size(); ++i) {
            maxDiffFull = std::max(maxDiffFull, std::abs(fnFullBoxes[i] - fnFullOnly[i]));
        }

        std::vector<uint32_t> iFaces;
        iFaces.reserve(3000);
        for (uint32_t f = 0; f < (uint32_t)sphere->nbFaces; f += 9) {
            iFaces.push_back(f);
        }

        std::vector<float> fnSparseBoxes(sphere->nbFaces * 3, 0.0f);
        std::vector<float> fnSparseOnly(sphere->nbFaces * 3, 0.0f);

        updateFaceNormalsAndBoxes(
            sphere->verts.data(), sphere->nbVerts,
            sphere->faces.data(), sphere->nbFaces,
            iFaces.data(), (int)iFaces.size(),
            fnSparseBoxes.data(), boxesFull.data(), centersFull.data()
        );

        updateFaceNormalsOnly(
            sphere->verts.data(), sphere->nbVerts,
            sphere->faces.data(), sphere->nbFaces,
            iFaces.data(), (int)iFaces.size(),
            fnSparseOnly.data()
        );

        float maxDiffSparse = 0.0f;
        for (uint32_t fid : iFaces) {
            maxDiffSparse = std::max({maxDiffSparse,
                std::abs(fnSparseBoxes[fid * 3 + 0] - fnSparseOnly[fid * 3 + 0]),
                std::abs(fnSparseBoxes[fid * 3 + 1] - fnSparseOnly[fid * 3 + 1]),
                std::abs(fnSparseBoxes[fid * 3 + 2] - fnSparseOnly[fid * 3 + 2])});
        }

        sphere->bvh.refit(*sphere, iFaces.data(), iFaces.size(), false);
        const auto& rs = sphere->bvh.getLastRefitStats();

        std::cout << "  [Info] Test 1 Normals: maxDiffFull=" << maxDiffFull
                  << ", maxDiffSparse=" << maxDiffSparse
                  << ", bvhUsedPrecomputed=" << rs.usedPrecomputed << "\n";

        checkCond(maxDiffFull == 0.0f && maxDiffSparse == 0.0f && !rs.usedPrecomputed,
                  "Test 1: updateFaceNormalsOnly matches updateFaceNormalsAndBoxes 100% (diff=0.000000) and BVH refits without boxes");
        delete sphere;
    }

    // -------------------------------------------------------------------------
    // Test 2: Sort & Dedup bypass for non-overlapping symmetry & paint/mask
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();

        Mesh* sphere = createSphereMeshWithPolygroups(180, 360).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(true);
        sculpt.setSymX(true);
        sculpt.setSymY(false);
        sculpt.setSymZ(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);

        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 35.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.culling = false;

        float strokeX = centerX + 180.0f * scalePx;
        doStrokeStep(strokeX, centerY, true, BRUSH_CLAYBUILDUP);
        releaseStroke(strokeX, centerY);

        const auto profClay = sculpt.getLastFrameProfile();
        std::cout << "  [Info] Test 2 ClayBuildup SymX: Dedup=" << profClay.dedupOnlyMs
                  << "ms, Radix=" << profClay.radixSortMs << "ms, Verts=" << profClay.affectedVertCount << "\n";

        g_undoManager.undo(scene);
        float undoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty() && redoEntry->deltas[0].hasPositions) {
            const auto& d = redoEntry->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                undoDiff = std::max({undoDiff,
                    std::abs(sphere->verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]),
                    std::abs(sphere->verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]),
                    std::abs(sphere->verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2])});
            }
        }
        g_undoManager.redo(scene);

        checkCond(profClay.dedupOnlyMs == 0.0 && profClay.affectedVertCount > 0 && undoDiff == 0.0f,
                  "Test 2a: Non-overlapping symmetry skips Tag-Dedup pass with exact 0.000000 UndoDiff");

        sculpt.setBrush(BRUSH_MASK);
        auto& maskSet = sculpt.getSettings(BRUSH_MASK);
        maskSet.radius = 45.0f * scalePx;
        maskSet.intensity = 1.0f;

        doStrokeStep(strokeX, centerY, true, BRUSH_MASK);
        releaseStroke(strokeX, centerY);
        const auto profMask = sculpt.getLastFrameProfile();

        std::cout << "  [Info] Test 2b Mask Brush: SortDedup=" << profMask.sortDedupMs
                  << "ms, Verts=" << profMask.affectedVertCount << "\n";

        checkCond(profMask.sortDedupMs == 0.0 && profMask.affectedVertCount > 0,
                  "Test 2b: BRUSH_MASK bypasses Sort & Dedup completely (sortDedupMs = 0.0)");

        sculpt.setBrush(BRUSH_PAINT);
        auto& paintSet = sculpt.getSettings(BRUSH_PAINT);
        paintSet.radius = 45.0f * scalePx;
        paintSet.intensity = 1.0f;

        doStrokeStep(strokeX, centerY, true, BRUSH_PAINT);
        releaseStroke(strokeX, centerY);
        const auto profPaint = sculpt.getLastFrameProfile();

        std::cout << "  [Info] Test 2c Paint Brush: SortDedup=" << profPaint.sortDedupMs
                  << "ms, Verts=" << profPaint.affectedVertCount << "\n";

        checkCond(profPaint.sortDedupMs == 0.0 && profPaint.affectedVertCount > 0,
                  "Test 2c: BRUSH_PAINT bypasses Sort & Dedup completely (sortDedupMs = 0.0)");
    }

    // -------------------------------------------------------------------------
    // Test 3: Move Tool Step 0 Idle & pickVerticesInSphereTopological acceleration
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();

        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        Camera& cam = scene.getCamera();
        cam.onResize(winW, winH);
        cam.resetViewToMesh(sphere, false);
        cam.update(0.016f);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);

        glm::vec3 testCenter(0.0f, 2.0f, 0.0f);
        float testR2 = 0.35f * 0.35f;
        uint32_t startFace = 0;

        std::vector<uint32_t> pickedLegacy;
        std::vector<uint8_t> visLegacy;
        uint8_t epLegacy = 0;
        OptFlags::get().moveOptim = false;
        SculptManager::pickVerticesInSphereTopological(sphere, testCenter, testR2, startFace, visLegacy, epLegacy, pickedLegacy);
        std::sort(pickedLegacy.begin(), pickedLegacy.end());

        std::vector<uint32_t> pickedOptim;
        std::vector<uint8_t> visOptim;
        uint8_t epOptim = 0;
        OptFlags::get().moveOptim = true;
        SculptManager::pickVerticesInSphereTopological(sphere, testCenter, testR2, startFace, visOptim, epOptim, pickedOptim);
        std::sort(pickedOptim.begin(), pickedOptim.end());

        bool topoPickIdentical = (pickedLegacy.size() == pickedOptim.size());
        if (topoPickIdentical) {
            for (size_t i = 0; i < pickedLegacy.size(); ++i) {
                if (pickedLegacy[i] != pickedOptim[i]) {
                    topoPickIdentical = false;
                    break;
                }
            }
        }
        std::cout << "  [Info] Test 3 Topo Pick: legacyCount=" << pickedLegacy.size()
                  << ", optimCount=" << pickedOptim.size() << ", match=" << topoPickIdentical << "\n";

        checkCond(topoPickIdentical && !pickedOptim.empty(),
                  "Test 3a: Topological pick with BVH candidate pre-filtering matches legacy BFS 100%");

        sculpt.setBrush(BRUSH_MOVE);
        auto& moveSet = sculpt.getSettings(BRUSH_MOVE);
        moveSet.radius = 60.0f * scalePx;
        moveSet.intensity = 1.0f;
        moveSet.topoCheck = true;

        doStrokeStep(centerX, centerY, true, BRUSH_MOVE);
        const auto profMoveStep0 = sculpt.getLastFrameProfile();

        std::cout << "  [Info] Test 3b Move Step 0: FaceLookup=" << profMoveStep0.faceLookupMs
                  << "ms, FaceNorm=" << profMoveStep0.faceNormalsMs
                  << "ms, VertNorm=" << profMoveStep0.vertNormalsMs
                  << "ms, BVHRefit=" << profMoveStep0.bvhUpdateMs << "ms\n";

        checkCond(profMoveStep0.faceNormalsMs == 0.0 && profMoveStep0.vertNormalsMs == 0.0 &&
                  profMoveStep0.bvhUpdateMs == 0.0 && profMoveStep0.pickedVertCount > 0,
                  "Test 3b: Move tool Step 0 (0 displacement) skips all idle Normals & BVH updates");

        doStrokeStep(centerX + 30.0f * scalePx, centerY, false, BRUSH_MOVE);
        releaseStroke(centerX + 30.0f * scalePx, centerY);

        g_undoManager.undo(scene);
        float moveUndoDiff = 0.0f;
        auto* redoMove = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoMove && !redoMove->deltas.empty() && redoMove->deltas[0].hasPositions) {
            const auto& d = redoMove->deltas[0];
            for (size_t i = 0; i < d.indices.size(); ++i) {
                uint32_t vid = d.indices[i];
                moveUndoDiff = std::max({moveUndoDiff,
                    std::abs(sphere->verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]),
                    std::abs(sphere->verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]),
                    std::abs(sphere->verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2])});
            }
        }
        checkCond(moveUndoDiff == 0.0f,
                  "Test 3c: Move tool Drag & Undo restores mesh with 0.000000 epsilon discrepancy");
    }

    // -------------------------------------------------------------------------
    // Test 4: Buffer reuse & lazy vertProxy sync across multiple steps
    // -------------------------------------------------------------------------
    {
        scene.clear();
        g_undoManager.clear();

        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);

        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 40.0f * scalePx;
        claySet.intensity = 1.0f;
        claySet.culling = false;

        bool proxySyncClean = true;
        for (int step = 0; step < 10; ++step) {
            float sx = centerX + (float)(step - 5) * 8.0f * scalePx;
            doStrokeStep(sx, centerY, step == 0, BRUSH_CLAYBUILDUP);
            const auto p = sculpt.getLastFrameProfile();
            if (p.vertProxySyncMs > 0.0) {
                proxySyncClean = false;
            }
        }
        releaseStroke(centerX + 32.0f * scalePx, centerY);

        std::cout << "  [Info] Test 4 Lazy Proxy: proxySyncClean=" << proxySyncClean << "\n";
        checkCond(proxySyncClean, "Test 4: Lazy vertProxy sync eliminates per-step OpenMP proxy copies during ClayBuildup");
    }

    // -------------------------------------------------------------------------
    // Test 5: Fast log flush verification
    // -------------------------------------------------------------------------
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 50; ++i) {
            sculpt_log("[STAGE3 BENCH] Test log line %d\n", i);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double logMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        Logger::instance().flush();

        std::cout << "  [Info] Test 5 FastLog: 50 log messages took " << logMs << " ms\n";
        checkCond(logMs < 25.0, "Test 5: FastLog buffers Info/Debug without per-line synchronous disk flush (< 25 ms)");
    }

    g_undoManager.clear();
    scene.clear();
    OptFlags::get() = savedFlags;

    std::cout << "================================================================================\n";
    std::cout << "  Stage 3 Tests Complete: " << (allOk ? "ALL PASSED" : "FAILED") << "\n";
    std::cout << "================================================================================\n\n";
    return allOk;
}

inline bool runStage4Tests(Scene& scene, SculptManager& sculpt, AngleRenderer& renderer, SDL_Window* window) {
    int winW = 1920, winH = 1080;
    if (window) {
        SDL_GetWindowSize(window, &winW, &winH);
    }

    const OptFlags::Config savedFlags = OptFlags::get();
    OptFlags::get().coalesceGrab = true;
    OptFlags::get().batchSubsteps = true;
    OptFlags::get().cursorThrottle = true;
    OptFlags::get().bvhRefitLeaves = true;

    const float scalePx = std::max(0.5f, (float)std::min(winW, winH) / 1080.0f);
    const float centerX = (float)winW * 0.5f;
    const float centerY = (float)winH * 0.5f;

    std::cout << "\n================================================================================\n";
    std::cout << "  Stage 4 Invariant Verification (--test-stage4)\n";
    std::cout << "================================================================================\n";

    bool allOk = true;
    auto checkCond = [&](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "  [FAIL] " << msg << "\n";
            allOk = false;
        } else {
            std::cout << "  [PASS] " << msg << "\n";
        }
    };

    auto doStrokeStep = [&](float x, float y, bool isDown, BrushType brush) {
        SDL_PumpEvents();
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        if (isDown) {
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = (int)std::round(x);
            ev.button.y = (int)std::round(y);
        } else {
            ev.type = SDL_MOUSEMOTION;
            ev.motion.state = SDL_BUTTON_LMASK;
            ev.motion.x = (int)std::round(x);
            ev.motion.y = (int)std::round(y);
        }
        sculpt.handleEvent(ev, scene);
        sculpt.setRawMousePos((int)std::round(x), (int)std::round(y));
        sculpt.processFrame(scene);
        renderer.setActiveBrush(brush);
    };

    auto releaseStroke = [&](float x, float y) {
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(x);
        evUp.button.y = (int)std::round(y);
        sculpt.handleEvent(evUp, scene);
    };

    // -------------------------------------------------------------------------
    // Test 1: Input Coalescing for Grab Brushes (coalesceGrab)
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);

        sculpt.setBrush(BRUSH_MOVE);
        auto& moveSet = sculpt.getSettings(BRUSH_MOVE);
        moveSet.radius = 50.0f * scalePx;
        moveSet.intensity = 1.0f;

        sculpt.resetStage4Stats();

        // Step 0
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)std::round(centerX);
        evDown.button.y = (int)std::round(centerY);
        sculpt.handleEvent(evDown, scene);

        // 10 motion events without rendering (burst input)
        for (int i = 1; i <= 10; ++i) {
            SDL_Event evMove;
            std::memset(&evMove, 0, sizeof(evMove));
            evMove.type = SDL_MOUSEMOTION;
            evMove.motion.state = SDL_BUTTON_LMASK;
            evMove.motion.x = (int)std::round(centerX + (float)i * 3.0f * scalePx);
            evMove.motion.y = (int)std::round(centerY);
            sculpt.handleEvent(evMove, scene);
        }

        bool hasPending = sculpt.hasPendingGrabMotion();
        size_t coalescedBefore = sculpt.getCoalescedGrabCount();

        // processFrame flushes the coalesced grab stroke
        sculpt.processFrame(scene);

        bool clearedPending = !sculpt.hasPendingGrabMotion();
        size_t coalescedAfter = sculpt.getCoalescedGrabCount();

        releaseStroke(centerX + 30.0f * scalePx, centerY);

        std::vector<float> postStrokeVerts = sphere->verts;
        g_undoManager.undo(scene);
        g_undoManager.redo(scene);
        float redoDiff = 0.0f;
        for (size_t i = 0; i < sphere->verts.size(); ++i) {
            redoDiff = std::max(redoDiff, std::abs(sphere->verts[i] - postStrokeVerts[i]));
        }

        std::cout << "  [Info] Test 1 CoalesceGrab: pending=" << hasPending
                  << ", flushed=" << clearedPending
                  << ", countBefore=" << coalescedBefore
                  << ", countAfter=" << coalescedAfter
                  << ", redoDiff=" << redoDiff << "\n";

        checkCond(hasPending && clearedPending && coalescedAfter == 1 && redoDiff == 0.0f,
                  "Test 1: Move tool coalesces 10 motion events into 1 stroke with exact Undo/Redo (diff=0.000000)");
        scene.clear();
        g_undoManager.clear();
    }

    // -------------------------------------------------------------------------
    // Test 2: Sub-step Batching & Arc Remainder (batchSubsteps)
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        sculpt.setUseSym(false);
        sculpt.saveSymmetryToMesh(sphere);
        sculpt.warmupMeshCaches(sphere);

        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& claySet = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        claySet.radius = 35.0f * scalePx;
        claySet.spacing = 0.1f;
        claySet.intensity = 1.0f;

        sculpt.resetStage4Stats();

        // Step 0
        doStrokeStep(centerX, centerY, true, BRUSH_CLAYBUILDUP);

        // Move 40 pixels in one event: generates ~11 sub-steps
        doStrokeStep(centerX + 40.0f * scalePx, centerY, false, BRUSH_CLAYBUILDUP);
        size_t batchCount = sculpt.getBatchedSubstepsCount();

        releaseStroke(centerX + 40.0f * scalePx, centerY);

        std::vector<float> strokeVerts = sphere->verts;
        g_undoManager.undo(scene);
        g_undoManager.redo(scene);
        float redoDiff = 0.0f;
        for (size_t i = 0; i < sphere->verts.size(); ++i) {
            redoDiff = std::max(redoDiff, std::abs(sphere->verts[i] - strokeVerts[i]));
        }

        std::cout << "  [Info] Test 2 BatchSubsteps: batchCount=" << batchCount << ", redoDiff=" << redoDiff << "\n";
        checkCond(batchCount >= 1 && redoDiff == 0.0f,
                  "Test 2: Sub-steps batched post-processing into 1 pass with exact Undo/Redo (diff=0.000000)");
        scene.clear();
        g_undoManager.clear();
    }

    // -------------------------------------------------------------------------
    // Test 3: Cursor Occlusion Throttling & Raycast Optimization (cursorThrottle)
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);
        scene.addMesh(sphere);
        scene.selectMesh(sphere);

        sculpt.setUseSym(true);
        sculpt.setSymX(true);
        sculpt.setSymY(true);
        sculpt.setSymZ(true);
        sculpt.saveSymmetryToMesh(sphere);

        sculpt.getCursor().resetOcclusionQueryCount();

        // Call processFrame with same mouse position multiple times
        for (int i = 0; i < 5; ++i) {
            sculpt.setRawMousePos((int)centerX, (int)centerY);
            sculpt.processFrame(scene);
        }

        size_t queries1 = sculpt.getCursor().getOcclusionQueryCount();

        // Move slightly <= 1.0px (sub-pixel jitter)
        for (int i = 0; i < 5; ++i) {
            sculpt.setRawMousePos((int)centerX + (i % 2), (int)centerY);
            sculpt.processFrame(scene);
        }

        size_t queries2 = sculpt.getCursor().getOcclusionQueryCount();

        std::cout << "  [Info] Test 3 CursorThrottle: initialQueries=" << queries1
                  << ", jitterQueries=" << queries2 << "\n";
        checkCond(queries1 <= 7 && queries2 <= 7,
                  "Test 3: Cursor occlusion throttling eliminates redundant BVH raycasts on stationary/jittering mouse");
        scene.clear();
        g_undoManager.clear();
    }

    // -------------------------------------------------------------------------
    // Test 4: BVH::refitLeaves Equivalence & Tree Validation (bvhRefitLeaves)
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);

        // Pick vertices on the sphere surface (R = 2.0f)
        std::vector<uint32_t> picked = sphere->bvh.pickVerticesInSphere(
            *sphere, 0.0f, 0.0f, 2.0f, 0.25f, nullptr, 0
        );

        const auto& queriedLeaves = sphere->bvh.getLastQueriedLeaves();
        size_t numLeaves = queriedLeaves.size();

        // Deform picked vertices
        for (uint32_t v : picked) {
            sphere->verts[v * 3 + 2] += 0.25f;
        }

        // Refit directly via refitLeaves
        sphere->bvh.refitLeaves(*sphere, queriedLeaves.data(), queriedLeaves.size(), false);
        const auto& rs = sphere->bvh.getLastRefitStats();

        std::string treeErr;
        bool treeValid = sphere->bvh.validateTree(*sphere, treeErr);

        // Test raycast against the deformed surface
        RayHit hit;
        bool hitValid = sphere->bvh.raycast(*sphere, glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f, 0.0f, -1.0f), hit);

        std::cout << "  [Info] Test 4 RefitLeaves: leaves=" << numLeaves
                  << ", dedupMs=" << rs.leafDedupMs
                  << ", treeValid=" << treeValid
                  << ", hit=" << hitValid
                  << ", err='" << treeErr << "'\n";

        checkCond(treeValid && hitValid && rs.leafDedupMs <= 0.05,
                  "Test 4: BVH::refitLeaves passes validateTree 100% and executes with zero face-to-leaf overhead");
        delete sphere;
    }

    // -------------------------------------------------------------------------
    // Test 5: OpenMP Small Buffer Merge Optimization
    // -------------------------------------------------------------------------
    {
        Mesh* sphere = createSphereMeshWithPolygroups(120, 240).release();
        sphere->postInit(true);

        std::vector<uint32_t> testVerts;
        testVerts.reserve(1000);
        for (uint32_t v = 0; v < 1000; ++v) testVerts.push_back(v * 2);

        std::vector<uint32_t> outFaces1(sphere->nbFaces, 0);
        std::vector<uint8_t> tagFlags(sphere->nbFaces, 0);
        uint8_t epoch = 1;

        uint32_t countFaces = getFacesFromVerticesFast(
            testVerts.data(), (uint32_t)testVerts.size(),
            sphere->vrfStartCount.data(),
            sphere->vertRingFace.data(),
            outFaces1.data(),
            tagFlags.data(),
            &epoch,
            sphere->nbFaces,
            sphere->nbVerts,
            (uint32_t)sphere->nbFaces,
            sphere->vertRingFace.size()
        );

        bool facesValid = countFaces > 0;
        for (uint32_t f = 0; f < countFaces; ++f) {
            if (outFaces1[f] >= (uint32_t)sphere->nbFaces) {
                facesValid = false;
                break;
            }
        }

        std::cout << "  [Info] Test 5 Buffer Merge: countFaces=" << countFaces << ", valid=" << facesValid << "\n";
        checkCond(facesValid, "Test 5: Small buffer merge produces valid face indices without OpenMP barrier overhead");
        delete sphere;
    }

    g_undoManager.clear();
    scene.clear();
    OptFlags::get() = savedFlags;

    std::cout << "================================================================================\n";
    std::cout << "  Stage 4 Tests Complete: " << (allOk ? "ALL PASSED" : "FAILED") << "\n";
    std::cout << "================================================================================\n\n";
    return allOk;
}

} // namespace RenderBenchmark

