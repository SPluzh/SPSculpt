#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

struct ArmatureNode {
    uint32_t id;
    glm::vec3 position;
    float radius;

    ArmatureNode* parent = nullptr;
    std::vector<ArmatureNode*> children;
    ArmatureNode* symmetryPartner = nullptr;

    ArmatureNode(uint32_t id, const glm::vec3& pos, float r)
        : id(id), position(pos), radius(r) {}
};

class ArmatureGraph {
public:
    ArmatureGraph() = default;
    ~ArmatureGraph();

    ArmatureNode* addRoot(const glm::vec3& pos, float radius);
    ArmatureNode* addChild(ArmatureNode* parent, const glm::vec3& pos, float radius);
    
    // In Insert mode, we split a link between parent and child.
    ArmatureNode* insertOnLink(ArmatureNode* parent, ArmatureNode* child, const glm::vec3& pos, float radius);

    void removeNode(ArmatureNode* node);
    void mergeNodes(ArmatureNode* nodeToKeep, ArmatureNode* nodeToRemove);
    void clear();

    const std::vector<std::unique_ptr<ArmatureNode>>& getNodes() const { return m_nodes; }
    
    std::string serialize() const;
    void deserialize(const std::string& data);

    std::vector<ArmatureNode*> getDescendants(ArmatureNode* node) const;

private:
    std::vector<std::unique_ptr<ArmatureNode>> m_nodes;
    uint32_t m_nextId = 1;
};
