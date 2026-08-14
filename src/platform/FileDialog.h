#pragma once
#include <string>
#include <vector>

namespace FileDialog {

struct FilterSpec {
    std::string name;    // e.g. "SPSculpt Project (*.spsculpt)"
    std::string pattern; // e.g. "*.spsculpt" or "*.spsculpt;*.obj"
};

// Open file dialog: returns selected path, or empty string if canceled
std::string openFile(const std::vector<FilterSpec>& filters = {}, const std::string& title = "Open File");

// Save file dialog: returns selected path, or empty string if canceled
std::string saveFile(const std::vector<FilterSpec>& filters = {}, const std::string& defaultExt = "", const std::string& title = "Save File");

// Standard filters for importing and exporting 3D assets
std::vector<FilterSpec> getImportFilters();
std::vector<FilterSpec> getExportFilters();

} // namespace FileDialog
