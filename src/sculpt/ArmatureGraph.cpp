#include "ArmatureGraph.h"
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ArmatureGraph::~ArmatureGraph() {
    clear();
}

ArmatureNode* ArmatureGraph::addRoot(const glm::vec3& pos, float radius) {
    auto node = std::make_unique<ArmatureNode>(m_nextId++, pos, radius);
    ArmatureNode* ptr = node.get();
    m_nodes.push_back(std::move(node));
    return ptr;
}

ArmatureNode* ArmatureGraph::addChild(ArmatureNode* parent, const glm::vec3& pos, float radius) {
    auto node = std::make_unique<ArmatureNode>(m_nextId++, pos, radius);
    ArmatureNode* ptr = node.get();
    node->parent = parent;
    if (parent) {
        parent->children.push_back(ptr);
    }
    m_nodes.push_back(std::move(node));
    return ptr;
}

ArmatureNode* ArmatureGraph::insertOnLink(ArmatureNode* parent, ArmatureNode* child, const glm::vec3& pos, float radius) {
    auto node = std::make_unique<ArmatureNode>(m_nextId++, pos, radius);
    ArmatureNode* ptr = node.get();
    
    ptr->parent = parent;
    if (parent) {
        auto it = std::find(parent->children.begin(), parent->children.end(), child);
        if (it != parent->children.end()) {
            parent->children.erase(it);
        }
        parent->children.push_back(ptr);
    }
    
    ptr->children.push_back(child);
    child->parent = ptr;
    
    m_nodes.push_back(std::move(node));
    return ptr;
}

void ArmatureGraph::removeNode(ArmatureNode* node) {
    if (!node) return;
    
    // Reparent children to node's parent
    for (auto* child : node->children) {
        child->parent = node->parent;
        if (node->parent) {
            node->parent->children.push_back(child);
        }
    }
    
    if (node->parent) {
        auto& siblings = node->parent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
    }
    
    // Break symmetry link if any
    if (node->symmetryPartner) {
        node->symmetryPartner->symmetryPartner = nullptr;
    }
    
    // Erase unique_ptr from m_nodes
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(),
        [node](const std::unique_ptr<ArmatureNode>& uptr) {
            return uptr.get() == node;
        }), m_nodes.end());
}

void ArmatureGraph::mergeNodes(ArmatureNode* nodeToKeep, ArmatureNode* nodeToRemove) {
    if (!nodeToKeep || !nodeToRemove || nodeToKeep == nodeToRemove) return;
    
    for (auto* child : nodeToRemove->children) {
        child->parent = nodeToKeep;
        nodeToKeep->children.push_back(child);
    }
    
    if (nodeToRemove->parent) {
        auto& siblings = nodeToRemove->parent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), nodeToRemove), siblings.end());
        // Avoid adding to keep's parent if keeping node is child of keep's parent already
        // Wait, nodeToKeep already has its parent, we shouldn't reparent nodeToKeep.
        // But what if nodeToRemove had a parent? We might need to link nodeToKeep to nodeToRemove's parent if nodeToKeep is isolated.
        // Usually mergeNodes is called to snap symmetry pairs or collapse near nodes.
        if (!nodeToKeep->parent && nodeToRemove->parent && nodeToRemove->parent != nodeToKeep) {
            nodeToKeep->parent = nodeToRemove->parent;
            nodeToRemove->parent->children.push_back(nodeToKeep);
        }
    }
    
    if (nodeToRemove->symmetryPartner) {
        nodeToRemove->symmetryPartner->symmetryPartner = nullptr;
    }
    
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(),
        [nodeToRemove](const std::unique_ptr<ArmatureNode>& uptr) {
            return uptr.get() == nodeToRemove;
        }), m_nodes.end());
}

void ArmatureGraph::clear() {
    m_nodes.clear();
    m_nextId = 1;
}

std::vector<ArmatureNode*> ArmatureGraph::getDescendants(ArmatureNode* node) const {
    std::vector<ArmatureNode*> desc;
    if (!node) return desc;
    std::vector<ArmatureNode*> stack;
    stack.insert(stack.end(), node->children.begin(), node->children.end());
    while (!stack.empty()) {
        ArmatureNode* curr = stack.back();
        stack.pop_back();
        desc.push_back(curr);
        stack.insert(stack.end(), curr->children.begin(), curr->children.end());
    }
    return desc;
}

std::string ArmatureGraph::serialize() const {
    json j = json::array();
    for (const auto& node : m_nodes) {
        json jNode;
        jNode["id"] = node->id;
        jNode["px"] = node->position.x;
        jNode["py"] = node->position.y;
        jNode["pz"] = node->position.z;
        jNode["r"] = node->radius;
        jNode["parent"] = node->parent ? node->parent->id : 0;
        jNode["sym"] = node->symmetryPartner ? node->symmetryPartner->id : 0;
        j.push_back(jNode);
    }
    return j.dump();
}

void ArmatureGraph::deserialize(const std::string& data) {
    clear();
    if (data.empty() || data == "[]") return;
    try {
        json j = json::parse(data);
        uint32_t maxId = 0;
        for (const auto& jNode : j) {
            uint32_t id = jNode["id"];
            glm::vec3 pos(jNode["px"], jNode["py"], jNode["pz"]);
            float r = jNode["r"];
            auto node = std::make_unique<ArmatureNode>(id, pos, r);
            m_nodes.push_back(std::move(node));
            if (id > maxId) maxId = id;
        }
        m_nextId = maxId + 1;
        
        // Restore links
        for (const auto& jNode : j) {
            uint32_t id = jNode["id"];
            uint32_t parentId = jNode["parent"];
            uint32_t symId = jNode["sym"];
            
            ArmatureNode* node = nullptr;
            for (auto& n : m_nodes) if (n->id == id) { node = n.get(); break; }
            if (!node) continue;
            
            if (parentId) {
                for (auto& n : m_nodes) if (n->id == parentId) {
                    node->parent = n.get();
                    n->children.push_back(node);
                    break;
                }
            }
            if (symId) {
                for (auto& n : m_nodes) if (n->id == symId) {
                    node->symmetryPartner = n.get();
                    break;
                }
            }
        }
    } catch (...) {
        // Handle parsing errors
    }
}
