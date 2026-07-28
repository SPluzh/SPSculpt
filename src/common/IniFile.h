#ifndef INIFILE_H
#define INIFILE_H

#include <string>
#include <vector>
#include <unordered_map>

class IniFile {
public:
    IniFile() = default;

    bool load(const std::string& filepath);
    bool save(const std::string& filepath) const;

    std::string get(const std::string& section, const std::string& key, const std::string& defaultVal = "") const;
    int getInt(const std::string& section, const std::string& key, int defaultVal = 0) const;
    float getFloat(const std::string& section, const std::string& key, float defaultVal = 0.0f) const;
    bool getBool(const std::string& section, const std::string& key, bool defaultVal = false) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    void setInt(const std::string& section, const std::string& key, int value);
    void setFloat(const std::string& section, const std::string& key, float value);
    void setBool(const std::string& section, const std::string& key, bool value);

    bool hasSection(const std::string& section) const;
    bool hasKey(const std::string& section, const std::string& key) const;

private:
    std::vector<std::string> m_sectionOrder;
    std::unordered_map<std::string, std::vector<std::string>> m_keyOrder;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_data;
};

#endif // INIFILE_H
