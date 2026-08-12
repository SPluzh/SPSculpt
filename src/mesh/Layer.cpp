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
    layer.deltaColors.assign(nbVerts * 3, 0.0f);
    layer.deltaMaterials.assign(nbVerts * 3, 0.0f);
    m_layers.push_back(layer);
    m_activeIdx = (int)m_layers.size() - 1;
    return m_activeIdx;
}

void LayerStack::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    m_layers.erase(m_layers.begin() + idx);
    if (m_layers.empty()) {
        m_activeIdx = -1;
    } else {
        m_activeIdx = std::clamp(m_activeIdx, -1, (int)m_layers.size() - 1);
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
    m_layers.insert(m_layers.begin() + idx + 1, dup);
    m_activeIdx = idx + 1;
}

void LayerStack::mergeDown(int idx, std::vector<float>* outMeshVerts) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;

    if (idx == 0) {
        const Layer& source = m_layers[0];
        size_t count = std::min(m_baseVerts.size(), source.deltaVerts.size());
        for (size_t i = 0; i < count; ++i) {
            m_baseVerts[i] += source.deltaVerts[i] * source.intensity;
        }
        if (outMeshVerts && outMeshVerts->size() == m_baseVerts.size()) {
            *outMeshVerts = m_baseVerts;
        }

        if (!m_baseColors.empty()) {
            size_t countC = std::min(m_baseColors.size(), source.deltaColors.size());
            for (size_t i = 0; i < countC; ++i) {
                m_baseColors[i] = std::clamp(m_baseColors[i] + source.deltaColors[i] * source.intensity, 0.0f, 1.0f);
            }
        }
        if (!m_baseMaterials.empty()) {
            size_t countM = std::min(m_baseMaterials.size(), source.deltaMaterials.size());
            for (size_t i = 0; i < countM; ++i) {
                m_baseMaterials[i] = std::clamp(m_baseMaterials[i] + source.deltaMaterials[i] * source.intensity, 0.0f, 1.0f);
            }
        }

        removeLayer(0);
        if (m_layers.empty()) {
            m_activeIdx = -1;
        } else {
            m_activeIdx = std::clamp(m_activeIdx, -1, (int)m_layers.size() - 1);
        }
        return;
    }

    Layer& target = m_layers[idx - 1];
    const Layer& source = m_layers[idx];

    size_t count = std::min(target.deltaVerts.size(), source.deltaVerts.size());
    for (size_t i = 0; i < count; ++i) {
        target.deltaVerts[i] += source.deltaVerts[i] * source.intensity;
    }

    if (target.deltaColors.size() != source.deltaColors.size()) {
        target.deltaColors.resize(source.deltaColors.size(), 0.0f);
    }
    size_t countC = std::min(target.deltaColors.size(), source.deltaColors.size());
    for (size_t i = 0; i < countC; ++i) {
        target.deltaColors[i] += source.deltaColors[i] * source.intensity;
    }

    if (target.deltaMaterials.size() != source.deltaMaterials.size()) {
        target.deltaMaterials.resize(source.deltaMaterials.size(), 0.0f);
    }
    size_t countM = std::min(target.deltaMaterials.size(), source.deltaMaterials.size());
    for (size_t i = 0; i < countM; ++i) {
        target.deltaMaterials[i] += source.deltaMaterials[i] * source.intensity;
    }

    removeLayer(idx);
    m_activeIdx = idx - 1;
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

void LayerStack::bakeColors(const std::vector<float>& baseColors, std::vector<float>& outColors) const {
    bakeColorsExcept(-1, baseColors, outColors);
}

void LayerStack::bakeColorsExcept(int skipIdx, const std::vector<float>& baseColors, std::vector<float>& outColors) const {
    if (baseColors.empty()) return;
    outColors = baseColors;
    for (int idx = 0; idx < (int)m_layers.size(); ++idx) {
        if (idx == skipIdx) continue;
        const auto& layer = m_layers[idx];
        if (!layer.visible || std::abs(layer.intensity) <= 1e-6f) continue;
        size_t count = std::min(outColors.size(), layer.deltaColors.size());
        for (size_t i = 0; i < count; ++i) {
            outColors[i] = std::clamp(outColors[i] + layer.deltaColors[i] * layer.intensity, 0.0f, 1.0f);
        }
    }
}

void LayerStack::bakeMaterials(const std::vector<float>& baseMaterials, std::vector<float>& outMaterials) const {
    bakeMaterialsExcept(-1, baseMaterials, outMaterials);
}

void LayerStack::bakeMaterialsExcept(int skipIdx, const std::vector<float>& baseMaterials, std::vector<float>& outMaterials) const {
    if (baseMaterials.empty()) return;
    outMaterials = baseMaterials;
    for (int idx = 0; idx < (int)m_layers.size(); ++idx) {
        if (idx == skipIdx) continue;
        const auto& layer = m_layers[idx];
        if (!layer.visible || std::abs(layer.intensity) <= 1e-6f) continue;
        size_t count = std::min(outMaterials.size(), layer.deltaMaterials.size());
        for (size_t i = 0; i < count; ++i) {
            outMaterials[i] = std::clamp(outMaterials[i] + layer.deltaMaterials[i] * layer.intensity, 0.0f, 1.0f);
        }
    }
}

void LayerStack::initBase(const std::vector<float>& currentVerts) {
    m_baseVerts = currentVerts;
}

void LayerStack::initBaseColors(const std::vector<float>& currentColors) {
    m_baseColors = currentColors;
}

void LayerStack::initBaseMaterials(const std::vector<float>& currentMaterials) {
    m_baseMaterials = currentMaterials;
}

void LayerStack::onRemesh(const std::vector<float>& newVerts) {
    m_baseVerts = newVerts;
    m_baseColors.assign(newVerts.size(), 1.0f);
    m_baseMaterials.assign(newVerts.size(), 0.0f);
    for (auto& layer : m_layers) {
        layer.deltaVerts.assign(newVerts.size(), 0.0f);
        layer.deltaColors.assign(newVerts.size(), 0.0f);
        layer.deltaMaterials.assign(newVerts.size(), 0.0f);
    }
}

void LayerStack::clear() {
    m_layers.clear();
    m_baseVerts.clear();
    m_baseColors.clear();
    m_baseMaterials.clear();
    m_activeIdx = -1;
}
