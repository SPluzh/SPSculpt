#include "editing/PolyGroupTool.h"
#include <set>
#include <vector>
#include <queue>
#include <algorithm>
#include <iostream>

void PolyGroupTool::createGroupFromMask(Mesh* mesh, float maskThreshold) {
    if (!mesh || mesh->nbFaces <= 0) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;

    if (mesh->faceGroups.size() != (size_t)mesh->nbFaces) {
        mesh->initFaceGroups();
    }

    uint32_t nextGid = mesh->getNextFreeGroupID();
    int modifiedFaces = 0;

    for (int f = 0; f < mesh->nbFaces; ++f) {
        uint32_t iv1 = mesh->faces[f * 4];
        uint32_t iv2 = mesh->faces[f * 4 + 1];
        uint32_t iv3 = mesh->faces[f * 4 + 2];
        uint32_t iv4 = mesh->faces[f * 4 + 3];

        bool isMasked1 = (iv1 < (uint32_t)mesh->nbVerts) && (mesh->materials[iv1 * 3 + 2] < maskThreshold);
        bool isMasked2 = (iv2 < (uint32_t)mesh->nbVerts) && (mesh->materials[iv2 * 3 + 2] < maskThreshold);
        bool isMasked3 = (iv3 < (uint32_t)mesh->nbVerts) && (mesh->materials[iv3 * 3 + 2] < maskThreshold);
        bool isMasked4 = (iv4 != 0xffffffff && iv4 < (uint32_t)mesh->nbVerts) ? (mesh->materials[iv4 * 3 + 2] < maskThreshold) : true;

        if (isMasked1 && isMasked2 && isMasked3 && isMasked4) {
            mesh->faceGroups[f] = nextGid;
            modifiedFaces++;
        }
    }

    if (modifiedFaces > 0) {
        mesh->isFaceGroupDirty = true;
        std::cout << "[PolyGroupTool] Created group " << nextGid << " from mask (" << modifiedFaces << " faces assigned)." << std::endl;
    }
}

void PolyGroupTool::autoGroupByConnectedComponents(Mesh* mesh) {
    if (!mesh || mesh->nbFaces <= 0) return;

    if (mesh->faceGroups.size() != (size_t)mesh->nbFaces) {
        mesh->initFaceGroups();
    }

    if (mesh->vrfStartCount.empty() || mesh->vertRingFace.empty()) {
        std::cout << "[PolyGroupTool] Topology missing! Rebuilding..." << std::endl;
        mesh->postInit();
    }

    std::vector<bool> visited(mesh->nbFaces, false);
    uint32_t currentGroupId = 1;

    for (int i = 0; i < mesh->nbFaces; ++i) {
        if (visited[i]) continue;

        std::vector<uint32_t> queue;
        queue.push_back(i);
        visited[i] = true;

        size_t head = 0;
        while (head < queue.size()) {
            uint32_t f = queue[head++];
            mesh->faceGroups[f] = currentGroupId;

            uint32_t faceVerts[4] = {
                mesh->faces[f * 4],
                mesh->faces[f * 4 + 1],
                mesh->faces[f * 4 + 2],
                mesh->faces[f * 4 + 3]
            };

            for (int k = 0; k < 4; ++k) {
                uint32_t v = faceVerts[k];
                if (v == 0xffffffff || v >= (uint32_t)mesh->nbVerts) continue;
                if (v * 2 + 1 >= mesh->vrfStartCount.size()) continue;

                uint32_t start = mesh->vrfStartCount[v * 2];
                uint32_t count = mesh->vrfStartCount[v * 2 + 1];
                if (start + count > mesh->vertRingFace.size()) continue;

                for (uint32_t j = start; j < start + count; ++j) {
                    uint32_t nf = mesh->vertRingFace[j];
                    if (nf < (uint32_t)mesh->nbFaces && !visited[nf]) {
                        visited[nf] = true;
                        queue.push_back(nf);
                    }
                }
            }
        }

        currentGroupId++;
    }

    mesh->isFaceGroupDirty = true;
    std::cout << "[PolyGroupTool] Auto-grouped " << (currentGroupId - 1) << " connected topological components." << std::endl;
}

void PolyGroupTool::assignGroupToFace(Mesh* mesh, uint32_t faceIdx, uint32_t groupId) {
    if (!mesh || faceIdx >= (uint32_t)mesh->nbFaces) return;

    if (mesh->faceGroups.size() != (size_t)mesh->nbFaces) {
        mesh->initFaceGroups();
    }

    if (mesh->faceGroups[faceIdx] != groupId) {
        mesh->faceGroups[faceIdx] = groupId;
        mesh->isFaceGroupDirty = true;
    }
}

void PolyGroupTool::floodFillGroup(Mesh* mesh, uint32_t startFaceIdx, uint32_t groupId) {
    if (!mesh || startFaceIdx >= (uint32_t)mesh->nbFaces) return;

    if (mesh->faceGroups.size() != (size_t)mesh->nbFaces) {
        mesh->initFaceGroups();
    }

    uint32_t targetGroup = mesh->faceGroups[startFaceIdx];
    if (targetGroup == groupId) return;

    if (mesh->vrfStartCount.empty() || mesh->vertRingFace.empty()) {
        mesh->postInit();
    }

    std::vector<bool> visited(mesh->nbFaces, false);
    std::vector<uint32_t> queue;
    queue.push_back(startFaceIdx);
    visited[startFaceIdx] = true;

    size_t head = 0;
    while (head < queue.size()) {
        uint32_t f = queue[head++];
        mesh->faceGroups[f] = groupId;

        uint32_t faceVerts[4] = {
            mesh->faces[f * 4],
            mesh->faces[f * 4 + 1],
            mesh->faces[f * 4 + 2],
            mesh->faces[f * 4 + 3]
        };

        for (int k = 0; k < 4; ++k) {
            uint32_t v = faceVerts[k];
            if (v == 0xffffffff || v >= (uint32_t)mesh->nbVerts) continue;
            if (v * 2 + 1 >= mesh->vrfStartCount.size()) continue;

            uint32_t start = mesh->vrfStartCount[v * 2];
            uint32_t count = mesh->vrfStartCount[v * 2 + 1];
            if (start + count > mesh->vertRingFace.size()) continue;

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t nf = mesh->vertRingFace[j];
                if (nf < (uint32_t)mesh->nbFaces && !visited[nf] && mesh->faceGroups[nf] == targetGroup) {
                    visited[nf] = true;
                    queue.push_back(nf);
                }
            }
        }
    }

    mesh->isFaceGroupDirty = true;
    std::cout << "[PolyGroupTool] Flood filled " << queue.size() << " faces to group " << groupId << "." << std::endl;
}

void PolyGroupTool::clearAllGroups(Mesh* mesh) {
    if (!mesh) return;
    mesh->initFaceGroups();
    std::cout << "[PolyGroupTool] Cleared all polygroups." << std::endl;
}

uint32_t PolyGroupTool::getGroupAtFace(const Mesh* mesh, uint32_t faceIdx) const {
    if (!mesh || faceIdx >= mesh->faceGroups.size()) return 0;
    return mesh->faceGroups[faceIdx];
}

std::vector<uint32_t> PolyGroupTool::getAllGroupIDs(const Mesh* mesh) const {
    if (!mesh) return {};
    std::set<uint32_t> uniqueGroups;
    for (uint32_t gid : mesh->faceGroups) {
        if (gid != 0) uniqueGroups.insert(gid);
    }
    return std::vector<uint32_t>(uniqueGroups.begin(), uniqueGroups.end());
}
