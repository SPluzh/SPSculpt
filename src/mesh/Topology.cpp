#include "mesh/Topology.h"
#include <algorithm>

void computeTopology(
    int nbVerts, const uint32_t* faces, int nbFaces,
    std::vector<uint32_t>& vrfStartCount, std::vector<uint32_t>& vertRingFace,
    std::vector<uint32_t>& vrvStartCount, std::vector<uint32_t>& vertRingVert,
    std::vector<uint8_t>&  vertOnEdge
) {
    std::vector<std::vector<uint32_t>> vToF(nbVerts);
    for (int i = 0; i < nbFaces; ++i) {
        for (int j = 0; j < 4; ++j) {
            uint32_t vid = faces[i * 4 + j];
            if (vid != 0xffffffff && vid < (uint32_t)nbVerts) {
                vToF[vid].push_back(i);
            }
        }
    }

    vrfStartCount.resize(nbVerts * 2);
    vertRingFace.clear();
    for (int i = 0; i < nbVerts; ++i) {
        vrfStartCount[i * 2] = vertRingFace.size();
        vrfStartCount[i * 2 + 1] = vToF[i].size();
        vertRingFace.insert(vertRingFace.end(), vToF[i].begin(), vToF[i].end());
    }

    std::vector<std::vector<uint32_t>> vToV(nbVerts);
    for (int i = 0; i < nbFaces; ++i) {
        uint32_t f[4] = { faces[i*4], faces[i*4+1], faces[i*4+2], faces[i*4+3] };
        int num = (f[3] == 0xffffffff) ? 3 : 4;
        for (int j = 0; j < num; ++j) {
            uint32_t curr = f[j];
            uint32_t next = f[(j + 1) % num];
            if (curr < (uint32_t)nbVerts && next < (uint32_t)nbVerts) {
                vToV[curr].push_back(next);
                vToV[next].push_back(curr);
            }
        }
    }

    vrvStartCount.resize(nbVerts * 2);
    vertRingVert.clear();
    vertOnEdge.assign(nbVerts, 0);

    for (int i = 0; i < nbVerts; ++i) {
        auto& neighbors = vToV[i];
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

        vrvStartCount[i * 2] = vertRingVert.size();
        vrvStartCount[i * 2 + 1] = neighbors.size();
        vertRingVert.insert(vertRingVert.end(), neighbors.begin(), neighbors.end());

        if (vrfStartCount[i * 2 + 1] != neighbors.size()) {
            vertOnEdge[i] = 1;
        }
    }
}
