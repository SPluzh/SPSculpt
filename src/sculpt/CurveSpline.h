#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cstddef>

struct CurvePoint {
    glm::vec3 position{0.0f};
    float radius = 1.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    bool hasSurfaceNormal = false;
    bool isCorner = false; // Sharp corner / right-angle if true, smooth if false

    CurvePoint() = default;
    CurvePoint(const glm::vec3& pos, float r = 1.0f, const glm::vec3& norm = glm::vec3(0.0f, 1.0f, 0.0f), bool hasNorm = false, bool corner = false)
        : position(pos), radius(r), normal(norm), hasSurfaceNormal(hasNorm), isCorner(corner) {}
};

struct EvaluatedFrame {
    glm::vec3 position{0.0f};
    glm::vec3 tangent{0.0f, 0.0f, 1.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 binormal{1.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    float arcLength = 0.0f;
    float u = 0.0f; // 0.0f at start, 1.0f at end
};

class CurveSpline {
public:
    CurveSpline() = default;
    ~CurveSpline() = default;

    void clear();
    void addPoint(const glm::vec3& pos, float radius = 1.0f, const glm::vec3& normal = glm::vec3(0.0f, 1.0f, 0.0f), bool hasNorm = false, bool isCorner = false);
    void insertPoint(size_t index, const glm::vec3& pos, float radius = 1.0f, const glm::vec3& normal = glm::vec3(0.0f, 1.0f, 0.0f), bool hasNorm = false, bool isCorner = false);
    void removePoint(size_t index);

    size_t getPointCount() const { return m_points.size(); }
    CurvePoint& getPoint(size_t index) { return m_points[index]; }
    const CurvePoint& getPoint(size_t index) const { return m_points[index]; }
    std::vector<CurvePoint>& getPoints() { return m_points; }
    const std::vector<CurvePoint>& getPoints() const { return m_points; }

    // Evaluates smooth frames along the curve with arc-length parameterization and RMF.
    // cornerSegments specifies the cross-section density on smooth bends and sharp corners to prevent pinching/breaking.
    std::vector<EvaluatedFrame> evaluateFrames(
        int numSamples,
        float startRadiusScale = 1.0f,
        float endRadiusScale = 1.0f,
        float twistStartDeg = 0.0f,
        float twistEndDeg = 0.0f,
        int cornerSegments = 8
    ) const;

    // Evaluates Catmull-Rom point at t in segment
    glm::vec3 evaluatePosition(float globalU) const;

    // Evaluates smooth C1 monotonic Hermite radius at tLocal in segment (prevents sharp creases/kinks)
    float evaluateRadius(int segIdx, float tLocal) const;

    float getTotalLength() const;

    // Ramer-Douglas-Peucker simplification for raw freehand stroke points
    static std::vector<CurvePoint> simplifyStroke(const std::vector<CurvePoint>& input, float epsilon);

    // Multi-pass weighted moving average filter to eliminate hand tremors
    static std::vector<CurvePoint> smoothStroke(const std::vector<CurvePoint>& input, int passes = 2);

    // Natural curve simplification: averages stroke to remove jitter and extracts
    // as many control points as needed to accurately describe the curve without excess.
    static std::vector<CurvePoint> simplifyStrokeAdaptive(
        const std::vector<CurvePoint>& input,
        int smoothingPasses = 2,
        float toleranceMultiplier = 1.0f,
        float baseRadius = 2.5f
    );

private:
    std::vector<CurvePoint> m_points;

    // Internal helper for centripetal Catmull-Rom evaluation
    void evaluateSegment(
        int segIdx, float tLocal,
        glm::vec3& outPos, glm::vec3& outTangent, float& outRadius
    ) const;
};
