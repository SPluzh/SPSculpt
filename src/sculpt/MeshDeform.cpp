#include "sculpt/MeshDeform.h"

#include "common/Constants.h"
#include "editing/undo/TransformUndoEntry.h"
#include "editing/undo/UndoManager.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

extern UndoManager g_undoManager;

namespace {

void computePasses(float amount, int maxPasses, int& outCount, float& outEach) {
    const float t = std::clamp(std::abs(amount), 0.0f, 100.0f) / 100.0f * static_cast<float>(maxPasses);
    outCount = static_cast<int>(std::ceil(t - 1e-4f));
    outEach = outCount > 0 ? t / static_cast<float>(outCount) : 0.0f;
}

std::vector<float> computeFreedomWeights(const Mesh& mesh, const MeshDeformOptions& options) {
    const int nbVerts = mesh.nbVerts;
    std::vector<float> k(static_cast<size_t>(nbVerts), 0.0f);

    const bool hasTopo = mesh.vrvStartCount.size() >= static_cast<size_t>(nbVerts) * 2;
    const bool hasEdge = mesh.vertOnEdge.size() >= static_cast<size_t>(nbVerts);
    const bool hasVis = mesh.vertVisible.size() >= static_cast<size_t>(nbVerts);
    const bool hasMat = options.useMask && mesh.materials.size() >= static_cast<size_t>(nbVerts) * 3;

    #pragma omp parallel for schedule(static) if(nbVerts > 2048)
    for (int v = 0; v < nbVerts; ++v) {
        if (!hasTopo || mesh.vrvStartCount[static_cast<size_t>(v) * 2 + 1] == 0) {
            continue;
        }
        if (options.preserveBoundary && hasEdge && mesh.vertOnEdge[static_cast<size_t>(v)] != 0) {
            continue;
        }
        if (hasVis && mesh.vertVisible[static_cast<size_t>(v)] == 0) {
            continue;
        }
        float w = 1.0f;
        if (hasMat) {
            w = std::clamp(mesh.materials[static_cast<size_t>(v) * 3 + 2], 0.0f, 1.0f);
        }
        k[static_cast<size_t>(v)] = w;
    }

    return k;
}

void computeLaplacianDelta(
    const float* P,
    const uint32_t* vrvStartCount,
    const uint32_t* vertRingVert,
    int v,
    float& outDx,
    float& outDy,
    float& outDz
) {
    const uint32_t start = vrvStartCount[static_cast<size_t>(v) * 2];
    const uint32_t count = vrvStartCount[static_cast<size_t>(v) * 2 + 1];
    if (count == 0) {
        outDx = outDy = outDz = 0.0f;
        return;
    }

    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    for (uint32_t j = 0; j < count; ++j) {
        const size_t n3 = static_cast<size_t>(vertRingVert[start + j]) * 3;
        sx += P[n3 + 0];
        sy += P[n3 + 1];
        sz += P[n3 + 2];
    }

    const float inv = 1.0f / static_cast<float>(count);
    const size_t v3 = static_cast<size_t>(v) * 3;
    outDx = sx * inv - P[v3 + 0];
    outDy = sy * inv - P[v3 + 1];
    outDz = sz * inv - P[v3 + 2];
}

float computeBBoxDiagonal(const std::vector<float>& verts, int nbVerts) {
    if (nbVerts <= 0 || verts.size() < static_cast<size_t>(nbVerts) * 3) {
        return 0.0f;
    }
    float minX = verts[0], maxX = verts[0];
    float minY = verts[1], maxY = verts[1];
    float minZ = verts[2], maxZ = verts[2];
    for (int i = 1; i < nbVerts; ++i) {
        const size_t idx = static_cast<size_t>(i) * 3;
        const float x = verts[idx + 0];
        const float y = verts[idx + 1];
        const float z = verts[idx + 2];
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
        if (z < minZ) minZ = z;
        if (z > maxZ) maxZ = z;
    }
    const float dx = maxX - minX;
    const float dy = maxY - minY;
    const float dz = maxZ - minZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void laplacianStep(
    const Mesh& mesh,
    const std::vector<float>& k,
    std::vector<float>& P,
    std::vector<float>& Q,
    float factor
) {
    const int nbVerts = mesh.nbVerts;
    const uint32_t* vrvStartCount = mesh.vrvStartCount.data();
    const uint32_t* vertRingVert = mesh.vertRingVert.data();
    Q.resize(P.size());

    #pragma omp parallel for schedule(static) if(nbVerts > 2048)
    for (int v = 0; v < nbVerts; ++v) {
        const size_t v3 = static_cast<size_t>(v) * 3;
        const float kv = k[static_cast<size_t>(v)];
        if (kv <= 0.0f) {
            Q[v3 + 0] = P[v3 + 0];
            Q[v3 + 1] = P[v3 + 1];
            Q[v3 + 2] = P[v3 + 2];
            continue;
        }
        float dx, dy, dz;
        computeLaplacianDelta(P.data(), vrvStartCount, vertRingVert, v, dx, dy, dz);
        const float scale = factor * kv;
        Q[v3 + 0] = P[v3 + 0] + dx * scale;
        Q[v3 + 1] = P[v3 + 1] + dy * scale;
        Q[v3 + 2] = P[v3 + 2] + dz * scale;
    }

    P.swap(Q);
}

} // namespace

bool deformMeshPositions(
    const Mesh& mesh,
    const std::vector<float>& baseVerts,
    MeshDeformOp op,
    const MeshDeformOptions& options,
    std::vector<float>& outVerts
) {
    const int nbVerts = mesh.nbVerts;
    const int nbFaces = mesh.nbFaces;
    if (nbVerts <= 0 || nbFaces <= 0) {
        return false;
    }
    if (baseVerts.size() < static_cast<size_t>(nbVerts) * 3) {
        return false;
    }
    if (mesh.vrvStartCount.size() < static_cast<size_t>(nbVerts) * 2) {
        return false;
    }

    outVerts = baseVerts;
    if (std::abs(options.amount) <= 1e-4f) {
        return true;
    }

    const std::vector<float> k = computeFreedomWeights(mesh, options);
    std::vector<float> Q;

    switch (op) {
        case MeshDeformOp::Smooth: {
            int count = 0;
            float each = 0.0f;
            computePasses(options.amount, 20, count, each);
            const float lambda = 0.5f * each;
            // Taubin pass-band frequency 0.1: 1/lambda + 1/mu = 0.1
            const float mu = 1.0f / (0.1f - 1.0f / std::max(lambda, 1e-3f));
            for (int i = 0; i < count; ++i) {
                laplacianStep(mesh, k, outVerts, Q, lambda);
                if (options.preserveVolume) {
                    laplacianStep(mesh, k, outVerts, Q, mu);
                }
            }
            break;
        }

        case MeshDeformOp::Relax: {
            int count = 0;
            float each = 0.0f;
            computePasses(options.amount, 20, count, each);
            const float lambda = 0.5f * each;

            std::vector<float> faceNormals(static_cast<size_t>(nbFaces) * 3, 0.0f);
            std::vector<float> vertNormals(static_cast<size_t>(nbVerts) * 3, 0.0f);
            const uint32_t* vrvStartCount = mesh.vrvStartCount.data();
            const uint32_t* vertRingVert = mesh.vertRingVert.data();

            for (int i = 0; i < count; ++i) {
                updateFaceNormalsOnly(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data());
                computeNormalsDirect(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data(), vertNormals.data());

                Q.resize(outVerts.size());
                #pragma omp parallel for schedule(static) if(nbVerts > 2048)
                for (int v = 0; v < nbVerts; ++v) {
                    const size_t v3 = static_cast<size_t>(v) * 3;
                    const float kv = k[static_cast<size_t>(v)];
                    if (kv <= 0.0f) {
                        Q[v3 + 0] = outVerts[v3 + 0];
                        Q[v3 + 1] = outVerts[v3 + 1];
                        Q[v3 + 2] = outVerts[v3 + 2];
                        continue;
                    }

                    float dx, dy, dz;
                    computeLaplacianDelta(outVerts.data(), vrvStartCount, vertRingVert, v, dx, dy, dz);

                    const float nx = vertNormals[v3 + 0];
                    const float ny = vertNormals[v3 + 1];
                    const float nz = vertNormals[v3 + 2];
                    const float dotN = nx * dx + ny * dy + nz * dz;

                    const float tx = dx - nx * dotN;
                    const float ty = dy - ny * dotN;
                    const float tz = dz - nz * dotN;

                    const float scale = lambda * kv;
                    Q[v3 + 0] = outVerts[v3 + 0] + tx * scale;
                    Q[v3 + 1] = outVerts[v3 + 1] + ty * scale;
                    Q[v3 + 2] = outVerts[v3 + 2] + tz * scale;
                }
                outVerts.swap(Q);
            }
            break;
        }

        case MeshDeformOp::Inflate: {
            int count = 0;
            float each = 0.0f;
            computePasses(options.amount, 10, count, each);
            const float diagonal = computeBBoxDiagonal(baseVerts, nbVerts);
            if (diagonal <= 1e-8f) {
                break;
            }
            const float sign = options.amount < 0.0f ? -1.0f : 1.0f;
            const float push = sign * 0.01f * diagonal * each;

            std::vector<float> faceNormals(static_cast<size_t>(nbFaces) * 3, 0.0f);
            std::vector<float> vertNormals(static_cast<size_t>(nbVerts) * 3, 0.0f);

            for (int i = 0; i < count; ++i) {
                updateFaceNormalsOnly(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data());
                computeNormalsDirect(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data(), vertNormals.data());

                Q.resize(outVerts.size());
                #pragma omp parallel for schedule(static) if(nbVerts > 2048)
                for (int v = 0; v < nbVerts; ++v) {
                    const size_t v3 = static_cast<size_t>(v) * 3;
                    const float kv = k[static_cast<size_t>(v)];
                    if (kv <= 0.0f) {
                        Q[v3 + 0] = outVerts[v3 + 0];
                        Q[v3 + 1] = outVerts[v3 + 1];
                        Q[v3 + 2] = outVerts[v3 + 2];
                        continue;
                    }
                    const float stepPush = push * kv;
                    Q[v3 + 0] = outVerts[v3 + 0] + vertNormals[v3 + 0] * stepPush;
                    Q[v3 + 1] = outVerts[v3 + 1] + vertNormals[v3 + 1] * stepPush;
                    Q[v3 + 2] = outVerts[v3 + 2] + vertNormals[v3 + 2] * stepPush;
                }
                outVerts.swap(Q);
            }
            break;
        }

        case MeshDeformOp::Balloon: {
            int count = 0;
            float each = 0.0f;
            computePasses(options.amount, 10, count, each);
            const float diagonal = computeBBoxDiagonal(baseVerts, nbVerts);
            if (diagonal <= 1e-8f) {
                break;
            }
            const float sign = options.amount < 0.0f ? -1.0f : 1.0f;
            const float push = sign * 0.01f * diagonal * each;

            std::vector<float> faceNormals(static_cast<size_t>(nbFaces) * 3, 0.0f);
            std::vector<float> vertNormals(static_cast<size_t>(nbVerts) * 3, 0.0f);

            for (int i = 0; i < count; ++i) {
                updateFaceNormalsOnly(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data());
                computeNormalsDirect(outVerts.data(), nbVerts, mesh.faces.data(), nbFaces, faceNormals.data(), vertNormals.data());

                Q.resize(outVerts.size());
                #pragma omp parallel for schedule(static) if(nbVerts > 2048)
                for (int v = 0; v < nbVerts; ++v) {
                    const size_t v3 = static_cast<size_t>(v) * 3;
                    const float kv = k[static_cast<size_t>(v)];
                    if (kv <= 0.0f) {
                        Q[v3 + 0] = outVerts[v3 + 0];
                        Q[v3 + 1] = outVerts[v3 + 1];
                        Q[v3 + 2] = outVerts[v3 + 2];
                        continue;
                    }
                    const float stepPush = push * kv;
                    Q[v3 + 0] = outVerts[v3 + 0] + vertNormals[v3 + 0] * stepPush;
                    Q[v3 + 1] = outVerts[v3 + 1] + vertNormals[v3 + 1] * stepPush;
                    Q[v3 + 2] = outVerts[v3 + 2] + vertNormals[v3 + 2] * stepPush;
                }
                outVerts.swap(Q);

                // Laplacian tension pull (Balloon effect: bulges out and rounds up corners)
                laplacianStep(mesh, k, outVerts, Q, 0.5f * each);
            }
            break;
        }

        case MeshDeformOp::Polish: {
            int count = 0;
            float each = 0.0f;
            computePasses(options.amount, 6, count, each);

            const uint32_t* faces = mesh.faces.data();
            const uint32_t* vrfStartCount = mesh.vrfStartCount.data();
            const uint32_t* vertRingFace = mesh.vertRingFace.data();

            struct FaceData {
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                float area = 0.0f;
            };
            std::vector<FaceData> fd(static_cast<size_t>(nbFaces));
            std::vector<float> fnX(static_cast<size_t>(nbFaces));
            std::vector<float> fnY(static_cast<size_t>(nbFaces));
            std::vector<float> fnZ(static_cast<size_t>(nbFaces));

            auto evalFaceData = [&]() {
                #pragma omp parallel for schedule(static) if(nbFaces > 2048)
                for (int t = 0; t < nbFaces; ++t) {
                    const size_t f4 = static_cast<size_t>(t) * 4;
                    const uint32_t i0 = faces[f4 + 0];
                    const uint32_t i1 = faces[f4 + 1];
                    const uint32_t i2 = faces[f4 + 2];
                    const uint32_t i3 = faces[f4 + 3];

                    const size_t v0 = static_cast<size_t>(i0) * 3;
                    const size_t v1 = static_cast<size_t>(i1) * 3;
                    const size_t v2 = static_cast<size_t>(i2) * 3;

                    const float ax = outVerts[v1 + 0] - outVerts[v0 + 0];
                    const float ay = outVerts[v1 + 1] - outVerts[v0 + 1];
                    const float az = outVerts[v1 + 2] - outVerts[v0 + 2];

                    const float bx = outVerts[v2 + 0] - outVerts[v0 + 0];
                    const float by = outVerts[v2 + 1] - outVerts[v0 + 1];
                    const float bz = outVerts[v2 + 2] - outVerts[v0 + 2];

                    float cx = ay * bz - az * by;
                    float cy = az * bx - ax * bz;
                    float cz = ax * by - ay * bx;
                    const float len = std::sqrt(cx * cx + cy * cy + cz * cz);

                    FaceData& d = fd[static_cast<size_t>(t)];
                    d.area = 0.5f * len;
                    if (len > 1e-12f) {
                        d.nx = cx / len;
                        d.ny = cy / len;
                        d.nz = cz / len;
                    } else {
                        d.nx = d.ny = d.nz = 0.0f;
                    }

                    if (i3 != TRI_INDEX) {
                        const size_t v3 = static_cast<size_t>(i3) * 3;
                        d.cx = 0.25f * (outVerts[v0 + 0] + outVerts[v1 + 0] + outVerts[v2 + 0] + outVerts[v3 + 0]);
                        d.cy = 0.25f * (outVerts[v0 + 1] + outVerts[v1 + 1] + outVerts[v2 + 1] + outVerts[v3 + 1]);
                        d.cz = 0.25f * (outVerts[v0 + 2] + outVerts[v1 + 2] + outVerts[v2 + 2] + outVerts[v3 + 2]);
                    } else {
                        d.cx = (1.0f / 3.0f) * (outVerts[v0 + 0] + outVerts[v1 + 0] + outVerts[v2 + 0]);
                        d.cy = (1.0f / 3.0f) * (outVerts[v0 + 1] + outVerts[v1 + 1] + outVerts[v2 + 1]);
                        d.cz = (1.0f / 3.0f) * (outVerts[v0 + 2] + outVerts[v1 + 2] + outVerts[v2 + 2]);
                    }
                }
            };

            constexpr float kSigmaNormal = 0.35f; // ~20 deg tolerance, preserves 90 deg edges
            constexpr float kInv2SigmaN2 = 1.0f / (2.0f * kSigmaNormal * kSigmaNormal);

            for (int p = 0; p < count; ++p) {
                evalFaceData();

                #pragma omp parallel for schedule(static) if(nbFaces > 2048)
                for (int t = 0; t < nbFaces; ++t) {
                    const FaceData& curr = fd[static_cast<size_t>(t)];
                    const size_t f4 = static_cast<size_t>(t) * 4;
                    const int cornerCount = (faces[f4 + 3] != TRI_INDEX) ? 4 : 3;

                    float spacing = 0.0f;
                    for (int c = 0; c < cornerCount; ++c) {
                        const size_t cv3 = static_cast<size_t>(faces[f4 + c]) * 3;
                        const float dx = outVerts[cv3 + 0] - curr.cx;
                        const float dy = outVerts[cv3 + 1] - curr.cy;
                        const float dz = outVerts[cv3 + 2] - curr.cz;
                        spacing += std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                    spacing = std::max(spacing / static_cast<float>(cornerCount) * 2.0f, 1e-10f);
                    const float inv2Spacing2 = 1.0f / (2.0f * spacing * spacing);

                    float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
                    for (int c = 0; c < cornerCount; ++c) {
                        const uint32_t vertId = faces[f4 + c];
                        const uint32_t ringStart = vrfStartCount[static_cast<size_t>(vertId) * 2];
                        const uint32_t ringCount = vrfStartCount[static_cast<size_t>(vertId) * 2 + 1];

                        for (uint32_t r = 0; r < ringCount; ++r) {
                            const uint32_t g = vertRingFace[ringStart + r];
                            const FaceData& nbr = fd[static_cast<size_t>(g)];

                            const float dnx = nbr.nx - curr.nx;
                            const float dny = nbr.ny - curr.ny;
                            const float dnz = nbr.nz - curr.nz;
                            const float distNormSq = dnx * dnx + dny * dny + dnz * dnz;

                            const float dcx = nbr.cx - curr.cx;
                            const float dcy = nbr.cy - curr.cy;
                            const float dcz = nbr.cz - curr.cz;
                            const float distCentSq = dcx * dcx + dcy * dcy + dcz * dcz;

                            const float w = nbr.area * std::exp(-distCentSq * inv2Spacing2) *
                                                      std::exp(-distNormSq * kInv2SigmaN2);
                            sumX += nbr.nx * w;
                            sumY += nbr.ny * w;
                            sumZ += nbr.nz * w;
                        }
                    }

                    const float len = std::sqrt(sumX * sumX + sumY * sumY + sumZ * sumZ);
                    if (len > 1e-12f) {
                        fnX[static_cast<size_t>(t)] = sumX / len;
                        fnY[static_cast<size_t>(t)] = sumY / len;
                        fnZ[static_cast<size_t>(t)] = sumZ / len;
                    } else {
                        fnX[static_cast<size_t>(t)] = curr.nx;
                        fnY[static_cast<size_t>(t)] = curr.ny;
                        fnZ[static_cast<size_t>(t)] = curr.nz;
                    }
                }

                // Fit vertices to filtered face normals (Sun et al. 2007)
                for (int fit = 0; fit < 3; ++fit) {
                    if (fit > 0) {
                        evalFaceData();
                    }
                    Q.resize(outVerts.size());

                    #pragma omp parallel for schedule(static) if(nbVerts > 2048)
                    for (int v = 0; v < nbVerts; ++v) {
                        const size_t v3 = static_cast<size_t>(v) * 3;
                        const float kv = k[static_cast<size_t>(v)];
                        if (kv <= 0.0f) {
                            Q[v3 + 0] = outVerts[v3 + 0];
                            Q[v3 + 1] = outVerts[v3 + 1];
                            Q[v3 + 2] = outVerts[v3 + 2];
                            continue;
                        }

                        const uint32_t ringStart = vrfStartCount[static_cast<size_t>(v) * 2];
                        const uint32_t ringCount = vrfStartCount[static_cast<size_t>(v) * 2 + 1];
                        if (ringCount == 0) {
                            Q[v3 + 0] = outVerts[v3 + 0];
                            Q[v3 + 1] = outVerts[v3 + 1];
                            Q[v3 + 2] = outVerts[v3 + 2];
                            continue;
                        }

                        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
                        const float px = outVerts[v3 + 0];
                        const float py = outVerts[v3 + 1];
                        const float pz = outVerts[v3 + 2];

                        for (uint32_t r = 0; r < ringCount; ++r) {
                            const uint32_t t = vertRingFace[ringStart + r];
                            const float fx = fnX[static_cast<size_t>(t)];
                            const float fy = fnY[static_cast<size_t>(t)];
                            const float fz = fnZ[static_cast<size_t>(t)];
                            const FaceData& d = fd[static_cast<size_t>(t)];

                            const float dot = fx * (d.cx - px) + fy * (d.cy - py) + fz * (d.cz - pz);
                            dx += fx * dot;
                            dy += fy * dot;
                            dz += fz * dot;
                        }

                        const float stepFactor = (each / static_cast<float>(ringCount)) * kv;
                        Q[v3 + 0] = px + dx * stepFactor;
                        Q[v3 + 1] = py + dy * stepFactor;
                        Q[v3 + 2] = pz + dz * stepFactor;
                    }
                    outVerts.swap(Q);
                }
            }
            break;
        }
    }

    return true;
}

void applyDeformedPositionsToMesh(
    Mesh& mesh,
    const std::vector<float>& newVerts,
    bool updateBVH
) {
    if (mesh.nbVerts <= 0 || newVerts.size() < static_cast<size_t>(mesh.nbVerts) * 3) {
        return;
    }

    mesh.verts = newVerts;
    mesh.vertProxy = newVerts;

    if (mesh.faceNormals.size() < static_cast<size_t>(mesh.nbFaces) * 3) {
        mesh.faceNormals.resize(static_cast<size_t>(mesh.nbFaces) * 3);
    }
    if (mesh.faceBoxes.size() < static_cast<size_t>(mesh.nbFaces) * 6) {
        mesh.faceBoxes.resize(static_cast<size_t>(mesh.nbFaces) * 6);
    }
    if (mesh.faceCenters.size() < static_cast<size_t>(mesh.nbFaces) * 3) {
        mesh.faceCenters.resize(static_cast<size_t>(mesh.nbFaces) * 3);
    }
    if (mesh.normals.size() < static_cast<size_t>(mesh.nbVerts) * 3) {
        mesh.normals.resize(static_cast<size_t>(mesh.nbVerts) * 3);
    }

    updateFaceNormalsAndBoxes(
        mesh.verts.data(), mesh.nbVerts,
        mesh.faces.data(), mesh.nbFaces,
        nullptr, -1,
        mesh.faceNormals.data(),
        mesh.faceBoxes.data(),
        mesh.faceCenters.data()
    );
    computeNormalsDirect(
        mesh.verts.data(), mesh.nbVerts,
        mesh.faces.data(), mesh.nbFaces,
        mesh.faceNormals.data(),
        mesh.normals.data()
    );

    if (updateBVH && mesh.nbFaces > 0) {
        mesh.bvh.refitAll(mesh, true);
    }

    mesh.invalidateLocalRadius();
    mesh.bumpVertsGeneration();
    mesh.markDirtyVertRange(0, static_cast<uint32_t>(mesh.nbVerts));
    mesh.isVertexDirty = true;
    mesh.isDirty = true;

    if (mesh.dynamicSettings.enabled) {
        mesh.invalidateDynamicSubdiv();
    }
}

void commitMeshDeformationUndo(
    Scene& scene,
    Mesh& mesh,
    std::vector<float>&& beforeVerts,
    std::vector<float>&& beforeNormals,
    const std::string& description
) {
    if (mesh.isLayerRecording()) {
        // Restore pre-deform state briefly so pushSculptOperation records both vertex & layer deltas accurately
        std::vector<float> afterVerts = mesh.verts;
        mesh.verts = std::move(beforeVerts);
        mesh.normals = std::move(beforeNormals);
        g_undoManager.pushSculptOperation(
            scene,
            mesh.getID(),
            description,
            [&]() {
                applyDeformedPositionsToMesh(mesh, afterVerts, true);
            },
            false,
            false
        );
        return;
    }

    TransformUndoEntry entry(description);
    TransformMeshState state;
    state.meshId = mesh.getID();
    state.beforeMatrix = mesh.matrix;
    state.afterMatrix = mesh.matrix;
    state.hasVertexDeformation = true;
    state.beforeVerts = std::move(beforeVerts);
    state.afterVerts = mesh.verts;
    state.beforeNormals = std::move(beforeNormals);
    state.afterNormals = mesh.normals;
    entry.m_meshStates.push_back(std::move(state));

    g_undoManager.pushTransformChange(scene, std::move(entry));
}
