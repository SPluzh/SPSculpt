#pragma once
#include <string>
#include <vector>

namespace FileDialog {

struct FilterSpec {
    std::string name;    // e.g. "SPSculpt Project (*.spsculpt)"
    std::string pattern; // e.g. "*.spsculpt" or "*.spsculpt;*.obj"
};

struct SaveFileOptions {
    bool showCheckbox = false;
    std::string checkboxLabel;
    bool checked = false;
};

// Open file dialog: returns selected path, or empty string if canceled
std::string openFile(const std::vector<FilterSpec>& filters = {}, const std::string& title = "Open File");

// Save file dialog: returns selected path, or empty string if canceled
std::string saveFile(const std::vector<FilterSpec>& filters = {}, const std::string& defaultExt = "", const std::string& title = "Save File", SaveFileOptions* options = nullptr);

// Folder picker dialog: returns selected directory with trailing '/', or empty string if canceled
std::string pickFolder(const std::string& title = "Select ZBrush Exchange Directory");

// Standard filters for importing and exporting 3D assets
std::vector<FilterSpec> getImportFilters();
std::vector<FilterSpec> getExportFilters();

} // namespace FileDialog
