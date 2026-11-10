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
#include "common/OptFlags.h"
#include "common/SystemStats.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "mesh/Mesh.h"
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

        std::vector<double> vSculptCpu, vProcMs, vVboMs, vVboMB, vSpanPct, vRenderCpu, vGpuFin, vTotal;
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
                  << " ms | VboUpload=" << vVboMB[0] << " MB (" << vVboMs[0] << " ms, span=" << vSpanPct[0] << "%)"
                  << " | FrameTotal=" << vTotal[0] << " ms\n";
        std::cout << "      All12Avg:    SculptCPU=" << meanOf(vSculptCpu)
                  << " ms | ProcessFrame=" << meanOf(vProcMs)
                  << " ms | VboUpload=" << meanOf(vVboMB) << " MB (" << meanOf(vVboMs) << " ms, span=" << meanOf(vSpanPct) << "%)\n";
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

        std::vector<double> vSculptCpu, vPgMs, vPgMB, vRenderCpu, vGpuFin, vTotal;
        uint32_t totalRebuilds = 0;

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
            totalRebuilds += rs.polygroupRebuilds;

            vSculptCpu.push_back(sCpu);
            vPgMs.push_back(ut.msPolygroupRebuild);
            vPgMB.push_back(pgMB);
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
                  << " | RenderCPU=" << meanOf(vRenderCpu)
                  << " ms | Finish=" << meanOf(vGpuFin)
                  << " ms | TotalFrame=" << meanOf(vTotal) << " ms"
                  << " | Rebuilds=" << totalRebuilds << "\n\n";
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

} // namespace RenderBenchmark

