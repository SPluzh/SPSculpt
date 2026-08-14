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
                                           SculptManager* sculpt) {
    std::string ext = getExtension(path);
    
    if (ext == "sgl") {
        if (!scene || !renderer) {
            std::cerr << "Importing SGL requires Scene and Renderer pointers" << std::endl;
            return {};
        }
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) return {};
        return ImportSGL::importSGL(buffer, *scene, *renderer, sculpt);
    }
    
    if (ext == "obj") {
        std::string data = readTextFile(path);
        if (data.empty()) return {};
        return ImportOBJ::importOBJ(data);
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
        std::vector<uint8_t> buffer = readBinaryFile(path);
        if (buffer.empty()) return {};
        return ImportGLTF::importGLB(buffer);
    }
    
    if (ext == "gltf") {
        std::string data = readTextFile(path);
        if (data.empty()) return {};
        return ImportGLTF::importGLTF(data);
    }
    
    std::cerr << "Unsupported import file format: ." << ext << std::endl;
    return {};
}

bool FileManager::exportMeshes(const std::string& path,
                               const std::vector<Mesh*>& meshes,
                               const Scene* scene,
                               const AngleRenderer* renderer,
                               const SculptManager* sculpt) {
    std::string ext = getExtension(path);
    
    if (ext == "sgl") {
        if (!scene || !renderer || !sculpt) {
            std::cerr << "Exporting SGL requires Scene, Renderer, and Sculpt pointers" << std::endl;
            return false;
        }
        std::vector<uint8_t> buffer = ExportSGL::exportSGL(meshes, *scene, *renderer, *sculpt);
        if (buffer.empty()) return false;
        return writeBinaryFile(path, buffer);
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
