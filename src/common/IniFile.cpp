#include "common/IniFile.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0);
    if (count <= 0) return L"";
    std::wstring wstr(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &wstr[0], count);
    return wstr;
}
#endif

static std::string trimStr(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool IniFile::load(const std::string& filepath) {
#ifdef _WIN32
    std::ifstream in(utf8ToWide(filepath).c_str());
#else
    std::ifstream in(filepath);
#endif
    if (!in.is_open()) {
        return false;
    }

    m_sectionOrder.clear();
    m_keyOrder.clear();
    m_data.clear();

    std::string line;
    std::string currentSection = "";

    while (std::getline(in, line)) {
        line = trimStr(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[' && line.back() == ']') {
            currentSection = trimStr(line.substr(1, line.size() - 2));
            if (m_data.find(currentSection) == m_data.end()) {
                m_sectionOrder.push_back(currentSection);
                m_data[currentSection] = {};
                m_keyOrder[currentSection] = {};
            }
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos && !currentSection.empty()) {
            std::string key = trimStr(line.substr(0, eqPos));
            std::string val = trimStr(line.substr(eqPos + 1));
            
            auto& keys = m_keyOrder[currentSection];
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                keys.push_back(key);
            }
            m_data[currentSection][key] = val;
        }
    }
    return true;
}

bool IniFile::save(const std::string& filepath) const {
#ifdef _WIN32
    std::ofstream out(utf8ToWide(filepath).c_str());
#else
    std::ofstream out(filepath);
#endif
    if (!out.is_open()) {
        std::cerr << "Failed to open INI file for writing: " << filepath << std::endl;
        return false;
    }

    for (const auto& sec : m_sectionOrder) {
        auto secIt = m_data.find(sec);
        if (secIt == m_data.end()) continue;

        out << "[" << sec << "]\n";
        auto keyOrderIt = m_keyOrder.find(sec);
        if (keyOrderIt != m_keyOrder.end()) {
            for (const auto& key : keyOrderIt->second) {
                auto valIt = secIt->second.find(key);
                if (valIt != secIt->second.end()) {
                    out << key << "=" << valIt->second << "\n";
                }
            }
        }
        out << "\n";
    }

    return true;
}

bool IniFile::hasSection(const std::string& section) const {
    return m_data.find(section) != m_data.end();
}

bool IniFile::hasKey(const std::string& section, const std::string& key) const {
    auto secIt = m_data.find(section);
    if (secIt == m_data.end()) return false;
    return secIt->second.find(key) != secIt->second.end();
}

std::string IniFile::get(const std::string& section, const std::string& key, const std::string& defaultVal) const {
    auto secIt = m_data.find(section);
    if (secIt == m_data.end()) return defaultVal;
    auto keyIt = secIt->second.find(key);
    if (keyIt == secIt->second.end()) return defaultVal;
    return keyIt->second;
}

int IniFile::getInt(const std::string& section, const std::string& key, int defaultVal) const {
    std::string str = get(section, key, "");
    if (str.empty()) return defaultVal;
    try {
        return std::stoi(str);
    } catch (...) {
        return defaultVal;
    }
}

float IniFile::getFloat(const std::string& section, const std::string& key, float defaultVal) const {
    std::string str = get(section, key, "");
    if (str.empty()) return defaultVal;
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

bool IniFile::getBool(const std::string& section, const std::string& key, bool defaultVal) const {
    std::string str = get(section, key, "");
    if (str.empty()) return defaultVal;
    return (str == "true" || str == "1" || str == "TRUE" || str == "True");
}

void IniFile::set(const std::string& section, const std::string& key, const std::string& value) {
    if (m_data.find(section) == m_data.end()) {
        m_sectionOrder.push_back(section);
        m_data[section] = {};
        m_keyOrder[section] = {};
    }
    auto& keys = m_keyOrder[section];
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(key);
    }
    m_data[section][key] = value;
}

void IniFile::setInt(const std::string& section, const std::string& key, int value) {
    set(section, key, std::to_string(value));
}

void IniFile::setFloat(const std::string& section, const std::string& key, float value) {
    set(section, key, std::to_string(value));
}

void IniFile::setBool(const std::string& section, const std::string& key, bool value) {
    set(section, key, value ? "true" : "false");
}
