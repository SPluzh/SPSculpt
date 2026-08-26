#include "files/FileManager.h"
#include "files/ImportSGL.h"
#include "files/ExportSGL.h"
#include "files/ImportOBJ.h"
#include "files/ExportOBJ.h"
#include "files/ImportSTL.h"
#include "files/ExportSTL.h"
#include "files/ImportPLY.h"
#include "files/ExportPLY.h"
#include "files/ImportGLTF.h"
#include "files/ExportGLTF.h"
#include "common/FormatConstants.h"
#include "common/StringUtils.h"
#include "common/Logger.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <chrono>

static std::vector<uint8_t> readBinaryFile(const std::string& path) {
#ifdef _WIN32
    std::ifstream file(utf8ToWide(path).c_str(), std::ios::binary | std::ios::ate);
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) {
        std::cerr << "Failed to open file for reading: " << path << std::endl;
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }
    return {};
}

static std::string readTextFile(const std::string& path) {
#ifdef _WIN32
    std::ifstream file(utf8ToWide(path).c_str());
#else
    std::ifstream file(path);
#endif
    if (!file.is_open()) {
        std::cerr << "Failed to open file for reading: " << path << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static bool writeBinaryFile(const std::string& path, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    std::ofstream file(utf8ToWide(path).c_str(), std::ios::binary);
#else
    std::ofstream file(path, std::ios::binary);
#endif
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << path << std::endl;
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

static bool writeTextFile(const std::string& path, const std::string& data) {
#ifdef _WIN32
    std::ofstream file(utf8ToWide(path).c_str());
#else
    std::ofstream file(path);
#endif
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << path << std::endl;
        return false;
    }
    file << data;
    return true;
}

std::string FileManager::getExtension(const std::string& path) {
    size_t dotIdx = path.find_last_of('.');
    if (dotIdx == std::string::npos) return "";
    std::string ext = path.substr(dotIdx + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::vector<Mesh*> FileManager::importFiles(const std::string& path,
                                           Scene* scene,
                                           AngleRenderer* renderer,
                                           SculptManager* sculpt,
                                           uint64_t* outWorkTime) {
    std::string ext = getExtension(path);
    
    if (ext == Format::PROJECT_EXT || ext == Format::LEGACY_EXT) {
        if (!scene || !renderer) {
            std::cerr << "Importing project requires Scene and Renderer pointers" << std::endl;
            return {};
        }
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) return {};
        return ImportSGL::importSGL(buffer, *scene, *renderer, sculpt, outWorkTime);
    }
    
    if (ext == "obj") {
        auto tStart = std::chrono::high_resolution_clock::now();
        sculpt_log("[OBJ Import] Starting OBJ file import: '%s'\n", path.c_str());

        auto tReadStart = std::chrono::high_resolution_clock::now();
        std::string data = readTextFile(path);
        auto tReadEnd = std::chrono::high_resolution_clock::now();
        double msRead = std::chrono::duration<double, std::milli>(tReadEnd - tReadStart).count();

        if (data.empty()) {
            sculpt_log("[OBJ Import] ERROR: Failed to read file or file is empty: '%s'\n", path.c_str());
            return {};
        }

        double dataMb = static_cast<double>(data.size()) / (1024.0 * 1024.0);
        sculpt_log("[OBJ Import] Disk read completed: %.2f ms (File Size: %.2f MB)\n", msRead, dataMb);

        auto tParseStart = std::chrono::high_resolution_clock::now();
        std::vector<Mesh*> meshes = ImportOBJ::importOBJ(data);
        auto tEnd = std::chrono::high_resolution_clock::now();
        double msParse = std::chrono::duration<double, std::milli>(tEnd - tParseStart).count();
        double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        sculpt_log("[OBJ Import] FileManager::importFiles completed in %.2f ms (Disk I/O: %.2f ms, Parse/Build: %.2f ms, Meshes: %zu)\n",
                   msTotal, msRead, msParse, meshes.size());
        return meshes;
    }
    
    if (ext == "stl") {
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) return {};
        return ImportSTL::importSTL(buffer);
    }
    
    if (ext == "ply") {
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) return {};
        return ImportPLY::importPLY(buffer);
    }
    
    if (ext == "glb") {
        auto tStart = std::chrono::high_resolution_clock::now();
        sculpt_log("[GLTF Import] Starting GLB file import: '%s'\n", path.c_str());
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) {
            sculpt_log("[GLTF Import ERROR] Failed to read GLB binary file or file is empty: '%s'\n", path.c_str());
            return {};
        }
        double dataMb = static_cast<double>(buffer.size()) / (1024.0 * 1024.0);
        sculpt_log("[GLTF Import] Disk read completed: %.2f MB\n", dataMb);

        auto tParseStart = std::chrono::high_resolution_clock::now();
        std::vector<Mesh*> meshes = ImportGLTF::importGLB(buffer);
        auto tEnd = std::chrono::high_resolution_clock::now();
        double msParse = std::chrono::duration<double, std::milli>(tEnd - tParseStart).count();
        double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        sculpt_log("[GLTF Import] FileManager::importFiles completed GLB import in %.2f ms (Parse/Build: %.2f ms, Meshes: %zu)\n",
                   msTotal, msParse, meshes.size());
        return meshes;
    }
    
    if (ext == "gltf") {
        auto tStart = std::chrono::high_resolution_clock::now();
        sculpt_log("[GLTF Import] Starting GLTF file import: '%s'\n", path.c_str());
        std::string data = readTextFile(path);
        if (data.empty()) {
            sculpt_log("[GLTF Import ERROR] Failed to read GLTF text file or file is empty: '%s'\n", path.c_str());
            return {};
        }
        double dataMb = static_cast<double>(data.size()) / (1024.0 * 1024.0);
        sculpt_log("[GLTF Import] Disk read completed: %.2f MB\n", dataMb);

        std::string basePath;
        size_t slashIdx = path.find_last_of("/\\");
        if (slashIdx != std::string::npos)
            basePath = path.substr(0, slashIdx);

        auto tParseStart = std::chrono::high_resolution_clock::now();
        std::vector<Mesh*> meshes = ImportGLTF::importGLTF(data, basePath);
        auto tEnd = std::chrono::high_resolution_clock::now();
        double msParse = std::chrono::duration<double, std::milli>(tEnd - tParseStart).count();
        double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        sculpt_log("[GLTF Import] FileManager::importFiles completed GLTF import in %.2f ms (Parse/Build: %.2f ms, Meshes: %zu)\n",
                   msTotal, msParse, meshes.size());
        return meshes;
    }
    
    std::cerr << "Unsupported import file format: ." << ext << std::endl;
    return {};
}

bool FileManager::exportMeshes(const std::string& path,
                               const std::vector<Mesh*>& meshes,
                               const Scene* scene,
                               const AngleRenderer* renderer,
                               const SculptManager* sculpt,
                               const std::vector<uint8_t>& thumbnail,
                               bool savePngNextToProject,
                               uint64_t workTimeSeconds) {
    std::string ext = getExtension(path);
    
    if (ext == Format::PROJECT_EXT || ext == Format::LEGACY_EXT) {
        if (!scene || !renderer || !sculpt) {
            sculpt_log("[FileManager Export ERROR] Exporting project requires Scene, Renderer, and Sculpt pointers\n");
            return false;
        }
        sculpt_log("[FileManager Export] Exporting scene to '%s' | Meshes: %zu | Thumbnail size: %zu bytes | Work time: %llu s\n", path.c_str(), meshes.size(), thumbnail.size(), static_cast<unsigned long long>(workTimeSeconds));
        std::vector<uint8_t> buffer = ExportSGL::exportSGL(meshes, *scene, *renderer, *sculpt, thumbnail, workTimeSeconds);
        if (buffer.empty()) {
            sculpt_log("[FileManager Export ERROR] ExportSGL returned empty binary buffer!\n");
            return false;
        }
        bool success = writeBinaryFile(path, buffer);
        if (success) {
            sculpt_log("[FileManager Export] SUCCESS | Written %zu bytes to '%s'\n", buffer.size(), path.c_str());
            if (savePngNextToProject && !thumbnail.empty()) {
                std::string pngPath = path;
                size_t dotPos = pngPath.find_last_of('.');
                if (dotPos != std::string::npos) {
                    pngPath = pngPath.substr(0, dotPos) + ".png";
                } else {
                    pngPath += ".png";
                }
                if (writeBinaryFile(pngPath, thumbnail)) {
                    sculpt_log("[FileManager Export] Saved preview image next to project: '%s'\n", pngPath.c_str());
                }
            }
        } else {
            sculpt_log("[FileManager Export ERROR] Failed to write binary file '%s'\n", path.c_str());
        }
        return success;
    }
    
    if (ext == "obj") {
        std::string data = ExportOBJ::exportOBJ(meshes);
        if (data.empty()) return false;
        return writeTextFile(path, data);
    }
    
    if (ext == "stl") {
        // Export as binary STL by default
        std::vector<uint8_t> buffer = ExportSTL::exportBinarySTL(meshes);
        if (buffer.empty()) return false;
        return writeBinaryFile(path, buffer);
    }
    
    if (ext == "ply") {
        // Export as binary PLY by default
        std::vector<uint8_t> buffer = ExportPLY::exportBinaryPLY(meshes);
        if (buffer.empty()) return false;
        return writeBinaryFile(path, buffer);
    }
    
    if (ext == "glb" || ext == "gltf") {
        std::vector<uint8_t> buffer = ExportGLTF::exportGLB(meshes);
        if (buffer.empty()) return false;
        return writeBinaryFile(path, buffer);
    }
    
    std::cerr << "Unsupported export file format: ." << ext << std::endl;
    return false;
}
