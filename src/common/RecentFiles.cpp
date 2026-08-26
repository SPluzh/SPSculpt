#include "common/RecentFiles.h"
#include <algorithm>

std::string RecentFiles::normalizePath(const std::string& path) {
    std::string norm = path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    return norm;
}

void RecentFiles::addFile(const std::string& path) {
    if (path.empty()) return;
    std::string normPath = normalizePath(path);

    auto it = std::find(m_files.begin(), m_files.end(), normPath);
    if (it != m_files.end()) {
        m_files.erase(it);
    }

    m_files.insert(m_files.begin(), normPath);

    if (m_files.size() > MAX_FILES) {
        m_files.resize(MAX_FILES);
    }
}

void RecentFiles::removeFile(const std::string& path) {
    if (path.empty()) return;
    std::string normPath = normalizePath(path);

    auto it = std::find(m_files.begin(), m_files.end(), normPath);
    if (it != m_files.end()) {
        m_files.erase(it);
    }
}

void RecentFiles::saveToIni(IniFile& ini) const {
    ini.setInt("RecentFiles", "count", static_cast<int>(m_files.size()));
    for (size_t i = 0; i < m_files.size(); ++i) {
        std::string key = "file" + std::to_string(i);
        ini.set("RecentFiles", key, m_files[i]);
    }
}

void RecentFiles::loadFromIni(const IniFile& ini) {
    m_files.clear();
    int count = ini.getInt("RecentFiles", "count", 0);
    if (count <= 0) return;

    for (int i = 0; i < count; ++i) {
        std::string key = "file" + std::to_string(i);
        std::string path = ini.get("RecentFiles", key, "");
        if (!path.empty()) {
            addFile(path);
        }
    }
}
