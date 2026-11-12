#include "common/Symmetry.h"
#include "mesh/Mesh.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

bool sameCopy(const SymCopy& a, const SymCopy& b) {
    float dm = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const glm::vec3 diff = a.m[c] - b.m[c];
        dm += glm::dot(diff, diff);
    }
    const glm::vec3 dt = a.translation() - b.translation();
    return (dm + glm::dot(dt, dt)) < 1e-7f;
}

} // namespace

std::vector<SymCopy> buildSymmetryCopies(const SymmetryParams& params, const Mesh* mesh, bool includeIdentity) {
    std::vector<SymCopy> out;
    if (!params.anyActive()) {
        if (includeIdentity) {
            out.push_back(SymCopy{});
        }
        return out;
    }

    std::vector<float> xVals = { 1.0f };
    std::vector<float> yVals = { 1.0f };
    std::vector<float> zVals = { 1.0f };
    if (params.symX) xVals.push_back(-1.0f);
    if (params.symY) yVals.push_back(-1.0f);
    if (params.symZ) zVals.push_back(-1.0f);

    const int n = params.radialOn() ? std::clamp(params.radialCount, 2, SymmetryParams::kMaxRadial) : 1;
    const int axis = std::clamp(params.radialAxis, 0, 2);
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;

    const bool isWorld = (params.mode == SymmetryMode::World) && (mesh != nullptr);
    float off = 0.0f;
    if (mesh) {
        const float radius = isWorld ? (mesh->computeLocalRadius() * mesh->scale) : mesh->computeLocalRadius();
        off = mesh->symmetryOffset * radius;
    }

    const glm::mat4 M = isWorld ? mesh->matrix : glm::mat4(1.0f);
    const glm::mat4 invM = isWorld ? glm::inverse(M) : glm::mat4(1.0f);
    const glm::vec3 localWorldOrigin = isWorld ? glm::vec3(invM * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)) : glm::vec3(0.0f);

    out.reserve(xVals.size() * yVals.size() * zVals.size() * static_cast<size_t>(n));

    for (float sx : xVals) {
        for (float sy : yVals) {
            for (float sz : zVals) {
                glm::mat4 mirrorMat(1.0f);
                mirrorMat[0][0] = sx;
                mirrorMat[1][1] = sy;
                mirrorMat[2][2] = sz;
                if (sx < 0.0f) mirrorMat[3][0] = 2.0f * off;
                if (sy < 0.0f) mirrorMat[3][1] = 2.0f * off;
                if (sz < 0.0f) mirrorMat[3][2] = 2.0f * off;

                for (int k = 0; k < n; ++k) {
                    glm::mat4 rotMat(1.0f);
                    if (k > 0) {
                        const double angle = kTwoPi * static_cast<double>(k) / static_cast<double>(n);
                        float c = static_cast<float>(std::cos(angle));
                        float si = static_cast<float>(std::sin(angle));
                        if ((4 * k) % n == 0) {
                            c = std::round(c);
                            si = std::round(si);
                        }
                        rotMat[u][u] = c;
                        rotMat[u][v] = si;
                        rotMat[v][u] = -si;
                        rotMat[v][v] = c;
                    }

                    const glm::mat4 W = mirrorMat * rotMat;
                    const glm::mat4 localMat = isWorld ? (invM * W * M) : W;

                    SymCopy copy;
                    copy.m = glm::mat3(localMat);
                    copy.offset = glm::vec3(localMat[3]);
                    copy.hasOffset = true;
                    copy.pivot = localWorldOrigin;

                    bool repeated = false;
                    for (const SymCopy& existing : out) {
                        if (sameCopy(existing, copy)) {
                            repeated = true;
                            break;
                        }
                    }
                    if (!repeated) {
                        out.push_back(copy);
                    }
                }
            }
        }
    }

    if (!includeIdentity && !out.empty() && out.front().identity()) {
        out.erase(out.begin());
    }

    return out;
}

std::vector<size_t> symmetryInverses(std::span<const SymCopy> copies) {
    std::vector<size_t> out(copies.size(), 0);
    for (size_t i = 0; i < copies.size(); ++i) {
        const glm::mat3 t = glm::transpose(copies[i].m);
        const glm::vec3 invTrans = -t * copies[i].translation();
        float best = 1e30f;
        for (size_t j = 0; j < copies.size(); ++j) {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const glm::vec3 diff = copies[j].m[c] - t[c];
                d += glm::dot(diff, diff);
            }
            const glm::vec3 dt = copies[j].translation() - invTrans;
            d += glm::dot(dt, dt);
            if (d < best) {
                best = d;
                out[i] = j;
            }
        }
    }
    return out;
}

