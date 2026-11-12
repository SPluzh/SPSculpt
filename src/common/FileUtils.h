#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace FileUtils {

// Returns the directory containing the running executable (UTF-8, no trailing separator).
std::string getExecutableDir();

// Atomically writes binary data to `path` by writing to a temporary file and renaming it.
bool writeBinaryFileAtomic(const std::string& path, const uint8_t* data, size_t size);
inline bool writeBinaryFileAtomic(const std::string& path, const std::vector<uint8_t>& data) {
    return writeBinaryFileAtomic(path, data.data(), data.size());
}

// Atomically writes text data to `path` by writing to a temporary file and renaming it.
bool writeTextFileAtomic(const std::string& path, const std::string& data);

} // namespace FileUtils

