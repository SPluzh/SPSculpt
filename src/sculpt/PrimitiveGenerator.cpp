#include "sculpt/PrimitiveGenerator.h"
#include "common/Constants.h"
#include <cmath>
#include <map>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

namespace PrimitiveGenerator {

void generateSphere(
    float radius, int rings, int sectors,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (rings < 3) rings = 3;
    if (sectors < 3) sectors = 3;
    int startIdx = static_cast<int>(vertices.size() / 3);

    // South Pole (index startIdx)
    vertices.push_back(0.0f); vertices.push_back(-radius); vertices.push_back(0.0f);
    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
    normals.push_back(0.0f); normals.push_back(-1.0f); normals.push_back(0.0f);

    // Intermediate rings (r = 1 to rings - 2)
    for (int r = 1; r < rings - 1; ++r) {
        float phi = -M_PI_2 + M_PI * (float)r / (float)(rings - 1);
        float y = radius * std::sin(phi);
        float cos_phi = std::cos(phi);

        for (int s = 0; s < sectors; ++s) {
            float theta = 2.0f * (float)M_PI * (float)s / (float)sectors;
            float x = radius * cos_phi * std::cos(theta);
            float z = radius * cos_phi * std::sin(theta);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
            normals.push_back(x / radius); normals.push_back(y / radius); normals.push_back(z / radius);
        }
    }

    // North Pole (index northPoleIdx)
    uint32_t northPoleIdx = static_cast<uint32_t>(vertices.size() / 3);
    vertices.push_back(0.0f); vertices.push_back(radius); vertices.push_back(0.0f);
    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
    normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);

    // South pole faces (r = 0 to r = 1)
    uint32_t southPoleIdx = static_cast<uint32_t>(startIdx);
    for (int s = 0; s < sectors; ++s) {
        int s_next = (s + 1) % sectors;
        uint32_t v_curr = startIdx + 1 + s;
        uint32_t v_next = startIdx + 1 + s_next;

        faces.push_back(southPoleIdx);
        faces.push_back(v_curr);
        faces.push_back(v_next);
        faces.push_back(TRI_INDEX);
    }

    // Intermediate ring faces
    for (int r = 1; r < rings - 2; ++r) {
        for (int s = 0; s < sectors; ++s) {
            int s_next = (s + 1) % sectors;
            uint32_t v0 = startIdx + 1 + (r - 1) * sectors + s;
            uint32_t v1 = startIdx + 1 + (r - 1) * sectors + s_next;
            uint32_t v2 = startIdx + 1 + r * sectors + s_next;
            uint32_t v3 = startIdx + 1 + r * sectors + s;

            faces.push_back(v0);
            faces.push_back(v3);
            faces.push_back(v2);
            faces.push_back(v1);
        }
    }

    // North pole faces
    int lastRingStart = startIdx + 1 + (rings - 3) * sectors;
    for (int s = 0; s < sectors; ++s) {
        int s_next = (s + 1) % sectors;
        uint32_t v_curr = lastRingStart + s;
        uint32_t v_next = lastRingStart + s_next;

        faces.push_back(v_curr);
        faces.push_back(northPoleIdx);
        faces.push_back(v_next);
        faces.push_back(TRI_INDEX);
    }
}

void generateGeosphere(
    float radius, int subdivision,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    int sub = subdivision * 8;
    if (sub < 1) sub = 1;

    struct VecCompare {
        bool operator()(const glm::vec3& a, const glm::vec3& b) const {
            float eps = 1e-4f;
            if (std::abs(a.x - b.x) > eps) return a.x < b.x;
            if (std::abs(a.y - b.y) > eps) return a.y < b.y;
            if (std::abs(a.z - b.z) > eps) return a.z < b.z;
            return false;
        }
    };

    std::map<glm::vec3, uint32_t, VecCompare> vertexMap;
    std::vector<glm::vec3> uniqueVertices;

    auto getUniqueVertex = [&](const glm::vec3& pos) {
        glm::vec3 spherePos = glm::normalize(pos) * radius;
        auto it = vertexMap.find(spherePos);
        if (it != vertexMap.end()) {
            return it->second;
        }
        uint32_t index = static_cast<uint32_t>(uniqueVertices.size());
        uniqueVertices.push_back(spherePos);
        vertexMap[spherePos] = index;
        return index;
    };

    float side = 2.0f;
    float half = 1.0f;
    int startIdx = static_cast<int>(vertices.size() / 3);

    auto generateFace = [&](glm::vec3 origin, glm::vec3 right, glm::vec3 up) {
        std::vector<std::vector<uint32_t>> grid(sub + 1, std::vector<uint32_t>(sub + 1));
        for (int y = 0; y <= sub; ++y) {
            float v = (float)y / sub;
            for (int x = 0; x <= sub; ++x) {
                float u = (float)x / sub;
                glm::vec3 pos = origin + (u - 0.5f) * side * right + (v - 0.5f) * side * up;
                grid[y][x] = getUniqueVertex(pos);
            }
        }

        for (int y = 0; y < sub; ++y) {
            for (int x = 0; x < sub; ++x) {
                uint32_t v0 = grid[y][x];
                uint32_t v1 = grid[y][x + 1];
                uint32_t v2 = grid[y + 1][x + 1];
                uint32_t v3 = grid[y + 1][x];

                faces.push_back(startIdx + v0);
                faces.push_back(startIdx + v1);
                faces.push_back(startIdx + v2);
                faces.push_back(startIdx + v3);
            }
        }
    };

    generateFace(glm::vec3(0, 0, half), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(0, 0, -half), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(half, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(-half, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(0, half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1));
    generateFace(glm::vec3(0, -half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1));

    for (const auto& v : uniqueVertices) {
        vertices.push_back(v.x);
        vertices.push_back(v.y);
        vertices.push_back(v.z);
        colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
        glm::vec3 n = glm::normalize(v);
        normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z);
    }
}

void generateCube(
    const glm::vec3& size, int subX, int subY, int subZ, float cornerRadius,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (subX < 1) subX = 1;
    if (subY < 1) subY = 1;
    if (subZ < 1) subZ = 1;

    glm::vec3 half = size * 0.5f;
    float maxFillet = 0.5f * std::min({size.x, size.y, size.z});
    float cr = std::clamp(cornerRadius, 0.0f, maxFillet);
    glm::vec3 innerHalf = half - glm::vec3(cr);

    struct VecCompare {
        bool operator()(const glm::vec3& a, const glm::vec3& b) const {
            float eps = 1e-4f;
            if (std::abs(a.x - b.x) > eps) return a.x < b.x;
            if (std::abs(a.y - b.y) > eps) return a.y < b.y;
            if (std::abs(a.z - b.z) > eps) return a.z < b.z;
            return false;
        }
    };

    std::map<glm::vec3, uint32_t, VecCompare> vertexMap;
    std::vector<glm::vec3> uniqueVertices;
    std::vector<glm::vec3> accumulatedNormals;

    int startIdx = static_cast<int>(vertices.size() / 3);

    auto getUniqueVertex = [&](const glm::vec3& rawPos, const glm::vec3& faceNormal) -> uint32_t {
        glm::vec3 pos = rawPos;
        glm::vec3 norm = faceNormal;

        if (cr > 1e-4f) {
            glm::vec3 clamped = glm::clamp(pos, -innerHalf, innerHalf);
            glm::vec3 delta = pos - clamped;
            float len = glm::length(delta);
            if (len > 1e-4f) {
                pos = clamped + (delta / len) * cr;
                norm = delta / len;
            }
        }

        auto it = vertexMap.find(pos);
        if (it != vertexMap.end()) {
            accumulatedNormals[it->second] += norm;
            return it->second;
        }
        uint32_t index = static_cast<uint32_t>(uniqueVertices.size());
        uniqueVertices.push_back(pos);
        accumulatedNormals.push_back(norm);
        vertexMap[pos] = index;
        return index;
    };

    auto generatePlaneFace = [&](glm::vec3 origin, glm::vec3 rightDir, float rightLen, int subR,
                                 glm::vec3 upDir, float upLen, int subU, glm::vec3 faceNorm) {
        std::vector<std::vector<uint32_t>> grid(subU + 1, std::vector<uint32_t>(subR + 1));
        for (int y = 0; y <= subU; ++y) {
            float v = (float)y / subU - 0.5f;
            for (int x = 0; x <= subR; ++x) {
                float u = (float)x / subR - 0.5f;
                glm::vec3 pos = origin + rightDir * (u * rightLen) + upDir * (v * upLen);
                grid[y][x] = getUniqueVertex(pos, faceNorm);
            }
        }

        for (int y = 0; y < subU; ++y) {
            for (int x = 0; x < subR; ++x) {
                uint32_t v0 = grid[y][x];
                uint32_t v1 = grid[y][x + 1];
                uint32_t v2 = grid[y + 1][x + 1];
                uint32_t v3 = grid[y + 1][x];

                faces.push_back(startIdx + v0);
                faces.push_back(startIdx + v1);
                faces.push_back(startIdx + v2);
                faces.push_back(startIdx + v3);
            }
        }
    };

    // +Z Front
    generatePlaneFace(glm::vec3(0, 0, half.z), glm::vec3(1, 0, 0), size.x, subX, glm::vec3(0, 1, 0), size.y, subY, glm::vec3(0, 0, 1));
    // -Z Back
    generatePlaneFace(glm::vec3(0, 0, -half.z), glm::vec3(-1, 0, 0), size.x, subX, glm::vec3(0, 1, 0), size.y, subY, glm::vec3(0, 0, -1));
    // +X Right
    generatePlaneFace(glm::vec3(half.x, 0, 0), glm::vec3(0, 0, -1), size.z, subZ, glm::vec3(0, 1, 0), size.y, subY, glm::vec3(1, 0, 0));
    // -X Left
    generatePlaneFace(glm::vec3(-half.x, 0, 0), glm::vec3(0, 0, 1), size.z, subZ, glm::vec3(0, 1, 0), size.y, subY, glm::vec3(-1, 0, 0));
    // +Y Top
    generatePlaneFace(glm::vec3(0, half.y, 0), glm::vec3(1, 0, 0), size.x, subX, glm::vec3(0, 0, -1), size.z, subZ, glm::vec3(0, 1, 0));
    // -Y Bottom
    generatePlaneFace(glm::vec3(0, -half.y, 0), glm::vec3(1, 0, 0), size.x, subX, glm::vec3(0, 0, 1), size.z, subZ, glm::vec3(0, -1, 0));

    for (size_t i = 0; i < uniqueVertices.size(); ++i) {
        const auto& v = uniqueVertices[i];
        vertices.push_back(v.x); vertices.push_back(v.y); vertices.push_back(v.z);
        colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);

        glm::vec3 n = accumulatedNormals[i];
        float len = glm::length(n);
        if (len > 1e-5f) n /= len;
        else n = glm::vec3(0, 1, 0);
        normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z);
    }
}

void generateCylinder(
    float radiusTop, float radiusBottom, float height,
    int radSegments, int heightSegments, bool topCap, bool bottomCap,
    int capRings, float innerRadius,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (radSegments < 3) radSegments = 3;
    if (heightSegments < 1) heightSegments = 1;
    if (capRings <= 0) capRings = std::max(1, heightSegments / 2);

    float heightHalf = height * 0.5f;
    int startIdx = static_cast<int>(vertices.size() / 3);
    bool isHollow = (innerRadius > 1e-3f && innerRadius < std::min(radiusTop, radiusBottom) - 1e-3f);

    // 1. Outer Wall Vertices
    for (int i = 0; i <= heightSegments; ++i) {
        float v = (float)i / heightSegments;
        float radius = (1.0f - v) * radiusTop + v * radiusBottom;
        float y = heightHalf - v * height;
        for (int j = 0; j < radSegments; ++j) {
            float u = (float)M_PI * 2.0f * j / radSegments;
            float x = radius * std::sin(u);
            float z = radius * std::cos(u);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);

            // Compute slant normal
            float dr = radiusBottom - radiusTop;
            float slopeAngle = std::atan2(dr, height);
            float ny = std::sin(slopeAngle);
            float nHoriz = std::cos(slopeAngle);
            float nx = nHoriz * std::sin(u);
            float nz = nHoriz * std::cos(u);

            normals.push_back(nx); normals.push_back(ny); normals.push_back(nz);
        }
    }

    // 2. Outer Wall Faces
    for (int i = 0; i < heightSegments; ++i) {
        for (int j = 0; j < radSegments; ++j) {
            int off = (j == radSegments - 1) ? 0 : j + 1;
            uint32_t v0 = startIdx + radSegments * i + j;
            uint32_t v1 = startIdx + radSegments * (i + 1) + j;
            uint32_t v2 = startIdx + radSegments * (i + 1) + off;
            uint32_t v3 = startIdx + radSegments * i + off;

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }

    // 3. Inner Wall (if hollow)
    int innerStartIdx = -1;
    if (isHollow) {
        innerStartIdx = static_cast<int>(vertices.size() / 3);
        for (int i = 0; i <= heightSegments; ++i) {
            float v = (float)i / heightSegments;
            float y = heightHalf - v * height;
            for (int j = 0; j < radSegments; ++j) {
                float u = (float)M_PI * 2.0f * j / radSegments;
                float x = innerRadius * std::sin(u);
                float z = innerRadius * std::cos(u);

                vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
                colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
                normals.push_back(-std::sin(u)); normals.push_back(0.0f); normals.push_back(-std::cos(u));
            }
        }

        // Inner Wall Faces (reversed winding)
        for (int i = 0; i < heightSegments; ++i) {
            for (int j = 0; j < radSegments; ++j) {
                int off = (j == radSegments - 1) ? 0 : j + 1;
                uint32_t v0 = innerStartIdx + radSegments * i + j;
                uint32_t v1 = innerStartIdx + radSegments * (i + 1) + j;
                uint32_t v2 = innerStartIdx + radSegments * (i + 1) + off;
                uint32_t v3 = innerStartIdx + radSegments * i + off;

                faces.push_back(v0);
                faces.push_back(v3);
                faces.push_back(v2);
                faces.push_back(v1);
            }
        }
    }

    // 4. Top Cap
    if (topCap && radiusTop > 1e-3f) {
        if (isHollow) {
            // Annular quad rings between innerRadius and radiusTop
            int capStartIdx = static_cast<int>(vertices.size() / 3);
            for (int r = 0; r <= capRings; ++r) {
                float t = (float)r / capRings;
                float rCur = (1.0f - t) * innerRadius + t * radiusTop;
                for (int j = 0; j < radSegments; ++j) {
                    float u = (float)M_PI * 2.0f * j / radSegments;
                    vertices.push_back(rCur * std::sin(u));
                    vertices.push_back(heightHalf);
                    vertices.push_back(rCur * std::cos(u));
                    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
                    normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);
                }
            }
            for (int r = 0; r < capRings; ++r) {
                for (int j = 0; j < radSegments; ++j) {
                    int off = (j == radSegments - 1) ? 0 : j + 1;
                    uint32_t v0 = capStartIdx + r * radSegments + j;
                    uint32_t v1 = capStartIdx + (r + 1) * radSegments + j;
                    uint32_t v2 = capStartIdx + (r + 1) * radSegments + off;
                    uint32_t v3 = capStartIdx + r * radSegments + off;
                    faces.push_back(v0); faces.push_back(v1); faces.push_back(v2); faces.push_back(v3);
                }
            }
        } else {
            // Standard Solid Top Cap
            uint32_t topCenterIdx = static_cast<uint32_t>(vertices.size() / 3);
            vertices.push_back(0.0f); vertices.push_back(heightHalf); vertices.push_back(0.0f);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
            normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);

            int capRingStart = static_cast<int>(vertices.size() / 3);
            for (int r = 1; r < capRings; ++r) {
                float ringR = radiusTop * ((float)r / capRings);
                for (int j = 0; j < radSegments; ++j) {
                    float u = (float)M_PI * 2.0f * j / radSegments;
                    vertices.push_back(ringR * std::sin(u));
                    vertices.push_back(heightHalf);
                    vertices.push_back(ringR * std::cos(u));
                    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
                    normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);
                }
            }

            // Center fan
            uint32_t ring1Start = (capRings == 1) ? startIdx : capRingStart;
            for (int j = 0; j < radSegments; ++j) {
                int next = (j == radSegments - 1) ? 0 : j + 1;
                uint32_t vOuter = ring1Start + j;
                uint32_t vNext = ring1Start + next;
                faces.push_back(topCenterIdx);
                faces.push_back(vOuter);
                faces.push_back(vNext);
                faces.push_back(TRI_INDEX);
            }

            // Intermediate concentric quad rings
            for (int r = 1; r < capRings; ++r) {
                uint32_t innerStart = capRingStart + (r - 1) * radSegments;
                uint32_t outerStart = (r + 1 == capRings) ? startIdx : (capRingStart + r * radSegments);
                for (int j = 0; j < radSegments; ++j) {
                    int next = (j == radSegments - 1) ? 0 : j + 1;
                    faces.push_back(innerStart + next);
                    faces.push_back(innerStart + j);
                    faces.push_back(outerStart + j);
                    faces.push_back(outerStart + next);
                }
            }
        }
    }

    // 5. Bottom Cap
    if (bottomCap && radiusBottom > 1e-3f) {
        int outerBottomRingStart = startIdx + heightSegments * radSegments;
        if (isHollow) {
            int capStartIdx = static_cast<int>(vertices.size() / 3);
            for (int r = 0; r <= capRings; ++r) {
                float t = (float)r / capRings;
                float rCur = (1.0f - t) * innerRadius + t * radiusBottom;
                for (int j = 0; j < radSegments; ++j) {
                    float u = (float)M_PI * 2.0f * j / radSegments;
                    vertices.push_back(rCur * std::sin(u));
                    vertices.push_back(-heightHalf);
                    vertices.push_back(rCur * std::cos(u));
                    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
                    normals.push_back(0.0f); normals.push_back(-1.0f); normals.push_back(0.0f);
                }
            }
            for (int r = 0; r < capRings; ++r) {
                for (int j = 0; j < radSegments; ++j) {
                    int off = (j == radSegments - 1) ? 0 : j + 1;
                    uint32_t v0 = capStartIdx + r * radSegments + j;
                    uint32_t v1 = capStartIdx + (r + 1) * radSegments + j;
                    uint32_t v2 = capStartIdx + (r + 1) * radSegments + off;
                    uint32_t v3 = capStartIdx + r * radSegments + off;
                    faces.push_back(v0); faces.push_back(v3); faces.push_back(v2); faces.push_back(v1);
                }
            }
        } else {
            uint32_t botCenterIdx = static_cast<uint32_t>(vertices.size() / 3);
            vertices.push_back(0.0f); vertices.push_back(-heightHalf); vertices.push_back(0.0f);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
            normals.push_back(0.0f); normals.push_back(-1.0f); normals.push_back(0.0f);

            int capRingStart = static_cast<int>(vertices.size() / 3);
            for (int r = 1; r < capRings; ++r) {
                float ringR = radiusBottom * ((float)r / capRings);
                for (int j = 0; j < radSegments; ++j) {
                    float u = (float)M_PI * 2.0f * j / radSegments;
                    vertices.push_back(ringR * std::sin(u));
                    vertices.push_back(-heightHalf);
                    vertices.push_back(ringR * std::cos(u));
                    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
                    normals.push_back(0.0f); normals.push_back(-1.0f); normals.push_back(0.0f);
                }
            }

            // Center fan (facing down)
            uint32_t ring1Start = (capRings == 1) ? outerBottomRingStart : capRingStart;
            for (int j = 0; j < radSegments; ++j) {
                int next = (j == radSegments - 1) ? 0 : j + 1;
                uint32_t vOuter = ring1Start + j;
                uint32_t vNext = ring1Start + next;
                faces.push_back(botCenterIdx);
                faces.push_back(vNext);
                faces.push_back(vOuter);
                faces.push_back(TRI_INDEX);
            }

            // Intermediate concentric quad rings (facing down)
            for (int r = 1; r < capRings; ++r) {
                uint32_t innerStart = capRingStart + (r - 1) * radSegments;
                uint32_t outerStart = (r + 1 == capRings) ? outerBottomRingStart : (capRingStart + r * radSegments);
                for (int j = 0; j < radSegments; ++j) {
                    int next = (j == radSegments - 1) ? 0 : j + 1;
                    faces.push_back(innerStart + j);
                    faces.push_back(innerStart + next);
                    faces.push_back(outerStart + next);
                    faces.push_back(outerStart + j);
                }
            }
        }
    }
}

void generateTorus(
    float radiusOut, float radiusWidth, float arcDegrees,
    int nbRadial, int nbTubular,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (nbRadial < 3) nbRadial = 3;
    if (nbTubular < 3) nbTubular = 3;

    float arc = (float)(arcDegrees * M_PI / 180.0f);
    bool isFull = std::abs(2.0f * (float)M_PI - arc) < 1e-2f;
    int endTubular = isFull ? nbTubular : nbTubular - 1;
    int startIdx = static_cast<int>(vertices.size() / 3);

    for (int i = 0; i < nbTubular; ++i) {
        float u = (float)i / endTubular * arc;
        for (int j = 0; j < nbRadial; ++j) {
            float v = (float)j / nbRadial * 2.0f * (float)M_PI;

            float x = (radiusOut + radiusWidth * std::cos(v)) * std::cos(u);
            float y = radiusWidth * std::sin(v);
            float z = (radiusOut + radiusWidth * std::cos(v)) * std::sin(u);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);

            float cx = radiusOut * std::cos(u);
            float cz = radiusOut * std::sin(u);
            float nx = x - cx;
            float ny = y;
            float nz = z - cz;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-5f) { nx /= len; ny /= len; nz /= len; }
            normals.push_back(nx); normals.push_back(ny); normals.push_back(nz);
        }
    }

    int segCount = isFull ? nbTubular : nbTubular - 1;
    for (int i = 0; i < segCount; ++i) {
        int iNext = (i + 1) % nbTubular;
        for (int j = 0; j < nbRadial; ++j) {
            int jNext = (j + 1) % nbRadial;
            uint32_t v0 = startIdx + i * nbRadial + j;
            uint32_t v1 = startIdx + iNext * nbRadial + j;
            uint32_t v2 = startIdx + iNext * nbRadial + jNext;
            uint32_t v3 = startIdx + i * nbRadial + jNext;

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }

    // End caps for partial arc
    if (!isFull) {
        // Start cap (i = 0)
        uint32_t cap0Center = static_cast<uint32_t>(vertices.size() / 3);
        vertices.push_back(radiusOut); vertices.push_back(0.0f); vertices.push_back(0.0f);
        colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
        normals.push_back(0.0f); normals.push_back(0.0f); normals.push_back(-1.0f);

        for (int j = 0; j < nbRadial; ++j) {
            int jNext = (j + 1) % nbRadial;
            faces.push_back(cap0Center);
            faces.push_back(startIdx + j);
            faces.push_back(startIdx + jNext);
            faces.push_back(TRI_INDEX);
        }

        // End cap (i = nbTubular - 1)
        uint32_t cap1Center = static_cast<uint32_t>(vertices.size() / 3);
        float endU = arc;
        float endCx = radiusOut * std::cos(endU);
        float endCz = radiusOut * std::sin(endU);
        vertices.push_back(endCx); vertices.push_back(0.0f); vertices.push_back(endCz);
        colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
        normals.push_back(-std::sin(endU)); normals.push_back(0.0f); normals.push_back(std::cos(endU));

        int lastRingStart = startIdx + (nbTubular - 1) * nbRadial;
        for (int j = 0; j < nbRadial; ++j) {
            int jNext = (j + 1) % nbRadial;
            faces.push_back(cap1Center);
            faces.push_back(lastRingStart + jNext);
            faces.push_back(lastRingStart + j);
            faces.push_back(TRI_INDEX);
        }
    }
}

void generatePlane(
    float sizeX, float sizeZ, int subX, int subZ,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (subX < 1) subX = 1;
    if (subZ < 1) subZ = 1;

    float halfX = sizeX * 0.5f;
    float halfZ = sizeZ * 0.5f;
    int startIdx = static_cast<int>(vertices.size() / 3);

    for (int z = 0; z <= subZ; ++z) {
        float fz = ((float)z / subZ - 0.5f) * sizeZ;
        for (int x = 0; x <= subX; ++x) {
            float fx = ((float)x / subX - 0.5f) * sizeX;
            vertices.push_back(fx);
            vertices.push_back(0.0f);
            vertices.push_back(fz);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
            normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);
        }
    }

    int rowStride = subX + 1;
    for (int z = 0; z < subZ; ++z) {
        for (int x = 0; x < subX; ++x) {
            uint32_t v0 = startIdx + z * rowStride + x;
            uint32_t v1 = startIdx + (z + 1) * rowStride + x;
            uint32_t v2 = startIdx + (z + 1) * rowStride + (x + 1);
            uint32_t v3 = startIdx + z * rowStride + (x + 1);

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }
}

void generateCapsule(
    float radius, float cylinderHeight,
    int radSegments, int heightSegments, int capRings,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    if (radSegments < 3) radSegments = 3;
    if (heightSegments < 1) heightSegments = 1;
    if (capRings < 2) capRings = 2;

    float halfH = cylinderHeight * 0.5f;
    int startIdx = static_cast<int>(vertices.size() / 3);

    // 1. South Pole
    vertices.push_back(0.0f); vertices.push_back(-halfH - radius); vertices.push_back(0.0f);
    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
    normals.push_back(0.0f); normals.push_back(-1.0f); normals.push_back(0.0f);

    // 2. Bottom Hemisphere Rings (r = 1 to capRings - 1)
    for (int r = 1; r < capRings; ++r) {
        float phi = -M_PI_2 + (M_PI_2 * (float)r / capRings);
        float y = -halfH + radius * std::sin(phi);
        float cos_phi = std::cos(phi);

        for (int s = 0; s < radSegments; ++s) {
            float theta = 2.0f * (float)M_PI * (float)s / radSegments;
            float x = radius * cos_phi * std::sin(theta);
            float z = radius * cos_phi * std::cos(theta);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);

            glm::vec3 n = glm::normalize(glm::vec3(x, y - (-halfH), z));
            normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z);
        }
    }

    // 3. Cylinder Body Rings (i = 0 to heightSegments)
    for (int i = 0; i <= heightSegments; ++i) {
        float v = (float)i / heightSegments;
        float y = -halfH + v * cylinderHeight;
        for (int s = 0; s < radSegments; ++s) {
            float theta = 2.0f * (float)M_PI * (float)s / radSegments;
            float x = radius * std::sin(theta);
            float z = radius * std::cos(theta);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
            normals.push_back(std::sin(theta)); normals.push_back(0.0f); normals.push_back(std::cos(theta));
        }
    }

    // 4. Top Hemisphere Rings (r = 1 to capRings - 1)
    for (int r = 1; r < capRings; ++r) {
        float phi = (M_PI_2 * (float)r / capRings);
        float y = halfH + radius * std::sin(phi);
        float cos_phi = std::cos(phi);

        for (int s = 0; s < radSegments; ++s) {
            float theta = 2.0f * (float)M_PI * (float)s / radSegments;
            float x = radius * cos_phi * std::sin(theta);
            float z = radius * cos_phi * std::cos(theta);

            vertices.push_back(x); vertices.push_back(y); vertices.push_back(z);
            colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);

            glm::vec3 n = glm::normalize(glm::vec3(x, y - halfH, z));
            normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z);
        }
    }

    // 5. North Pole
    uint32_t northPoleIdx = static_cast<uint32_t>(vertices.size() / 3);
    vertices.push_back(0.0f); vertices.push_back(halfH + radius); vertices.push_back(0.0f);
    colors.push_back(1.0f); colors.push_back(1.0f); colors.push_back(1.0f);
    normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);

    // Total rings count (excluding poles):
    // (capRings - 1) bottom + (heightSegments + 1) body + (capRings - 1) top
    int totalRings = (capRings - 1) + (heightSegments + 1) + (capRings - 1);

    // South pole triangle fan
    uint32_t southPoleIdx = static_cast<uint32_t>(startIdx);
    for (int s = 0; s < radSegments; ++s) {
        int sNext = (s + 1) % radSegments;
        faces.push_back(southPoleIdx);
        faces.push_back(startIdx + 1 + s);
        faces.push_back(startIdx + 1 + sNext);
        faces.push_back(TRI_INDEX);
    }

    // Ring-to-ring quads
    for (int r = 0; r < totalRings - 1; ++r) {
        uint32_t ring0 = startIdx + 1 + r * radSegments;
        uint32_t ring1 = startIdx + 1 + (r + 1) * radSegments;
        for (int s = 0; s < radSegments; ++s) {
            int sNext = (s + 1) % radSegments;
            faces.push_back(ring0 + s);
            faces.push_back(ring1 + s);
            faces.push_back(ring1 + sNext);
            faces.push_back(ring0 + sNext);
        }
    }

    // North pole triangle fan
    uint32_t lastRingStart = startIdx + 1 + (totalRings - 1) * radSegments;
    for (int s = 0; s < radSegments; ++s) {
        int sNext = (s + 1) % radSegments;
        faces.push_back(lastRingStart + s);
        faces.push_back(northPoleIdx);
        faces.push_back(lastRingStart + sNext);
        faces.push_back(TRI_INDEX);
    }
}

void generate(
    const PrimitiveData& data,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    vertices.clear();
    faces.clear();
    colors.clear();
    normals.clear();

    switch (data.type) {
        case PrimitiveType::Sphere:
            generateSphere(data.radius, data.heightSegments, data.radialSegments, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Geosphere:
            generateGeosphere(data.radius, data.subdivisionLevel, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Cube:
            generateCube(data.size, data.segmentsX, data.segmentsY, data.segmentsZ, data.cornerRadius, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Cylinder:
            generateCylinder(data.radiusTop, data.radiusBottom, data.height,
                             data.radialSegments, data.heightSegments, data.topCap, data.bottomCap,
                             data.capRings, data.innerRadius, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Cone:
            generateCylinder(data.radiusTop, data.radiusBottom, data.height,
                             data.radialSegments, data.heightSegments, (data.radiusTop > 1e-3f), data.bottomCap,
                             data.capRings, 0.0f, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Torus:
            generateTorus(data.torusMajorRadius, data.torusTubeRadius, data.arcDegrees,
                          data.radialSegments, data.heightSegments, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Plane:
            generatePlane(data.size.x, data.size.z, data.segmentsX, data.segmentsZ, vertices, faces, colors, normals);
            break;
        case PrimitiveType::Capsule:
            generateCapsule(data.radius, data.height, data.radialSegments, data.heightSegments, data.capRings, vertices, faces, colors, normals);
            break;
    }
}

} // namespace PrimitiveGenerator
