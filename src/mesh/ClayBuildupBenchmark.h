#pragma once

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
#include <cstring>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL2/SDL.h>

#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "sculpt/SculptEngine.h"

namespace ClayBuildupBenchmark {

inline std::unique_ptr<Mesh> createBenchmarkSphere(float R = 35.0f, int rings = 32, int sectors = 64) {
    auto mesh = std::make_unique<Mesh>();
    mesh->verts.reserve(((rings - 1) * sectors + 2) * 3);
    mesh->faces.reserve(((rings - 2) * sectors + 2 * sectors) * 4);

    // South pole (index 0)
    mesh->verts.push_back(0.0f);
    mesh->verts.push_back(-R);
    mesh->verts.push_back(0.0f);

    for (int r = 1; r < rings; ++r) {
        float phi = -(float)M_PI * 0.5f + (float)M_PI * (float)r / (float)rings;
        float y = R * std::sin(phi);
        float rCirc = R * std::cos(phi);
        for (int s = 0; s < sectors; ++s) {
            float theta = 2.0f * (float)M_PI * (float)s / (float)sectors;
            mesh->verts.push_back(rCirc * std::cos(theta));
            mesh->verts.push_back(y);
            mesh->verts.push_back(rCirc * std::sin(theta));
        }
    }

    uint32_t northPoleIdx = 1 + (rings - 1) * sectors;
    mesh->verts.push_back(0.0f);
    mesh->verts.push_back(R);
    mesh->verts.push_back(0.0f);

    mesh->nbVerts = (int)(mesh->verts.size() / 3);

    // Bottom cap
    for (int s = 0; s < sectors; ++s) {
        mesh->faces.push_back(0);
        mesh->faces.push_back(1 + (s + 1) % sectors);
        mesh->faces.push_back(1 + s);
        mesh->faces.push_back(0xFFFFFFFF);
        mesh->nbFaces++;
    }

    // Intermediate rings
    for (int r = 1; r < rings - 1; ++r) {
        uint32_t base0 = 1 + (r - 1) * sectors;
        uint32_t base1 = 1 + r * sectors;
        for (int s = 0; s < sectors; ++s) {
            uint32_t sNext = (s + 1) % sectors;
            mesh->faces.push_back(base0 + s);
            mesh->faces.push_back(base0 + sNext);
            mesh->faces.push_back(base1 + sNext);
            mesh->faces.push_back(base1 + s);
            mesh->nbFaces++;
        }
    }

    // Top cap
    uint32_t topRingBase = 1 + (rings - 2) * sectors;
    for (int s = 0; s < sectors; ++s) {
        mesh->faces.push_back(northPoleIdx);
        mesh->faces.push_back(topRingBase + s);
        mesh->faces.push_back(topRingBase + (s + 1) % sectors);
        mesh->faces.push_back(0xFFFFFFFF);
        mesh->nbFaces++;
    }

    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    return mesh;
}

inline bool run1000Strokes(bool withDyntopo = false, int numStrokes = 1000) {
    std::cout << "\n================================================================================\n";
    std::cout << "  SPSculpt Clay Buildup " << numStrokes << "-Stroke Stress Benchmark\n";
    std::cout << "  Mode: " << (withDyntopo ? "WITH Dynamic Topology (Global Dyntopo ON)" : "Static Mesh (Dyntopo OFF)") << "\n";
    std::cout << "================================================================================\n\n";

    auto meshPtr = createBenchmarkSphere(35.0f, 32, 64);
    Mesh* meshRaw = meshPtr.release();
    meshRaw->initFaceGroups();
    meshRaw->postInit(true);
    meshRaw->bvh.resetDiagnosticCounters();

    Scene scene;
    scene.addMesh(meshRaw);
    scene.selectMesh(meshRaw);

    Camera& camera = scene.getCamera();
    camera.onResize(1920, 1080);
    camera.resetViewToMesh(meshRaw, false);
    camera.update(0.016f);

    SculptManager sculpt;
    sculpt.setRenderer(nullptr);
    sculpt.setBrush(BRUSH_CLAYBUILDUP);

    auto& settings = sculpt.getSettings(BRUSH_CLAYBUILDUP);
    settings.radius = 120.0f;
    settings.intensity = 1.0f;
    settings.focalShift = -0.285f;
    settings.spacing = 0.05f;
    settings.clay = true;
    settings.accumulate = false;
    settings.culling = false;

    if (withDyntopo) {
        sculpt.setDyntopoEnabled(true);
        sculpt.getDyntopoSettings().enabled = true;
        sculpt.getDyntopoSettings().detail = 0.5f;
        sculpt.getDyntopoSettings().constantDetail = false;
        sculpt.getCurrentSettings().topologyEdgeSize = 0.6f;
    } else {
        sculpt.setDyntopoEnabled(false);
    }

    constexpr int DRAG_STEPS_PER_STROKE = 4;
    double totalBenchMs = 0.0;
    double maxStrokeMs = 0.0;
    int maxStrokeIdx = 0;
    size_t nanInfCount = 0;
    size_t oobCount = 0;

    std::cout << "Initial Mesh: " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces
              << " faces (QuadSphere R=35.0)\n";
    std::cout << "Executing " << numStrokes << " strokes of ClayBuildup...\n\n";

    auto tBenchStart = std::chrono::high_resolution_clock::now();

    for (int s = 0; s < numStrokes; ++s) {
        auto tStrokeStart = std::chrono::high_resolution_clock::now();

        // Stroke trajectory: moving angle across front hemisphere
        float angle = float(s) * 0.23f;
        float radial = 40.0f + 120.0f * std::sin(float(s) * 0.13f);
        float startX = 960.0f + radial * std::cos(angle);
        float startY = 540.0f + radial * std::sin(angle);
        float dirAngle = angle + 1.2f;
        float stepDist = 20.0f;
        float dx = stepDist * std::cos(dirAngle);
        float dy = stepDist * std::sin(dirAngle);

        // 1. Mouse Down
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)std::round(startX);
        evDown.button.y = (int)std::round(startY);
        sculpt.handleEvent(evDown, scene);

        // 2. Drag steps
        for (int step = 1; step <= DRAG_STEPS_PER_STROKE; ++step) {
            float curX = startX + float(step) * dx;
            float curY = startY + float(step) * dy;

            SDL_Event evMove;
            std::memset(&evMove, 0, sizeof(evMove));
            evMove.type = SDL_MOUSEMOTION;
            evMove.motion.state = SDL_BUTTON_LMASK;
            evMove.motion.x = (int)std::round(curX);
            evMove.motion.y = (int)std::round(curY);
            sculpt.handleEvent(evMove, scene);
        }

        // 3. Mouse Up
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(startX + float(DRAG_STEPS_PER_STROKE) * dx);
        evUp.button.y = (int)std::round(startY + float(DRAG_STEPS_PER_STROKE) * dy);
        sculpt.handleEvent(evUp, scene);

        auto tStrokeEnd = std::chrono::high_resolution_clock::now();
        double strokeMs = std::chrono::duration<double, std::milli>(tStrokeEnd - tStrokeStart).count();
        if (strokeMs > maxStrokeMs) {
            maxStrokeMs = strokeMs;
            maxStrokeIdx = s + 1;
        }

        // Health check on vertices: check for NaN / Inf
        for (size_t i = 0; i < meshRaw->verts.size(); ++i) {
            if (std::isnan(meshRaw->verts[i]) || std::isinf(meshRaw->verts[i])) {
                nanInfCount++;
                break;
            }
        }

        // Diagnostic check: verify vrfStartCount capacity vs nbVerts
        if (meshRaw->vrfStartCount.size() < (size_t)meshRaw->nbVerts * 2) {
            oobCount++;
        }

        // Checkpoint logging
        if ((s + 1) % 50 == 0 || (s + 1) == numStrokes || (s < 10)) {
            size_t undoMem = g_undoManager.getTotalMemoryUsage();
            std::cout << "  [Stroke " << std::setw(4) << (s + 1) << "/" << numStrokes << "]"
                      << " V=" << std::setw(7) << meshRaw->nbVerts
                      << " F=" << std::setw(7) << meshRaw->nbFaces
                      << " | StrokeTime=" << std::fixed << std::setprecision(1) << std::setw(6) << strokeMs << "ms"
                      << " | UndoMem=" << std::setw(5) << (undoMem / (1024 * 1024)) << "MB"
                      << " | BVHNd=" << std::setw(6) << meshRaw->bvh.nodeCount()
                      << " | NanInf=" << nanInfCount
                      << " | VRF_OOB=" << oobCount << "\n";
            std::cout.flush();
        }
    }

    auto tBenchEnd = std::chrono::high_resolution_clock::now();
    totalBenchMs = std::chrono::duration<double, std::milli>(tBenchEnd - tBenchStart).count();

    std::cout << "\n================================================================================\n";
    std::cout << "              CLAY BUILDUP " << numStrokes << "-STROKE BENCHMARK SUMMARY\n";
    std::cout << "================================================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Mode:                      " << (withDyntopo ? "WITH Dynamic Topology" : "Static Mesh") << "\n";
    std::cout << "Total Wall Time:           " << totalBenchMs << " ms (" << (totalBenchMs / 1000.0) << " s)\n";
    std::cout << "Average Stroke Time:       " << (totalBenchMs / numStrokes) << " ms\n";
    std::cout << "Max Stroke Time:           " << maxStrokeMs << " ms at stroke " << maxStrokeIdx << "\n";
    std::cout << "Final Mesh Complexity:     " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces << " faces\n";
    std::cout << "Final Undo Stack Memory:   " << (g_undoManager.getTotalMemoryUsage() / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "NaN/Inf Vertices Detected: " << nanInfCount << "\n";
    std::cout << "Stale VRF OOB Occurrences: " << oobCount << "\n";
    std::cout << "================================================================================\n\n";

    return (nanInfCount == 0 && oobCount == 0);
}

} // namespace ClayBuildupBenchmark

