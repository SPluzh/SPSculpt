#include "mesh/Topology.h"
#include <algorithm>

void computeTopology(
    int nbVerts, const uint32_t* faces, int nbFaces,
    std::vector<uint32_t>& vrfStartCount, std::vector<uint32_t>& vertRingFace,
    std::vector<uint32_t>& vrvStartCount, std::vector<uint32_t>& vertRingVert,
    std::vector<uint8_t>&  vertOnEdge
) {
    if (nbVerts <= 0) {
        vrfStartCount.clear();
        vertRingFace.clear();
        vrvStartCount.clear();
        vertRingVert.clear();
        vertOnEdge.clear();
        return;
    }

    // 1. Vertex to Face (vToF) flat buffer
    std::vector<uint32_t> fCounts(nbVerts, 0);
    for (int i = 0; i < nbFaces; ++i) {
        for (int j = 0; j < 4; ++j) {
            uint32_t vid = faces[i * 4 + j];
            if (vid != 0xffffffff && vid < static_cast<uint32_t>(nbVerts)) {
                fCounts[vid]++;
            }
        }
    }

    vrfStartCount.resize(nbVerts * 2);
    uint32_t totalF = 0;
    std::vector<uint32_t> fOffsets(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        vrfStartCount[i * 2] = totalF;
        vrfStartCount[i * 2 + 1] = fCounts[i];
        fOffsets[i] = totalF;
        totalF += fCounts[i];
    }

    vertRingFace.resize(totalF);
    std::vector<uint32_t> fHeads = fOffsets;
    for (int i = 0; i < nbFaces; ++i) {
        for (int j = 0; j < 4; ++j) {
            uint32_t vid = faces[i * 4 + j];
            if (vid != 0xffffffff && vid < static_cast<uint32_t>(nbVerts)) {
                vertRingFace[fHeads[vid]++] = i;
            }
        }
    }

    // 2. Vertex to Vertex (vToV) flat buffer
    std::vector<uint32_t> vCounts(nbVerts, 0);
    for (int i = 0; i < nbFaces; ++i) {
        uint32_t f[4] = { faces[i * 4], faces[i * 4 + 1], faces[i * 4 + 2], faces[i * 4 + 3] };
        int num = (f[3] == 0xffffffff) ? 3 : 4;
        for (int j = 0; j < num; ++j) {
            uint32_t curr = f[j];
            uint32_t next = f[(j + 1) % num];
            if (curr < static_cast<uint32_t>(nbVerts) && next < static_cast<uint32_t>(nbVerts)) {
                vCounts[curr]++;
                vCounts[next]++;
            }
        }
    }

    std::vector<uint32_t> vOffsets(nbVerts);
    uint32_t totalV = 0;
    for (int i = 0; i < nbVerts; ++i) {
        vOffsets[i] = totalV;
        totalV += vCounts[i];
    }

    std::vector<uint32_t> tempVertRingVert(totalV);
    std::vector<uint32_t> vHeads = vOffsets;
    for (int i = 0; i < nbFaces; ++i) {
        uint32_t f[4] = { faces[i * 4], faces[i * 4 + 1], faces[i * 4 + 2], faces[i * 4 + 3] };
        int num = (f[3] == 0xffffffff) ? 3 : 4;
        for (int j = 0; j < num; ++j) {
            uint32_t curr = f[j];
            uint32_t next = f[(j + 1) % num];
            if (curr < static_cast<uint32_t>(nbVerts) && next < static_cast<uint32_t>(nbVerts)) {
                tempVertRingVert[vHeads[curr]++] = next;
                tempVertRingVert[vHeads[next]++] = curr;
            }
        }
    }

    // 3. Sort, unique, and compact neighbors
    vrvStartCount.resize(nbVerts * 2);
    vertRingVert.clear();
    vertRingVert.reserve(totalV);
    vertOnEdge.assign(nbVerts, 0);

    for (int i = 0; i < nbVerts; ++i) {
        uint32_t start = vOffsets[i];
        uint32_t count = vCounts[i];
        auto* begin = tempVertRingVert.data() + start;
        auto* end = begin + count;

        std::sort(begin, end);
        auto* newEnd = std::unique(begin, end);
        uint32_t uniqueCount = static_cast<uint32_t>(newEnd - begin);

        vrvStartCount[i * 2] = static_cast<uint32_t>(vertRingVert.size());
        vrvStartCount[i * 2 + 1] = uniqueCount;
        vertRingVert.insert(vertRingVert.end(), begin, newEnd);

        if (vrfStartCount[i * 2 + 1] != uniqueCount) {
            vertOnEdge[i] = 1;
        }
    }
}
