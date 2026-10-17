#include "sculpt/CurveSpline.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

void CurveSpline::clear() {
    m_points.clear();
}

void CurveSpline::addPoint(const glm::vec3& pos, float radius, const glm::vec3& normal, bool hasNorm, bool isCorner) {
    m_points.emplace_back(pos, radius, normal, hasNorm, isCorner);
}

void CurveSpline::insertPoint(size_t index, const glm::vec3& pos, float radius, const glm::vec3& normal, bool hasNorm, bool isCorner) {
    if (index >= m_points.size()) {
        m_points.emplace_back(pos, radius, normal, hasNorm, isCorner);
    } else {
        m_points.insert(m_points.begin() + index, CurvePoint(pos, radius, normal, hasNorm, isCorner));
    }
}

void CurveSpline::removePoint(size_t index) {
    if (index < m_points.size()) {
        m_points.erase(m_points.begin() + index);
    }
}

static float getKnotInterval(const glm::vec3& p0, const glm::vec3& p1, float alpha = 0.5f) {
    float d2 = glm::dot(p1 - p0, p1 - p0);
    if (d2 <= 1e-10f) return 1e-5f;
    return std::pow(d2, alpha * 0.5f);
}

// Centripetal Catmull-Rom evaluation using Barry-Goldman pyramid
static glm::vec3 evalCatmullRomBarryGoldman(
    const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
    float t0, float t1, float t2, float t3,
    float t
) {
    float inv01 = 1.0f / (t1 - t0);
    float inv12 = 1.0f / (t2 - t1);
    float inv23 = 1.0f / (t3 - t2);

    glm::vec3 a1 = (t1 - t) * inv01 * p0 + (t - t0) * inv01 * p1;
    glm::vec3 a2 = (t2 - t) * inv12 * p1 + (t - t1) * inv12 * p2;
    glm::vec3 a3 = (t3 - t) * inv23 * p2 + (t - t2) * inv23 * p3;

    float inv02 = 1.0f / (t2 - t0);
    float inv13 = 1.0f / (t3 - t1);

    glm::vec3 b1 = (t2 - t) * inv02 * a1 + (t - t0) * inv02 * a2;
    glm::vec3 b2 = (t3 - t) * inv13 * a2 + (t - t1) * inv13 * a3;

    return (t2 - t) * inv12 * b1 + (t - t1) * inv12 * b2;
}

void CurveSpline::evaluateSegment(
    int segIdx, float tLocal,
    glm::vec3& outPos, glm::vec3& outTangent, float& outRadius
) const {
    int n = static_cast<int>(m_points.size());
    if (n < 2) {
        if (n == 1) {
            outPos = m_points[0].position;
            outTangent = glm::vec3(0.0f, 0.0f, 1.0f);
            outRadius = m_points[0].radius;
        }
        return;
    }

    tLocal = std::clamp(tLocal, 0.0f, 1.0f);

    int i0 = segIdx - 1;
    int i1 = segIdx;
    int i2 = segIdx + 1;
    int i3 = segIdx + 2;

    glm::vec3 p1 = m_points[i1].position;
    glm::vec3 p2 = m_points[i2].position;
    glm::vec3 p0 = (i0 >= 0 && !m_points[i1].isCorner) ? m_points[i0].position : (p1 - (p2 - p1));
    glm::vec3 p3 = (i3 < n && !m_points[i2].isCorner) ? m_points[i3].position : (p2 + (p2 - p1));

    float dt0 = getKnotInterval(p0, p1);
    float dt1 = getKnotInterval(p1, p2);
    float dt2 = getKnotInterval(p2, p3);

    float t0 = 0.0f;
    float t1 = t0 + dt0;
    float t2 = t1 + dt1;
    float t3 = t2 + dt2;

    float tEval = t1 + tLocal * dt1;

    outPos = evalCatmullRomBarryGoldman(p0, p1, p2, p3, t0, t1, t2, t3, tEval);

    // Tangent via central finite difference
    const float eps = 1e-4f * dt1;
    float tA = std::max(t1, tEval - eps);
    float tB = std::min(t2, tEval + eps);
    if (tB > tA) {
        glm::vec3 posA = evalCatmullRomBarryGoldman(p0, p1, p2, p3, t0, t1, t2, t3, tA);
        glm::vec3 posB = evalCatmullRomBarryGoldman(p0, p1, p2, p3, t0, t1, t2, t3, tB);
        glm::vec3 diff = posB - posA;
        float len = glm::length(diff);
        if (len > 1e-7f) {
            outTangent = diff / len;
        } else {
            glm::vec3 chord = p2 - p1;
            float chordLen = glm::length(chord);
            outTangent = (chordLen > 1e-6f) ? (chord / chordLen) : glm::vec3(0.0f, 0.0f, 1.0f);
        }
    } else {
        glm::vec3 chord = p2 - p1;
        float chordLen = glm::length(chord);
        outTangent = (chordLen > 1e-6f) ? (chord / chordLen) : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    // Smooth C1 monotonic Hermite radius interpolation (PCHIP)
    outRadius = evaluateRadius(segIdx, tLocal);
}

float CurveSpline::evaluateRadius(int segIdx, float tLocal) const {
    int n = static_cast<int>(m_points.size());
    if (n == 0) return 1.0f;
    if (n == 1) return m_points[0].radius;

    segIdx = std::clamp(segIdx, 0, n - 2);
    tLocal = std::clamp(tLocal, 0.0f, 1.0f);

    int i0 = segIdx - 1;
    int i1 = segIdx;
    int i2 = segIdx + 1;
    int i3 = segIdx + 2;

    float r1 = m_points[i1].radius;
    float r2 = m_points[i2].radius;
    float r0 = (i0 >= 0 && !m_points[i1].isCorner) ? m_points[i0].radius : r1;
    float r3 = (i3 < n && !m_points[i2].isCorner) ? m_points[i3].radius : r2;

    float L0 = (i0 >= 0) ? std::max(1e-4f, glm::distance(m_points[i0].position, m_points[i1].position)) : 1.0f;
    float L1 = std::max(1e-4f, glm::distance(m_points[i1].position, m_points[i2].position));
    float L2 = (i3 < n) ? std::max(1e-4f, glm::distance(m_points[i2].position, m_points[i3].position)) : 1.0f;

    // Slopes between control points
    float s0 = (r1 - r0) / L0;
    float s1 = (r2 - r1) / L1;
    float s2 = (r3 - r2) / L2;

    // PCHIP derivatives at i1 and i2:
    // If local extremum (e.g. radius increased at point), derivative is exactly 0.0f to round off peak smoothly!
    auto calcPchipD = [](float sIn, float sOut) -> float {
        if (sIn * sOut <= 0.0f) {
            return 0.0f;
        }
        return (2.0f * sIn * sOut) / (sIn + sOut);
    };

    float d1 = (i0 >= 0 && !m_points[i1].isCorner) ? calcPchipD(s0, s1) : 0.0f;
    float d2 = (i3 < n && !m_points[i2].isCorner) ? calcPchipD(s1, s2) : 0.0f;

    // Cubic Hermite evaluation with continuous C1 derivative
    float t = tLocal;
    float t2 = t * t;
    float t3 = t2 * t;

    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;

    float r = h00 * r1 + h10 * L1 * d1 + h01 * r2 + h11 * L1 * d2;
    return std::max(0.01f, r);
}

glm::vec3 CurveSpline::evaluatePosition(float globalU) const {
    if (m_points.empty()) return glm::vec3(0.0f);
    if (m_points.size() == 1) return m_points[0].position;

    globalU = std::clamp(globalU, 0.0f, 1.0f);
    int numSegments = static_cast<int>(m_points.size()) - 1;
    float segU = globalU * static_cast<float>(numSegments);
    int segIdx = static_cast<int>(segU);
    if (segIdx >= numSegments) {
        segIdx = numSegments - 1;
    }
    float tLocal = segU - static_cast<float>(segIdx);

    glm::vec3 pos, tangent;
    float radius;
    evaluateSegment(segIdx, tLocal, pos, tangent, radius);
    return pos;
}

float CurveSpline::getTotalLength() const {
    if (m_points.size() < 2) return 0.0f;
    int numSegments = static_cast<int>(m_points.size()) - 1;
    const int samplesPerSeg = 20;
    float totalLen = 0.0f;
    glm::vec3 lastPos = m_points[0].position;

    for (int seg = 0; seg < numSegments; ++seg) {
        for (int i = 1; i <= samplesPerSeg; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(samplesPerSeg);
            glm::vec3 pos, tan;
            float r;
            evaluateSegment(seg, t, pos, tan, r);
            totalLen += glm::distance(lastPos, pos);
            lastPos = pos;
        }
    }
    return totalLen;
}

struct CornerElbow {
    bool active = false;
    glm::vec3 P{0.0f};
    glm::vec3 Tin{0.0f};
    glm::vec3 Tout{0.0f};
    float theta = 0.0f;
    float rFillet = 1.0f;
    float d = 0.0f;
    glm::vec3 A{0.0f};
    glm::vec3 B{0.0f};
    glm::vec3 C{0.0f};
    glm::vec3 U{0.0f};
    glm::vec3 V{0.0f};
    float sStart = 0.0f;
    float sEnd = 0.0f;
    float radius = 1.0f;
};

float CurveSpline::evaluateRadiusProfile(float u, const std::vector<ProfilePoint2D>& points, bool smooth) {
    if (points.empty()) return 1.0f;
    if (points.size() == 1) return points[0].pos.y;
    u = std::clamp(u, 0.0f, 1.0f);
    if (u <= points.front().pos.x) return points.front().pos.y;
    if (u >= points.back().pos.x) return points.back().pos.y;

    int seg = 0;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (u >= points[i].pos.x && u <= points[i + 1].pos.x) {
            seg = static_cast<int>(i);
            break;
        }
    }

    float x0 = points[seg].pos.x;
    float x1 = points[seg + 1].pos.x;
    float dx = x1 - x0;
    if (dx < 1e-6f) return points[seg].pos.y;

    float t = (u - x0) / dx;
    float y0 = points[seg].pos.y;
    float y1 = points[seg + 1].pos.y;

    bool p0Corner = points[seg].isCorner;
    bool p1Corner = points[seg + 1].isCorner;

    if (!smooth || (p0Corner && p1Corner) || points.size() < 3) {
        return glm::mix(y0, y1, t);
    }

    // Monotonic cubic Hermite interpolation
    float delta = (y1 - y0) / dx;
    float m0 = delta;
    float m1 = delta;

    if (!p0Corner && seg > 0) {
        float dxPrev = points[seg].pos.x - points[seg - 1].pos.x;
        if (dxPrev > 1e-6f) {
            float deltaPrev = (points[seg].pos.y - points[seg - 1].pos.y) / dxPrev;
            m0 = 0.5f * (deltaPrev + delta);
        }
    }
    if (!p1Corner && seg + 2 < static_cast<int>(points.size())) {
        float dxNext = points[seg + 2].pos.x - points[seg + 1].pos.x;
        if (dxNext > 1e-6f) {
            float deltaNext = (points[seg + 2].pos.y - points[seg + 1].pos.y) / dxNext;
            m1 = 0.5f * (delta + deltaNext);
        }
    }

    if (std::abs(delta) < 1e-6f) {
        m0 = 0.0f;
        m1 = 0.0f;
    } else {
        float alpha = m0 / delta;
        float beta = m1 / delta;
        if (alpha < 0.0f) m0 = 0.0f;
        if (beta < 0.0f) m1 = 0.0f;
        float dist = alpha * alpha + beta * beta;
        if (dist > 9.0f) {
            float tau = 3.0f / std::sqrt(dist);
            m0 = tau * alpha * delta;
            m1 = tau * beta * delta;
        }
    }

    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;

    float val = h00 * y0 + h10 * (m0 * dx) + h01 * y1 + h11 * (m1 * dx);
    return std::max(0.0f, val);
}

std::vector<EvaluatedFrame> CurveSpline::evaluateFrames(
    int numSamples,
    float startRadiusScale,
    float endRadiusScale,
    float twistStartDeg,
    float twistEndDeg,
    int cornerSegments,
    bool useRadiusProfile,
    const std::vector<ProfilePoint2D>& radiusProfile,
    bool radiusProfileSmooth
) const {
    std::vector<EvaluatedFrame> frames;
    if (m_points.size() < 2 || numSamples < 2) return frames;

    int numPoints = static_cast<int>(m_points.size());
    int numSegments = numPoints - 1;

    // 1. Precalculate corner elbows for all sharp corner control points
    std::vector<CornerElbow> elbows(numPoints);
    for (int k = 1; k < numPoints - 1; ++k) {
        if (!m_points[k].isCorner) continue;

        glm::vec3 Vin = m_points[k].position - m_points[k - 1].position;
        glm::vec3 Vout = m_points[k + 1].position - m_points[k].position;
        float Lin = glm::length(Vin);
        float Lout = glm::length(Vout);
        if (Lin < 1e-4f || Lout < 1e-4f) continue;

        glm::vec3 Tin = Vin / Lin;
        glm::vec3 Tout = Vout / Lout;
        float dotT = glm::clamp(glm::dot(Tin, Tout), -1.0f, 1.0f);
        float theta = std::acos(dotT);
        if (theta < 0.05f) continue; // nearly straight

        glm::vec3 Nplane = glm::cross(Tin, Tout);
        float nLen = glm::length(Nplane);
        glm::vec3 nPlaneUnit;
        if (nLen > 1e-5f) {
            nPlaneUnit = Nplane / nLen;
        } else {
            glm::vec3 up = (std::abs(Tin.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            nPlaneUnit = glm::normalize(glm::cross(Tin, up));
        }

        glm::vec3 V = glm::normalize(glm::cross(nPlaneUnit, Tin));
        float tanHalf = std::tan(theta * 0.5f);
        if (tanHalf < 1e-4f) continue;

        float r = m_points[k].radius;
        // Make fillet radius at least 1.25x the tube radius to guarantee positive inner wall curvature (no pinch)
        float targetR = std::max(r * 1.25f, 0.5f);
        float targetD = targetR * tanHalf;
        float maxD = std::min(Lin, Lout) * 0.45f;
        float d = std::min(targetD, maxD);
        float rFillet = d / tanHalf;

        elbows[k].active = true;
        elbows[k].P = m_points[k].position;
        elbows[k].Tin = Tin;
        elbows[k].Tout = Tout;
        elbows[k].theta = theta;
        elbows[k].rFillet = rFillet;
        elbows[k].d = d;
        elbows[k].A = elbows[k].P - Tin * d;
        elbows[k].B = elbows[k].P + Tout * d;
        elbows[k].C = elbows[k].A + V * rFillet;
        elbows[k].U = Tin;
        elbows[k].V = V;
        elbows[k].radius = r;
    }

    // 2. Dense sampling to build arc-length table with dense corner cross-sections
    struct DenseSample {
        float globalU;
        int segIdx;
        float tLocal;
        float arcLength;
        glm::vec3 pos;
        glm::vec3 tangent;
        float radius;
    };

    std::vector<DenseSample> denseSamples;
    int densePerSeg = std::max(64, (numSamples * 3) / std::max(1, numSegments));
    denseSamples.reserve(numSegments * densePerSeg * 2);

    float currentLength = 0.0f;
    glm::vec3 prevPos = m_points[0].position;

    for (int seg = 0; seg < numSegments; ++seg) {
        bool startIsCorner = elbows[seg].active;
        bool endIsCorner = elbows[seg + 1].active;

        glm::vec3 legStart = startIsCorner ? elbows[seg].B : m_points[seg].position;
        glm::vec3 legEnd = endIsCorner ? elbows[seg + 1].A : m_points[seg + 1].position;
        float rStart = m_points[seg].radius;
        float rEnd = m_points[seg + 1].radius;

        int legSteps = (startIsCorner || endIsCorner) ? std::max(16, densePerSeg / 2) : densePerSeg;

        for (int i = 0; i <= legSteps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(legSteps);
            glm::vec3 pos, tan;
            float r;

            if (startIsCorner || endIsCorner) {
                // Straight leg between elbows or into elbow
                pos = glm::mix(legStart, legEnd, t);
                glm::vec3 dLeg = legEnd - legStart;
                float dLen = glm::length(dLeg);
                tan = (dLen > 1e-5f) ? (dLeg / dLen) : (startIsCorner ? elbows[seg].Tout : elbows[seg + 1].Tin);
                r = evaluateRadius(seg, t);
            } else {
                evaluateSegment(seg, t, pos, tan, r);
            }

            if (seg == 0 && i == 0) {
                currentLength = 0.0f;
            } else {
                currentLength += glm::distance(prevPos, pos);
            }
            prevPos = pos;

            float globalU = (static_cast<float>(seg) + t) / static_cast<float>(numSegments);
            denseSamples.push_back({globalU, seg, t, currentLength, pos, tan, r});
        }

        // If the end of this segment is a corner elbow, sample the circular fillet arc!
        if (endIsCorner) {
            elbows[seg + 1].sStart = currentLength;
            int arcSteps = std::max(24, cornerSegments * 4);
            for (int j = 1; j <= arcSteps; ++j) {
                float alpha = static_cast<float>(j) / static_cast<float>(arcSteps);
                float phi = alpha * elbows[seg + 1].theta;

                glm::vec3 pos = elbows[seg + 1].C - elbows[seg + 1].rFillet * elbows[seg + 1].V * std::cos(phi)
                                                 + elbows[seg + 1].rFillet * elbows[seg + 1].U * std::sin(phi);
                glm::vec3 tan = glm::normalize(elbows[seg + 1].U * std::cos(phi) + elbows[seg + 1].V * std::sin(phi));
                float r = elbows[seg + 1].radius;

                currentLength += glm::distance(prevPos, pos);
                prevPos = pos;

                float globalU = static_cast<float>(seg + 1) / static_cast<float>(numSegments);
                denseSamples.push_back({globalU, seg + 1, alpha, currentLength, pos, tan, r});
            }
            elbows[seg + 1].sEnd = currentLength;
        }
    }

    float totalLength = currentLength;
    if (totalLength <= 1e-6f) {
        totalLength = 1e-6f;
    }

    // Smooth radius transitions around control points to eliminate creases / folds
    if (denseSamples.size() > 2) {
        std::vector<float> smoothedRadii(denseSamples.size());
        for (size_t i = 0; i < denseSamples.size(); ++i) {
            float sCenter = denseSamples[i].arcLength;
            float rCenter = denseSamples[i].radius;
            float sigma = std::max(0.15f, rCenter * 0.4f);
            float twoSigma2 = 2.0f * sigma * sigma;
            float maxDist = sigma * 2.5f;

            float weightSum = 0.0f;
            float rSum = 0.0f;

            int startJ = static_cast<int>(i);
            while (startJ > 0 && (sCenter - denseSamples[startJ - 1].arcLength) <= maxDist) {
                --startJ;
            }
            int endJ = static_cast<int>(i);
            while (endJ + 1 < static_cast<int>(denseSamples.size()) && (denseSamples[endJ + 1].arcLength - sCenter) <= maxDist) {
                ++endJ;
            }

            for (int j = startJ; j <= endJ; ++j) {
                float dist = std::abs(denseSamples[j].arcLength - sCenter);
                float w = std::exp(-(dist * dist) / twoSigma2);
                rSum += denseSamples[j].radius * w;
                weightSum += w;
            }

            smoothedRadii[i] = (weightSum > 1e-5f) ? (rSum / weightSum) : rCenter;
        }

        for (size_t i = 0; i < denseSamples.size(); ++i) {
            denseSamples[i].radius = glm::mix(denseSamples[i].radius, smoothedRadii[i], 0.45f);
        }
    }

    // 3. Build target arc-length points with curvature-adaptive sampling for BOTH smooth bends & sharp corners!
    std::vector<float> cumWeight(denseSamples.size(), 0.0f);
    float dsBase = totalLength / static_cast<float>(std::max(1, numSamples - 1));
    float angleLimit = (float)M_PI * 0.5f / static_cast<float>(std::max(3, cornerSegments));

    for (size_t i = 1; i < denseSamples.size(); ++i) {
        float ds = denseSamples[i].arcLength - denseSamples[i - 1].arcLength;
        float dotT = glm::clamp(glm::dot(denseSamples[i - 1].tangent, denseSamples[i].tangent), -1.0f, 1.0f);
        float dTheta = std::acos(dotT);
        float rAvg = (denseSamples[i - 1].radius + denseSamples[i].radius) * 0.5f;

        // Base uniform step contribution for straight sections
        float wLen = (dsBase > 1e-5f) ? (ds / dsBase) : 0.0f;

        // Curvature / bend angle contribution:
        // Densifies rings on smooth bends and sharp corners so angle per quad <= angleLimit.
        // Thick tubes bending need denser rings so inner wall doesn't crease or pinch.
        float radiusFactor = 1.0f + (rAvg / std::max(dsBase, 0.1f)) * 0.5f;
        float wCurv = (dTheta / angleLimit) * radiusFactor;

        cumWeight[i] = cumWeight[i - 1] + wLen + wCurv;
    }

    float totalWeight = cumWeight.back();
    int totalSegments = std::clamp(
        std::max(numSamples - 1, static_cast<int>(std::ceil(totalWeight))),
        numSamples - 1,
        2048
    );

    std::vector<float> sTargets;
    sTargets.reserve(totalSegments + 1 + elbows.size() * 2);

    for (int k = 0; k <= totalSegments; ++k) {
        float targetW = (static_cast<float>(k) / static_cast<float>(totalSegments)) * totalWeight;

        auto it = std::lower_bound(cumWeight.begin(), cumWeight.end(), targetW);
        if (it == cumWeight.begin()) {
            sTargets.push_back(denseSamples.front().arcLength);
        } else if (it == cumWeight.end()) {
            sTargets.push_back(denseSamples.back().arcLength);
        } else {
            size_t idx = std::distance(cumWeight.begin(), it);
            float w0 = cumWeight[idx - 1];
            float w1 = cumWeight[idx];
            float dw = w1 - w0;
            float alpha = (dw > 1e-6f) ? ((targetW - w0) / dw) : 0.0f;
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            float s = glm::mix(denseSamples[idx - 1].arcLength, denseSamples[idx].arcLength, alpha);
            sTargets.push_back(s);
        }
    }

    // Preserve exact tangency boundaries for any sharp corner elbows
    for (size_t p = 1; p + 1 < m_points.size(); ++p) {
        if (elbows[p].active) {
            sTargets.push_back(elbows[p].sStart);
            sTargets.push_back(elbows[p].sEnd);
        }
    }

    std::sort(sTargets.begin(), sTargets.end());
    sTargets.erase(std::unique(sTargets.begin(), sTargets.end(), [](float a, float b) {
        return std::abs(a - b) < 1e-4f;
    }), sTargets.end());

    frames.reserve(sTargets.size());

    for (size_t k = 0; k < sTargets.size(); ++k) {
        float sTarget = sTargets[k];
        float uTarget = std::clamp(sTarget / totalLength, 0.0f, 1.0f);

        auto it = std::lower_bound(
            denseSamples.begin(), denseSamples.end(), sTarget,
            [](const DenseSample& sample, float s) {
                return sample.arcLength < s;
            }
        );

        EvaluatedFrame frame;
        frame.u = uTarget;
        frame.arcLength = sTarget;

        if (it == denseSamples.begin()) {
            frame.position = it->pos;
            frame.tangent = it->tangent;
            frame.radius = it->radius;
        } else if (it == denseSamples.end()) {
            const auto& last = denseSamples.back();
            frame.position = last.pos;
            frame.tangent = last.tangent;
            frame.radius = last.radius;
        } else {
            const auto& sB = *it;
            const auto& sA = *(it - 1);
            float ds = sB.arcLength - sA.arcLength;

            if (ds <= 1e-6f) {
                frame.position = sA.pos;
                frame.tangent = sA.tangent;
                frame.radius = sA.radius;
            } else {
                float alpha = (sTarget - sA.arcLength) / ds;
                alpha = std::clamp(alpha, 0.0f, 1.0f);

                frame.position = glm::mix(sA.pos, sB.pos, alpha);
                frame.tangent = glm::normalize(glm::mix(sA.tangent, sB.tangent, alpha));
                frame.radius = glm::mix(sA.radius, sB.radius, alpha);
            }
        }

        // Apply tapering (startRadiusScale -> endRadiusScale) and optional longitudinal radius profile graph
        float profileScale = 1.0f;
        if (useRadiusProfile && !radiusProfile.empty()) {
            profileScale = evaluateRadiusProfile(uTarget, radiusProfile, radiusProfileSmooth);
        }
        float taperScale = glm::mix(startRadiusScale, endRadiusScale, uTarget) * profileScale;
        frame.radius *= taperScale;

        frames.push_back(frame);
    }

    // 3. Compute Rotation Minimizing Frames (RMF) via Double Reflection method
    if (frames.empty()) return frames;

    // Initial normal for frame 0
    glm::vec3 T0 = frames[0].tangent;
    glm::vec3 N0;

    if (m_points[0].hasSurfaceNormal && glm::length(m_points[0].normal) > 0.5f) {
        glm::vec3 surfNorm = glm::normalize(m_points[0].normal);
        // Project onto plane orthogonal to T0
        N0 = surfNorm - T0 * glm::dot(T0, surfNorm);
        if (glm::length(N0) > 1e-4f) {
            N0 = glm::normalize(N0);
        } else {
            glm::vec3 up = (std::abs(T0.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            N0 = glm::normalize(glm::cross(T0, up));
        }
    } else {
        glm::vec3 up = (std::abs(T0.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        N0 = glm::normalize(glm::cross(T0, up));
    }

    glm::vec3 B0 = glm::normalize(glm::cross(T0, N0));
    N0 = glm::normalize(glm::cross(B0, T0));

    frames[0].normal = N0;
    frames[0].binormal = B0;

    // Double reflection step along sequence
    for (size_t i = 0; i + 1 < frames.size(); ++i) {
        const glm::vec3& x_i = frames[i].position;
        const glm::vec3& x_next = frames[i + 1].position;
        const glm::vec3& t_i = frames[i].tangent;
        const glm::vec3& t_next = frames[i + 1].tangent;
        const glm::vec3& r_i = frames[i].normal;

        glm::vec3 v1 = x_next - x_i;
        float c1 = glm::dot(v1, v1);
        glm::vec3 r_i_L = r_i;
        glm::vec3 t_i_L = t_i;

        if (c1 > 1e-10f) {
            r_i_L = r_i - (2.0f / c1) * glm::dot(v1, r_i) * v1;
            t_i_L = t_i - (2.0f / c1) * glm::dot(v1, t_i) * v1;
        }

        glm::vec3 v2 = t_next - t_i_L;
        float c2 = glm::dot(v2, v2);
        glm::vec3 r_next = r_i_L;

        if (c2 > 1e-10f) {
            r_next = r_i_L - (2.0f / c2) * glm::dot(v2, r_i_L) * v2;
        }

        glm::vec3 b_next = glm::cross(t_next, r_next);
        float bLen = glm::length(b_next);
        if (bLen > 1e-6f) {
            b_next /= bLen;
            r_next = glm::normalize(glm::cross(b_next, t_next));
        } else {
            b_next = frames[i].binormal;
            r_next = frames[i].normal;
        }

        frames[i + 1].normal = r_next;
        frames[i + 1].binormal = b_next;
    }

    // 4. Apply optional user twist (rotation around tangent)
    float twistStartRad = glm::radians(twistStartDeg);
    float twistEndRad = glm::radians(twistEndDeg);

    for (size_t i = 0; i < frames.size(); ++i) {
        float theta = glm::mix(twistStartRad, twistEndRad, frames[i].u);
        if (std::abs(theta) > 1e-5f) {
            float cosTheta = std::cos(theta);
            float sinTheta = std::sin(theta);
            glm::vec3 N = frames[i].normal;
            glm::vec3 B = frames[i].binormal;
            frames[i].normal = glm::normalize(cosTheta * N + sinTheta * B);
            frames[i].binormal = glm::normalize(glm::cross(frames[i].tangent, frames[i].normal));
        }
    }

    return frames;
}

// Distance from point P to line segment AB
static float perpendicularDistance(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-8f) {
        return glm::distance(p, a);
    }
    float t = glm::dot(p - a, ab) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    glm::vec3 proj = a + t * ab;
    return glm::distance(p, proj);
}

static void rdpRecursive(
    const std::vector<CurvePoint>& points,
    int first, int last, float epsilon,
    std::vector<bool>& keep
) {
    if (last <= first + 1) return;

    float maxDist = 0.0f;
    int index = -1;

    for (int i = first + 1; i < last; ++i) {
        float d = perpendicularDistance(points[i].position, points[first].position, points[last].position);
        if (d > maxDist) {
            maxDist = d;
            index = i;
        }
    }

    if (maxDist > epsilon && index != -1) {
        keep[index] = true;
        rdpRecursive(points, first, index, epsilon, keep);
        rdpRecursive(points, index, last, epsilon, keep);
    }
}

std::vector<CurvePoint> CurveSpline::simplifyStroke(const std::vector<CurvePoint>& input, float epsilon) {
    if (input.size() <= 2) return input;

    std::vector<bool> keep(input.size(), false);
    keep[0] = true;
    keep[input.size() - 1] = true;

    rdpRecursive(input, 0, static_cast<int>(input.size()) - 1, epsilon, keep);

    std::vector<CurvePoint> result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (keep[i]) {
            result.push_back(input[i]);
        }
    }

    // Safety check: ensure at least 2 points
    if (result.size() < 2) {
        result.clear();
        result.push_back(input.front());
        result.push_back(input.back());
    }

    return result;
}

std::vector<CurvePoint> CurveSpline::smoothStroke(const std::vector<CurvePoint>& input, int passes) {
    if (input.size() <= 2 || passes <= 0) return input;

    std::vector<CurvePoint> current = input;
    std::vector<CurvePoint> next = current;

    for (int p = 0; p < passes; ++p) {
        // Keep first and last points fixed
        for (size_t i = 1; i + 1 < current.size(); ++i) {
            // Weighted moving average: 0.25 * prev + 0.50 * curr + 0.25 * next
            next[i].position = 0.25f * current[i - 1].position +
                               0.50f * current[i].position +
                               0.25f * current[i + 1].position;
            next[i].radius = 0.25f * current[i - 1].radius +
                             0.50f * current[i].radius +
                             0.25f * current[i + 1].radius;

            glm::vec3 avgNorm = 0.25f * current[i - 1].normal +
                                0.50f * current[i].normal +
                                0.25f * current[i + 1].normal;
            float nLen = glm::length(avgNorm);
            if (nLen > 1e-4f) {
                next[i].normal = avgNorm / nLen;
            }
        }
        current = next;
    }
    return current;
}

// Resamples a stroke polyline into N equidistant samples along its arc-length
static std::vector<CurvePoint> resampleEquidistant(const std::vector<CurvePoint>& stroke, int targetCount) {
    if (stroke.size() <= 2 || targetCount <= 2) return stroke;

    std::vector<float> cumLengths;
    cumLengths.reserve(stroke.size());
    cumLengths.push_back(0.0f);
    float totalLen = 0.0f;
    for (size_t i = 1; i < stroke.size(); ++i) {
        totalLen += glm::distance(stroke[i - 1].position, stroke[i].position);
        cumLengths.push_back(totalLen);
    }

    if (totalLen < 1e-5f) return stroke;

    std::vector<CurvePoint> resampled;
    resampled.reserve(targetCount);
    resampled.push_back(stroke.front());

    size_t curSeg = 0;
    for (int k = 1; k < targetCount - 1; ++k) {
        float targetDist = (static_cast<float>(k) / static_cast<float>(targetCount - 1)) * totalLen;
        while (curSeg + 1 < cumLengths.size() && cumLengths[curSeg + 1] < targetDist) {
            curSeg++;
        }
        if (curSeg + 1 >= stroke.size()) {
            break;
        }
        float segLen = cumLengths[curSeg + 1] - cumLengths[curSeg];
        float alpha = (segLen > 1e-6f) ? (targetDist - cumLengths[curSeg]) / segLen : 0.0f;
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        CurvePoint pt;
        pt.position = glm::mix(stroke[curSeg].position, stroke[curSeg + 1].position, alpha);
        pt.radius = glm::mix(stroke[curSeg].radius, stroke[curSeg + 1].radius, alpha);
        glm::vec3 n = glm::mix(stroke[curSeg].normal, stroke[curSeg + 1].normal, alpha);
        float nLen = glm::length(n);
        pt.normal = (nLen > 1e-4f) ? (n / nLen) : glm::vec3(0.0f, 1.0f, 0.0f);
        pt.hasSurfaceNormal = stroke[curSeg].hasSurfaceNormal || stroke[curSeg + 1].hasSurfaceNormal;
        resampled.push_back(pt);
    }

    resampled.push_back(stroke.back());
    return resampled;
}

std::vector<CurvePoint> CurveSpline::simplifyStrokeAdaptive(
    const std::vector<CurvePoint>& input,
    int smoothingPasses,
    float toleranceMultiplier,
    float baseRadius
) {
    if (input.size() <= 2) return input;

    // 1. Average / smooth out hand tremors and micro-jitter
    std::vector<CurvePoint> smoothed = (smoothingPasses > 0)
        ? smoothStroke(input, smoothingPasses)
        : input;

    if (smoothed.size() <= 2) return smoothed;

    // 2. Adaptive Ramer-Douglas-Peucker epsilon:
    // Scale with tube baseRadius so features smaller than ~35% of tube thickness are ignored
    float baseEps = std::clamp(baseRadius * 0.35f, 0.4f, 2.5f);
    float epsilon = baseEps * std::max(0.1f, toleranceMultiplier);

    // 3. RDP extracts as many points as needed for the curve's geometry without arbitrary limits
    std::vector<CurvePoint> rdpPoints = simplifyStroke(smoothed, epsilon);

    if (rdpPoints.size() <= 2) return rdpPoints;

    // 4. Ensure points are not overly crowded together
    float minSpacing = std::max(0.6f, baseRadius * 0.35f) * std::max(0.2f, toleranceMultiplier);
    std::vector<CurvePoint> result;
    result.reserve(rdpPoints.size());
    result.push_back(rdpPoints.front());

    for (size_t i = 1; i + 1 < rdpPoints.size(); ++i) {
        if (glm::distance(rdpPoints[i].position, result.back().position) >= minSpacing) {
            result.push_back(rdpPoints[i]);
        }
    }

    // Always preserve exact endpoint
    if (result.size() > 1 && glm::distance(rdpPoints.back().position, result.back().position) < minSpacing * 0.6f) {
        result.back() = rdpPoints.back();
    } else {
        result.push_back(rdpPoints.back());
    }

    return result;
}
