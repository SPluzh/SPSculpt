#pragma once

#include "common/Enums.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

class Mesh;

struct SymmetryParams {
    bool useSym = false;
    bool symX = false;
    bool symY = false;
    bool symZ = false;
    bool radialEnabled = false;
    int radialCount = 8;
    int radialAxis = 1; // 0=X, 1=Y, 2=Z
    SymmetryMode mode = SymmetryMode::Local;

    static constexpr int kMaxRadial = 64;

    bool radialOn() const { return radialEnabled && radialCount > 1; }
    bool anyActive() const { return useSym && (symX || symY || symZ || radialOn()); }
};

struct SymCopy {
    glm::mat3 m{1.0f};      // Orthogonal 3x3 (rotation and/or reflection) in mesh local space
    glm::vec3 pivot{0.0f};  // Symmetry center/origin in mesh local space
    glm::vec3 offset{0.0f}; // Explicit affine translation when hasOffset is true
    bool hasOffset = false;

    glm::vec3 point(const glm::vec3& p) const {
        return hasOffset ? (offset + m * p) : (pivot + m * (p - pivot));
    }

    glm::vec3 dir(const glm::vec3& d) const {
        return m * d;
    }

    glm::vec3 inversePoint(const glm::vec3& p) const {
        const glm::mat3 invM = glm::transpose(m);
        return hasOffset ? (invM * (p - offset)) : (pivot + invM * (p - pivot));
    }

    glm::vec3 inverseDir(const glm::vec3& d) const {
        return glm::transpose(m) * d;
    }

    float handedness() const {
        return glm::determinant(m) < 0.0f ? -1.0f : 1.0f;
    }

    bool isMirror() const {
        return glm::determinant(m) < 0.0f;
    }

    glm::vec3 translation() const {
        return hasOffset ? offset : (pivot - m * pivot);
    }

    glm::mat4 toMat4() const {
        glm::mat4 mat(m);
        mat[3] = glm::vec4(translation(), 1.0f);
        return mat;
    }

    SymCopy inverse() const {
        const glm::mat3 invM = glm::transpose(m);
        SymCopy out;
        out.m = invM;
        out.pivot = pivot;
        out.offset = -invM * translation();
        out.hasOffset = true;
        return out;
    }

    bool identity() const {
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) {
                if (std::abs(m[c][r] - (c == r ? 1.0f : 0.0f)) > 1e-6f) {
                    return false;
                }
            }
        }
        const glm::vec3 t = translation();
        return glm::dot(t, t) <= 1e-10f;
    }
};

std::vector<SymCopy> buildSymmetryCopies(const SymmetryParams& params, const Mesh* mesh, bool includeIdentity = false);
std::vector<size_t> symmetryInverses(std::span<const SymCopy> copies);

