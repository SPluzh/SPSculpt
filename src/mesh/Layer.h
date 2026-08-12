#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

struct Layer {
    uint32_t    id        = 0;
    std::string name      = "Layer";
    bool        visible   = true;
    float       intensity = 1.0f; // 0.0 - 1.0

    // Per-vertex displacement from base pose (nbVerts * 3 floats)
    std::vector<float> deltaVerts;

    Layer() {
        static uint32_t s_id = 0;
        id = ++s_id;
    }
};

class LayerStack {
public:
    LayerStack() = default;
    ~LayerStack() = default;

    int addLayer(int nbVerts, const std::string& name = "");
    void removeLayer(int idx);
    void moveLayer(int from, int to);
    void duplicateLayer(int idx);
    void mergeDown(int idx);

    int getActiveIdx() const { return m_activeIdx; }
    void setActiveIdx(int idx) {
        if (m_layers.empty()) m_activeIdx = -1;
        else m_activeIdx = std::clamp(idx, 0, (int)m_layers.size() - 1);
    }
    Layer* getActive() {
        if (m_activeIdx >= 0 && m_activeIdx < (int)m_layers.size()) {
            return &m_layers[m_activeIdx];
        }
        return nullptr;
    }
    const Layer* getActive() const {
        if (m_activeIdx >= 0 && m_activeIdx < (int)m_layers.size()) {
            return &m_layers[m_activeIdx];
        }
        return nullptr;
    }

    int count() const { return (int)m_layers.size(); }
    Layer& at(int i) { return m_layers[i]; }
    const Layer& at(int i) const { return m_layers[i]; }

    Layer* getLayer(int idx) {
        if (idx >= 0 && idx < (int)m_layers.size()) {
            return &m_layers[idx];
        }
        return nullptr;
    }
    const Layer* getLayer(int idx) const {
        if (idx >= 0 && idx < (int)m_layers.size()) {
            return &m_layers[idx];
        }
        return nullptr;
    }

    void bake(const std::vector<float>& baseVerts, std::vector<float>& outVerts) const;

    void initBase(const std::vector<float>& currentVerts);
    const std::vector<float>& getBase() const { return m_baseVerts; }
    std::vector<float>& getBase() { return m_baseVerts; }
    bool hasBase() const { return !m_baseVerts.empty(); }

    void onRemesh(const std::vector<float>& newVerts);
    void clear();

private:
    std::vector<Layer> m_layers;
    std::vector<float> m_baseVerts;
    int m_activeIdx = -1;
};
