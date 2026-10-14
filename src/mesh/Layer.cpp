#include "mesh/Layer.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>

int LayerStack::addLayer(int nbVerts, const std::string& name) {
    Layer layer;
    if (!name.empty()) {
        layer.name = name;
    } else {
        layer.name = "Layer " + std::to_string(m_layers.size() + 1);
    }
    layer.deltaVerts.assign(nbVerts * 3, 0.0f);
    layer.recording = true;
    for (auto& l : m_layers) {
        l.recording = false;
    }
    m_layers.push_back(layer);
    m_activeIdx = (int)m_layers.size() - 1;
    return m_activeIdx;
}

void LayerStack::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    bool wasRecording = m_layers[idx].recording;
    m_layers.erase(m_layers.begin() + idx);
    if (m_layers.empty()) {
        m_activeIdx = -1;
    } else {
        m_activeIdx = std::clamp(m_activeIdx, 0, (int)m_layers.size() - 1);
        if (wasRecording && m_activeIdx >= 0) {
            m_layers[m_activeIdx].recording = true;
        }
    }
}

void LayerStack::moveLayer(int from, int to) {
    if (from < 0 || from >= (int)m_layers.size()) return;
    if (to < 0 || to >= (int)m_layers.size()) return;
    if (from == to) return;

    Layer layer = m_layers[from];
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, layer);
    m_activeIdx = to;
}

void LayerStack::duplicateLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    Layer dup = m_layers[idx];
    static uint32_t s_dupId = 1000;
    dup.id = ++s_dupId;
    dup.name += " Copy";
    dup.recording = true;
    for (auto& l : m_layers) {
        l.recording = false;
    }
    m_layers.insert(m_layers.begin() + idx + 1, dup);
    m_activeIdx = idx + 1;
}

void LayerStack::mergeDown(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;

    if (idx > 0) {
        Layer& target = m_layers[idx - 1];
        const Layer& source = m_layers[idx];

        bool wasRecording = source.recording || target.recording;
        float targetScale = (std::abs(target.intensity) > 1e-4f) ? (1.0f / target.intensity) : 1.0f;
        size_t count = std::min(target.deltaVerts.size(), source.deltaVerts.size());
        for (size_t i = 0; i < count; ++i) {
            target.deltaVerts[i] += source.deltaVerts[i] * source.intensity * targetScale;
        }

        removeLayer(idx);
        m_activeIdx = idx - 1;
        if (wasRecording && m_activeIdx >= 0) {
            m_layers[m_activeIdx].recording = true;
        }
    } else {
        const Layer& source = m_layers[0];
        if (!m_baseVerts.empty()) {
            size_t count = std::min(m_baseVerts.size(), source.deltaVerts.size());
            for (size_t i = 0; i < count; ++i) {
                m_baseVerts[i] += source.deltaVerts[i] * source.intensity;
            }
        }
        removeLayer(0);
        if (m_layers.empty()) {
            clear();
        } else {
            m_activeIdx = 0;
        }
    }
}

void LayerStack::mergeAll(std::vector<float>& outVerts) {
    if (m_layers.empty()) return;
    if (!m_baseVerts.empty()) {
        bake(m_baseVerts, outVerts);
    }
    clear();
}

void LayerStack::bake(const std::vector<float>& baseVerts, std::vector<float>& outVerts) const {
    if (baseVerts.empty()) {
        sculpt_log_lvl(LogLevel::Warning, "[LayerStack bake WARNING] baseVerts is empty! Cannot bake layers.\n");
        return;
    }
    outVerts = baseVerts;
    size_t nbVerts3 = baseVerts.size();

    int activeLayersCount = 0;
    for (const auto& layer : m_layers) {
        if (!layer.visible || std::abs(layer.intensity) <= 1e-6f) continue;
        size_t count = std::min(nbVerts3, layer.deltaVerts.size());
        if (layer.deltaVerts.size() != nbVerts3) {
            sculpt_log_lvl(LogLevel::Warning, "[LayerStack bake WARNING] Layer '%s' deltaVerts size mismatch (%zu vs base %zu).\n",
                           layer.name.c_str(), layer.deltaVerts.size(), nbVerts3);
        }
        for (size_t i = 0; i < count; ++i) {
            outVerts[i] += layer.deltaVerts[i] * layer.intensity;
        }
        activeLayersCount++;
    }
    sculpt_log_lvl(LogLevel::Debug, "[LayerStack bake] Baked %d layers into mesh (%zu floats).\n", activeLayersCount, nbVerts3);
}

void LayerStack::initBase(const std::vector<float>& currentVerts) {
    m_baseVerts = currentVerts;
}

void LayerStack::onRemesh(const std::vector<float>& newVerts) {
    m_baseVerts = newVerts;
    for (auto& layer : m_layers) {
        layer.deltaVerts.assign(newVerts.size(), 0.0f);
    }
}

void LayerStack::clear() {
    m_layers.clear();
    m_baseVerts.clear();
    m_activeIdx = -1;
}
