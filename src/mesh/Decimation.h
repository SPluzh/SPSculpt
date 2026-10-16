#pragma once
#include <vector>
#include <cstdint>
#include <functional>
#include <cfloat>
#include <glm/glm.hpp>

class Mesh;

struct DecimationResult {
    bool success = false;
    int originalVerts = 0;
    int originalFaces = 0;
    int finalVerts = 0;
    int finalFaces = 0;
    float volumeChange = 0.0f; // in percent (-0.5% = lost 0.5% volume)
    float durationMs = 0.0f;
};

struct DecimationOptions {
    // Target polycount
    float targetRatio = 0.5f;     // 0.01 .. 0.99 (0.5 = reduce to 50% faces)
    int   targetFaces = -1;       // Exact target faces (-1 = use targetRatio)

    // Preservation weights
    float volumeWeight = 1.0f;    // Lambda for volume conservation penalty (0 = pure QEM)
    float boundaryWeight = 10.0f; // Penalty for collapsing / moving boundary edges
    float shapeWeight = 1.0f;     // Aspect ratio / triangle quality penalty weight

    // Quality thresholds
    float maxError = FLT_MAX;     // Stop if lowest cost exceeds this threshold
    float maxNormalFlip = 0.2f;   // Min dot product between old and new triangle normal

    // Feature preservation
    bool preservePolyGroups = true; // Do not collapse across PolyGroup borders
    bool preserveBoundary = true;   // Preserve silhouette / open borders
    bool lockSymmetryPlane = true;  // Keep vertices on symmetry plane (X=0) locked to plane

    // Progress callback: percentage [0..100], returns false if cancelled
    std::function<void(int progress)> onProgress = nullptr;
};

// Compact symmetric 4x4 quadric matrix (10 unique entries)
struct Quadric {
    double a2 = 0.0, ab = 0.0, ac = 0.0, ad = 0.0;
    double           b2 = 0.0, bc = 0.0, bd = 0.0;
    double                     c2 = 0.0, cd = 0.0;
    double                               d2 = 0.0;

    Quadric() = default;

    Quadric(double a, double b, double c, double d, double weight = 1.0) {
        a2 = a * a * weight;
        ab = a * b * weight;
        ac = a * c * weight;
        ad = a * d * weight;
        b2 = b * b * weight;
        bc = b * c * weight;
        bd = b * d * weight;
        c2 = c * c * weight;
        cd = c * d * weight;
        d2 = d * d * weight;
    }

    static Quadric fromPlane(const glm::dvec3& n, const glm::dvec3& p, double weight = 1.0) {
        double d = -glm::dot(n, p);
        return Quadric(n.x, n.y, n.z, d, weight);
    }

    Quadric operator+(const Quadric& o) const {
        Quadric r;
        r.a2 = a2 + o.a2; r.ab = ab + o.ab; r.ac = ac + o.ac; r.ad = ad + o.ad;
        r.b2 = b2 + o.b2; r.bc = bc + o.bc; r.bd = bd + o.bd;
        r.c2 = c2 + o.c2; r.cd = cd + o.cd;
        r.d2 = d2 + o.d2;
        return r;
    }

    Quadric& operator+=(const Quadric& o) {
        a2 += o.a2; ab += o.ab; ac += o.ac; ad += o.ad;
        b2 += o.b2; bc += o.bc; bd += o.bd;
        c2 += o.c2; cd += o.cd;
        d2 += o.d2;
        return *this;
    }

    // Evaluate error at position v: v^T * Q * v
    double evaluate(const glm::dvec3& v) const {
        return a2 * v.x * v.x + 2.0 * ab * v.x * v.y + 2.0 * ac * v.x * v.z +
               b2 * v.y * v.y + 2.0 * bc * v.y * v.z +
               c2 * v.z * v.z +
               2.0 * ad * v.x + 2.0 * bd * v.y + 2.0 * cd * v.z +
               d2;
    }

    // Solve 3x3 system: A * v = -b for optimal vertex position
    bool optimalPosition(glm::dvec3& outPos) const;
};

class MeshDecimator {
public:
    MeshDecimator() = default;

    // Main execution: simplifies mesh in-place with volume preservation
    DecimationResult decimate(Mesh& mesh, const DecimationOptions& opts);

    // Static helper for one-line calls
    static DecimationResult simplify(Mesh& mesh, const DecimationOptions& opts) {
        MeshDecimator d;
        return d.decimate(mesh, opts);
    }
};
