#include "ArmatureTool.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
// Removed unused Geometry

ArmatureTool::ArmatureTool(SculptManager& sculptManager)
    : m_sculptManager(sculptManager) {
}

ArmatureGraph* ArmatureTool::getGraph(Scene& scene) {
    Mesh* active = scene.getSelected();
    if (active && active->isArmature) {
        return active->armatureGraph.get();
    }
    return nullptr;
}

const ArmatureGraph* ArmatureTool::getGraph(const Scene& scene) const {
    Mesh* active = scene.getSelected();
    if (active && active->isArmature) {
        return active->armatureGraph.get();
    }
    return nullptr;
}


void ArmatureTool::onActivate(Scene& scene) {
    auto* graph = getGraph(scene);
    if (!graph) {
        // Find existing armature first
        Mesh* existingArmature = nullptr;
        for (Mesh* m : scene.getMeshes()) {
            if (m->isArmature) {
                existingArmature = m;
                break;
            }
        }
        
        if (existingArmature) {
            scene.selectMesh(existingArmature);
            graph = existingArmature->armatureGraph.get();
        } else {
            Mesh* newMesh = new Mesh();
            newMesh->isArmature = true;
            newMesh->armatureGraph = std::make_unique<ArmatureGraph>();
            newMesh->outlinerName = "Armature";
            scene.addMesh(newMesh);
            scene.selectMesh(newMesh);
            graph = newMesh->armatureGraph.get();
        }
    }
    if (graph->getNodes().empty()) {
        glm::vec3 origin(0.0f);
        m_historyState = graph->serialize();
        graph->addRoot(origin, 2.5f);
        pushHistoryState(scene);
    }
}

void ArmatureTool::onDeactivate() {
    clearHoverAndSelection();
}

void ArmatureTool::clearHoverAndSelection() {
    m_selectedNode = nullptr;
    m_hoveredLinkParent = nullptr;
    m_hoveredLinkChild = nullptr;
}

void ArmatureTool::pushHistoryState(Scene& scene) {
    if (m_historyState.empty()) return;
    auto* graph = getGraph(scene);
    if (!graph) return;
    std::string after = graph->serialize();
    if (m_historyState != after) {
        // TODO: integrate with StateManager
    }
    m_historyState.clear();
}

Mesh* ArmatureTool::getActiveMesh() const {
    // Return active mesh if any, else null. For armature, we usually don't need a mesh until createMesh
    return nullptr;
}

glm::vec3 ArmatureTool::getSymmetricPosition(const glm::vec3& pos) const {
    // Simplification: use global X=0 plane for symmetry if no mesh
    glm::vec3 symPos = pos;
    symPos.x = -symPos.x;
    return symPos;
}

float ArmatureTool::getDistanceToSymmetryPlane(const glm::vec3& pos) const {
    glm::vec3 symPos = getSymmetricPosition(pos);
    return glm::distance(pos, symPos) * 0.5f;
}

glm::vec3 ArmatureTool::snapToSymmetryPlane(const glm::vec3& pos) const {
    glm::vec3 symPos = getSymmetricPosition(pos);
    return (pos + symPos) * 0.5f;
}

float ArmatureTool::getSymmetrySnapThreshold() const {
    if (!m_activeNode) return 0.08f;
    return std::max(0.08f, 0.15f * m_activeNode->radius);
}

float ArmatureTool::intersectRaySphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& center, float radius) const {
    glm::vec3 w = rayOrigin - center;
    float b = glm::dot(w, rayDir);
    float c = glm::dot(w, w) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float t = -b - std::sqrt(disc);
    if (t < 0.0f) t = -b + std::sqrt(disc);
    return t >= 0.0f ? t : -1.0f;
}

bool ArmatureTool::intersectLink(const glm::vec3& rayOrigin, const glm::vec3& rayDir, ArmatureNode* parent, ArmatureNode* child, ArmatureHitResult& outRes) const {
    glm::vec3 A = parent->position;
    glm::vec3 B = child->position;
    float rA = parent->radius;
    float rB = child->radius;

    glm::vec3 V = B - A;
    glm::vec3 U = rayOrigin - A;

    float b = glm::dot(rayDir, V);
    float c = glm::dot(V, V);
    if (c < 1e-6f) return false;

    float d = glm::dot(rayDir, U);
    float e = glm::dot(V, U);
    float det = c - b * b;

    float t, u;
    if (std::abs(det) < 1e-6f) {
        u = 0.5f;
        t = u * b - d;
    } else {
        t = (b * e - c * d) / det;
        u = (e - b * d) / det;
    }

    if (u < 0.0f) { u = 0.0f; t = -d; }
    else if (u > 1.0f) { u = 1.0f; t = b - d; }

    if (t < 0.0f) t = 0.0f;

    glm::vec3 P_ray = rayOrigin + rayDir * t;
    glm::vec3 P_seg = A + V * u;

    float dist = glm::distance(P_ray, P_seg);
    float r_u = rA + u * (rB - rA);

    if (dist < r_u * 1.5f) { // Some threshold buffer
        outRes.t = t;
        outRes.u = u;
        outRes.position = P_seg;
        outRes.radius = r_u;
        outRes.linkParent = parent;
        outRes.linkChild = child;
        return true;
    }
    return false;
}

ArmatureHitResult ArmatureTool::hitTest(const Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir) const {
    ArmatureHitResult res;
    auto* graph = getGraph(scene);
    if (!graph) return res;

    float minDist = 1e9f;

    const auto& nodes = graph->getNodes();

    // In DRAW mode, hit test only against nodes.
    // In INSERT mode, hit test only against links.
    // In other modes, hit test against nodes.
    
    if (m_mode != ArmatureMode::INSERT) {
        for (const auto& nodePtr : nodes) {
            ArmatureNode* node = nodePtr.get();
            float t = intersectRaySphere(rayOrigin, rayDir, node->position, node->radius);
            if (t >= 0.0f && t < minDist) {
                minDist = t;
                res.type = ArmatureHitResult::NODE;
                res.node = node;
                res.t = t;
            }
        }
    }

    if (m_mode == ArmatureMode::INSERT) {
        for (const auto& childPtr : nodes) {
            ArmatureNode* child = childPtr.get();
            ArmatureNode* parent = child->parent;
            if (!parent) continue;

            ArmatureHitResult tempRes;
            if (intersectLink(rayOrigin, rayDir, parent, child, tempRes)) {
                if (tempRes.t >= 0.0f && tempRes.t < minDist) {
                    minDist = tempRes.t;
                    res = tempRes;
                    res.type = ArmatureHitResult::LINK;
                }
            }
        }
    }

    return res;
}

void ArmatureTool::preUpdate(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt) {
    auto* graph = getGraph(scene);
    if (!graph) return;

    if (isCtrl || isAlt) {
        clearHoverAndSelection();
        return;
    }

    if (m_isDragging && m_activeNode) {
        m_selectedNode = m_activeNode;
        m_hoveredLinkParent = nullptr;
        m_hoveredLinkChild = nullptr;
        return;
    }

    glm::vec3 vNear = camera.unproject(mouseX, mouseY, 0.0f);
    glm::vec3 vFar = camera.unproject(mouseX, mouseY, 0.1f);
    glm::vec3 rayDir = glm::normalize(vFar - vNear);

    ArmatureHitResult hit = hitTest(scene, vNear, rayDir);
    if (hit.type == ArmatureHitResult::NODE) {
        m_selectedNode = hit.node;
        m_hoveredLinkParent = nullptr;
        m_hoveredLinkChild = nullptr;
    } else if (hit.type == ArmatureHitResult::LINK) {
        m_selectedNode = nullptr;
        m_hoveredLinkParent = hit.linkParent;
        m_hoveredLinkChild = hit.linkChild;
        m_previewPosition = hit.position;
        m_previewRadius = hit.radius;
    } else {
        clearHoverAndSelection();
    }
}

bool ArmatureTool::start(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt) {
    auto* graph = getGraph(scene);
    if (!graph) {
        Mesh* existingArmature = nullptr;
        for (Mesh* m : scene.getMeshes()) {
            if (m->isArmature) {
                existingArmature = m;
                break;
            }
        }
        
        if (existingArmature) {
            scene.selectMesh(existingArmature);
            graph = existingArmature->armatureGraph.get();
        } else {
            Mesh* newMesh = new Mesh();
            newMesh->isArmature = true;
            newMesh->armatureGraph = std::make_unique<ArmatureGraph>();
            newMesh->outlinerName = "Armature";
            scene.addMesh(newMesh);
            scene.selectMesh(newMesh);
            graph = newMesh->armatureGraph.get();
        }
    }
    m_historyState = graph->serialize();


    if (isAlt) {
        m_historyState.clear();
        return false;
    }

    glm::vec3 vNear = camera.unproject(mouseX, mouseY, 0.0f);
    glm::vec3 vFar = camera.unproject(mouseX, mouseY, 0.1f);
    glm::vec3 rayDir = glm::normalize(vFar - vNear);

    ArmatureHitResult hit = hitTest(scene, vNear, rayDir);

    if (isCtrl) {
        if (hit.type == ArmatureHitResult::NODE) {
            if (hit.node->symmetryPartner) {
                ArmatureNode* partner = hit.node->symmetryPartner;
                hit.node->symmetryPartner = nullptr;
                partner->symmetryPartner = nullptr;
                graph->removeNode(partner);
            }
            graph->removeNode(hit.node);
            m_selectedNode = nullptr;
            pushHistoryState(scene);
            return true;
        }
        m_historyState.clear();
        return false;
    }

    bool isSym = m_sculptManager.getUseSym();

    if (m_mode == ArmatureMode::DRAW) {
        if (graph->getNodes().empty()) {
            glm::vec3 hitPoint = camera.getPivot();
            if (isSym) hitPoint = snapToSymmetryPlane(hitPoint);
            graph->addRoot(hitPoint, 2.5f);
            return true;
        }

        if (hit.type == ArmatureHitResult::NODE) {
            ArmatureNode* parent = hit.node;
            float newRadius = parent->radius * 0.7f;
            ArmatureNode* child = graph->addChild(parent, parent->position, newRadius);

            if (isSym && parent->symmetryPartner && parent->symmetryPartner != parent) {
                ArmatureNode* partnerChild = graph->addChild(parent->symmetryPartner, parent->symmetryPartner->position, newRadius);
                child->symmetryPartner = partnerChild;
                partnerChild->symmetryPartner = child;
            }

            m_activeNode = child;
            m_isDragging = true;
            m_dragMode = ArmatureMode::DRAW;
            return true;
        }
    } else if (m_mode == ArmatureMode::INSERT) {
        if (hit.type == ArmatureHitResult::LINK) {
            ArmatureNode* parent = hit.linkParent;
            ArmatureNode* child = hit.linkChild;
            ArmatureNode* newNode = graph->insertOnLink(parent, child, hit.position, hit.radius);

            if (isSym && parent->symmetryPartner && child->symmetryPartner) {
                ArmatureNode* pParent = parent->symmetryPartner;
                ArmatureNode* pChild = child->symmetryPartner;
                glm::vec3 symPos = getSymmetricPosition(hit.position);
                ArmatureNode* pNewNode = graph->insertOnLink(pParent, pChild, symPos, hit.radius);
                newNode->symmetryPartner = pNewNode;
                pNewNode->symmetryPartner = newNode;
            }

            m_activeNode = newNode;
            m_isDragging = true;
            m_dragMode = ArmatureMode::SCALE;
            m_startRadius = newNode->radius;
            m_startMouseX = mouseX;
            return true;
        }
    } else if (m_mode == ArmatureMode::MOVE) {
        if (hit.type == ArmatureHitResult::NODE) {
            m_activeNode = hit.node;
            m_isDragging = true;
            m_dragMode = ArmatureMode::MOVE;
            m_screenZ = camera.project(hit.node->position).z;
            return true;
        }
    } else if (m_mode == ArmatureMode::SCALE) {
        if (hit.type == ArmatureHitResult::NODE) {
            m_activeNode = hit.node;
            m_isDragging = true;
            m_dragMode = ArmatureMode::SCALE;
            m_startRadius = hit.node->radius;
            m_startMouseX = mouseX;
            return true;
        }
    } else if (m_mode == ArmatureMode::ROTATE) {
        if (hit.type == ArmatureHitResult::NODE) {
            m_activeNode = hit.node;
            m_isDragging = true;
            m_dragMode = ArmatureMode::ROTATE;
            m_startMouseX = mouseX;
            m_startMouseY = mouseY;

            m_initialPositions.clear();
            auto desc = graph->getDescendants(m_activeNode);
            for (auto* d : desc) m_initialPositions[d->id] = d->position;

            if (m_activeNode->symmetryPartner) {
                auto pDesc = graph->getDescendants(m_activeNode->symmetryPartner);
                for (auto* d : pDesc) m_initialPositions[d->id] = d->position;
            }
            return true;
        }
    }

    m_historyState.clear();
    return false;
}

void ArmatureTool::update(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    auto* graph = getGraph(scene);
    if (!graph) return;

    if (!m_isDragging || !m_activeNode) return;

    bool isSym = m_sculptManager.getUseSym();

    if (m_dragMode == ArmatureMode::DRAW) {
        ArmatureNode* parent = m_activeNode->parent;
        if (parent) {
            glm::vec3 screenParent = camera.project(parent->position);
            glm::vec3 worldPos = camera.unproject(mouseX, mouseY, screenParent.z);
            m_activeNode->position = worldPos;
            m_activeNode->radius = parent->radius * 0.7f;

            if (isSym) {
                if (!m_activeNode->symmetryPartner) {
                    if (getDistanceToSymmetryPlane(m_activeNode->position) > getSymmetrySnapThreshold()) {
                        ArmatureNode* partnerParent = parent->symmetryPartner ? parent->symmetryPartner : parent;
                        glm::vec3 symPos = getSymmetricPosition(m_activeNode->position);
                        ArmatureNode* pChild = graph->addChild(partnerParent, symPos, m_activeNode->radius);
                        m_activeNode->symmetryPartner = pChild;
                        pChild->symmetryPartner = m_activeNode;
                    } else {
                        m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
                    }
                } else {
                    if (getDistanceToSymmetryPlane(m_activeNode->position) <= getSymmetrySnapThreshold()) {
                        ArmatureNode* partner = m_activeNode->symmetryPartner;
                        m_activeNode->symmetryPartner = nullptr;
                        partner->symmetryPartner = nullptr;
                        graph->mergeNodes(partner, m_activeNode);
                        m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
                    } else {
                        ArmatureNode* partner = m_activeNode->symmetryPartner;
                        partner->position = getSymmetricPosition(m_activeNode->position);
                        partner->radius = m_activeNode->radius;
                    }
                }
            }
        }
    } else if (m_dragMode == ArmatureMode::MOVE) {
        glm::vec3 worldPos = camera.unproject(mouseX, mouseY, m_screenZ);
        m_activeNode->position = worldPos;
        
        if (isSym) {
            ArmatureNode* parent = m_activeNode->parent;
            if (parent) {
                if (!m_activeNode->symmetryPartner) {
                    m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
                } else {
                    if (getDistanceToSymmetryPlane(m_activeNode->position) <= getSymmetrySnapThreshold()) {
                        ArmatureNode* partner = m_activeNode->symmetryPartner;
                        m_activeNode->symmetryPartner = nullptr;
                        partner->symmetryPartner = nullptr;
                        graph->mergeNodes(partner, m_activeNode);
                        m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
                    } else {
                        m_activeNode->symmetryPartner->position = getSymmetricPosition(m_activeNode->position);
                    }
                }
            } else {
                m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
            }
        }
    } else if (m_dragMode == ArmatureMode::SCALE) {
        float dx = mouseX - m_startMouseX;
        m_activeNode->radius = std::max(0.05f, m_startRadius + dx * 0.01f);
        if (isSym && m_activeNode->symmetryPartner) {
            m_activeNode->symmetryPartner->radius = m_activeNode->radius;
        }
    } else if (m_dragMode == ArmatureMode::ROTATE) {
        float dx = mouseX - m_startMouseX;
        float dy = mouseY - m_startMouseY;

        glm::mat4 invView = glm::inverse(camera.getViewMatrix());
        glm::vec3 camRight(invView[0][0], invView[1][0], invView[2][0]);
        glm::vec3 camUp(invView[0][1], invView[1][1], invView[2][1]);

        float angleX = dx * 0.005f;
        float angleY = dy * 0.005f;

        glm::quat qX = glm::angleAxis(angleX, camUp);
        glm::quat qY = glm::angleAxis(angleY, camRight);
        glm::quat q = qX * qY;

        auto desc = graph->getDescendants(m_activeNode);
        std::vector<ArmatureNode*> rotated;

        for (auto* d : desc) {
            if (d == m_activeNode->symmetryPartner) continue;
            if (isSym && d->symmetryPartner && std::find(rotated.begin(), rotated.end(), d->symmetryPartner) != rotated.end()) {
                continue; // will mirror later
            }
            if (m_initialPositions.count(d->id)) {
                glm::vec3 relPos = m_initialPositions[d->id] - m_activeNode->position;
                relPos = q * relPos;
                d->position = m_activeNode->position + relPos;
                rotated.push_back(d);
            }
        }

        if (isSym) {
            for (auto* d : rotated) {
                if (d->symmetryPartner) {
                    d->symmetryPartner->position = getSymmetricPosition(d->position);
                }
            }

            if (m_activeNode->symmetryPartner) {
                ArmatureNode* pNode = m_activeNode->symmetryPartner;
                auto pDesc = graph->getDescendants(pNode);
                for (auto* d : pDesc) {
                    if (d->symmetryPartner) {
                        d->position = getSymmetricPosition(d->symmetryPartner->position);
                    } else if (m_initialPositions.count(d->id)) {
                        glm::vec3 relPos = m_initialPositions[d->id] - pNode->position;
                        glm::quat qSym(q.w, q.x, -q.y, -q.z); // Approximation for mirrored rotation
                        relPos = qSym * relPos;
                        d->position = pNode->position + relPos;
                    }
                }
            }
        }
    }
}

void ArmatureTool::end(Scene& scene) {
    auto* graph = getGraph(scene);
    if (!graph) {
        m_isDragging = false;
        m_activeNode = nullptr;
        return;
    }

    if ((m_dragMode == ArmatureMode::DRAW || m_dragMode == ArmatureMode::MOVE) && m_activeNode) {
        const auto& nodes = graph->getNodes();
        ArmatureNode* minNode = nullptr;
        float minDist = 1e9f;

        for (const auto& nPtr : nodes) {
            ArmatureNode* n = nPtr.get();
            if (n == m_activeNode) continue;
            float d = glm::distance(m_activeNode->position, n->position);
            if (d < minDist) {
                minDist = d;
                minNode = n;
            }
        }

        if (minNode) {
            float threshold = 0.3f * (m_activeNode->radius + minNode->radius);
            
            ArmatureNode* partner = m_activeNode->symmetryPartner;
            bool mergePartners = false;
            if (partner) {
                bool closeToCenter = getDistanceToSymmetryPlane(m_activeNode->position) < getSymmetrySnapThreshold();
                if ((minNode == partner && minDist < threshold) || closeToCenter) {
                    mergePartners = true;
                }
            }

            if (mergePartners) {
                m_activeNode->symmetryPartner = nullptr;
                partner->symmetryPartner = nullptr;
                graph->mergeNodes(partner, m_activeNode);
                m_activeNode->position = snapToSymmetryPlane(m_activeNode->position);
            } else {
                bool activeHasPartner = (m_activeNode->symmetryPartner != nullptr);
                bool minHasPartner = (minNode->symmetryPartner != nullptr);
                if (activeHasPartner == minHasPartner && minDist < threshold) {
                    if (m_activeNode->symmetryPartner && minNode->symmetryPartner) {
                        graph->mergeNodes(m_activeNode->symmetryPartner, minNode->symmetryPartner);
                    }
                    graph->mergeNodes(m_activeNode, minNode);
                }
            }
        }
    }

    m_isDragging = false;
    m_activeNode = nullptr;
    pushHistoryState(scene);
}

#include "sculpt/Remesh.h"
#include "mesh/Topology.h"
#include <limits>

void ArmatureTool::createMesh(Scene& scene) {
    auto* graph = getGraph(scene);
    if (!graph) return;
    const auto& nodes = graph->getNodes();
    if (nodes.empty()) return;

    // 1. Calculate bounding box of all nodes
    float bbox[6] = {
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()
    };
    float maxRadius = 0.0f;
    for (const auto& nodePtr : nodes) {
        auto* node = nodePtr.get();
        const glm::vec3& pos = node->position;
        float r = node->radius;
        if (r > maxRadius) maxRadius = r;
        if (pos.x - r < bbox[0]) bbox[0] = pos.x - r;
        if (pos.y - r < bbox[1]) bbox[1] = pos.y - r;
        if (pos.z - r < bbox[2]) bbox[2] = pos.z - r;
        if (pos.x + r > bbox[3]) bbox[3] = pos.x + r;
        if (pos.y + r > bbox[4]) bbox[4] = pos.y + r;
        if (pos.z + r > bbox[5]) bbox[5] = pos.z + r;
    }

    float padding = maxRadius * 2.0f;
    float minCoords[3] = { bbox[0] - padding, bbox[1] - padding, bbox[2] - padding };
    float maxCoords[3] = { bbox[3] + padding, bbox[4] + padding, bbox[5] + padding };

    // 2. Evaluate SDF
    int resolution = 64;
    int total = resolution * resolution * resolution;
    std::vector<float> distanceField(total);

    float stepX = (maxCoords[0] - minCoords[0]) / (resolution - 1);
    float stepY = (maxCoords[1] - minCoords[1]) / (resolution - 1);
    float stepZ = (maxCoords[2] - minCoords[2]) / (resolution - 1);

    auto smin = [](float a, float b, float k) {
        float h = std::max(k - std::abs(a - b), 0.0f) / k;
        return std::min(a, b) - h * h * h * k * (1.0f / 6.0f);
    };

    auto evalSDF = [&](const glm::vec3& pt) {
        float minDist = std::numeric_limits<float>::infinity();
        float k = 0.5f;

        for (const auto& nodePtr : nodes) {
            auto* n = nodePtr.get();
            float d = glm::distance(pt, n->position) - n->radius;
            if (minDist == std::numeric_limits<float>::infinity()) minDist = d;
            else minDist = smin(minDist, d, k);
        }

        for (const auto& childPtr : nodes) {
            auto* child = childPtr.get();
            auto* parent = child->parent;
            if (!parent) continue;

            const glm::vec3& A = parent->position;
            const glm::vec3& B = child->position;
            float rA = parent->radius;
            float rB = child->radius;

            glm::vec3 V = B - A;
            float lenV = glm::length(V);
            if (lenV < 1e-4f) continue;
            
            glm::vec3 W = pt - A;
            float t = glm::dot(W, V) / (lenV * lenV);
            t = std::max(0.0f, std::min(1.0f, t));

            glm::vec3 proj = A + V * t;
            float r_t = rA + t * (rB - rA);
            float d = glm::distance(pt, proj) - r_t;
            minDist = smin(minDist, d, k);
        }
        return minDist;
    };

    int index = 0;
    for (int gz = 0; gz < resolution; ++gz) {
        float z = minCoords[2] + gz * stepZ;
        for (int gy = 0; gy < resolution; ++gy) {
            float y = minCoords[1] + gy * stepY;
            for (int gx = 0; gx < resolution; ++gx) {
                float x = minCoords[0] + gx * stepX;
                glm::vec3 P(x, y, z);
                distanceField[index++] = evalSDF(P);
            }
        }
    }

    // 3. Reconstruct Surface
    float uniformColor[3] = {1.0f, 1.0f, 1.0f};
    float uniformMaterial[3] = {0.5f, 0.0f, 1.0f};
    RemeshResult meshData = doSurfaceNetsFromSDF(resolution, minCoords, maxCoords, distanceField.data(), uniformColor, uniformMaterial);

    Mesh* newMesh = new Mesh();
    newMesh->verts = std::move(meshData.vertices);
    newMesh->faces = std::move(meshData.faces);
    if (!meshData.colors.empty()) newMesh->colors = std::move(meshData.colors);
    if (!meshData.materials.empty()) newMesh->materials = std::move(meshData.materials);
    newMesh->nbVerts = newMesh->verts.size() / 3;
    newMesh->nbFaces = newMesh->faces.size() / 4;

    std::vector<uint32_t> vrvStartCount, vertRingVert, vrfStartCount, vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(
        newMesh->nbVerts, newMesh->faces.data(), newMesh->nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );

    newMesh->vrfStartCount = std::move(vrfStartCount);
    newMesh->vertRingFace = std::move(vertRingFace);
    newMesh->vrvStartCount = std::move(vrvStartCount);
    newMesh->vertRingVert = std::move(vertRingVert);
    newMesh->vertOnEdge = std::move(vertOnEdge);

    newMesh->postInit();
    newMesh->isDirty = true;

    scene.addMesh(newMesh);
    
    // Auto-select the newly created mesh
    auto& meshes = scene.getMeshes();
    if (!meshes.empty()) {
        scene.selectMesh(meshes.back());
    }
}
