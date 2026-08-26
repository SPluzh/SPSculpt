#pragma once

#include <string>
#include <vector>
#include "common/IniFile.h"

class RecentFiles {
public:
    static constexpr int MAX_FILES = 20;

    void addFile(const std::string& path);
    void removeFile(const std::string& path);
    const std::vector<std::string>& getFiles() const { return m_files; }
    void clear() { m_files.clear(); }

    void saveToIni(IniFile& ini) const;
    void loadFromIni(const IniFile& ini);

private:
    static std::string normalizePath(const std::string& path);
    std::vector<std::string> m_files;
};
