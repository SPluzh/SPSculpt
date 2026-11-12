#include "common/FileUtils.h"
#include "common/StringUtils.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace FileUtils {

namespace {

std::filesystem::path toFsPath(const std::string& utf8Path) {
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(utf8Path));
#else
    return std::filesystem::path(utf8Path);
#endif
}

std::filesystem::path makeTempPath(const std::filesystem::path& targetPath) {
#ifdef _WIN32
    return std::filesystem::path(targetPath.wstring() + L".tmp");
#else
    return std::filesystem::path(targetPath.string() + ".tmp");
#endif
}

bool writeAllAtomic(const std::string& path, const char* data, size_t size, std::ios::openmode mode) {
    if (path.empty()) return false;

    const std::filesystem::path targetPath = toFsPath(path);
    std::error_code ec;
    if (targetPath.has_parent_path()) {
        std::filesystem::create_directories(targetPath.parent_path(), ec);
        ec.clear();
    }

    const std::filesystem::path tempPath = makeTempPath(targetPath);

    {
        std::ofstream out(tempPath, mode | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "Failed to open temporary file for writing: " << path << std::endl;
            return false;
        }
        if (size > 0 && data != nullptr) {
            out.write(data, static_cast<std::streamsize>(size));
        }
        out.flush();
        if (!out.good()) {
            std::cerr << "Failed while writing temporary file: " << path << std::endl;
            out.close();
            std::error_code rmEc;
            std::filesystem::remove(tempPath, rmEc);
            return false;
        }
    }

    std::filesystem::rename(tempPath, targetPath, ec);
    if (ec) {
        std::cerr << "Failed to atomically rename temporary file to " << path
                  << ": " << ec.message() << std::endl;
        std::error_code rmEc;
        std::filesystem::remove(tempPath, rmEc);
        return false;
    }

    return true;
}

} // namespace

std::string getExecutableDir() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::filesystem::path exePath(buf);
    return wideToUtf8(exePath.parent_path().wstring());
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return {};
    }
    buf[len] = '\0';
    return std::filesystem::path(buf).parent_path().string();
#endif
}

bool writeBinaryFileAtomic(const std::string& path, const uint8_t* data, size_t size) {
    return writeAllAtomic(path, reinterpret_cast<const char*>(data), size, std::ios::binary);
}

bool writeTextFileAtomic(const std::string& path, const std::string& data) {
    return writeAllAtomic(path, data.data(), data.size(), std::ios::out);
}

} // namespace FileUtils

