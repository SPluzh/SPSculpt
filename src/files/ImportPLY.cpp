#include "files/ImportPLY.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include <sstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace ImportPLY {

struct Property {
    std::string type;
    std::string type2; // for lists
    std::string name;
    int offsetOctet = 0; // binary offset
    int id = 0;          // ascii token index
};

struct Element {
    std::string name;
    int count = 0;
    std::vector<Property> properties;
    std::unordered_map<std::string, Property> objProperties;
    int offsetOctet = 0; // total element binary size
};

static int typeToOctet(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
    if (type == "int" || type == "uint" || type == "float" || type == "int32" || type == "uint32" || type == "float32") return 4;
    if (type == "double" || type == "float64") return 8;
    return 0;
}

static float readBinaryValue(const uint8_t* buffer, size_t offset, const std::string& type, bool isFloat) {
    float fac = isFloat ? 1.0f / 255.0f : 1.0f;
    if (type == "char" || type == "int8") {
        int8_t val;
        std::memcpy(&val, buffer + offset, 1);
        return static_cast<float>(val) * fac;
    }
    if (type == "uchar" || type == "uint8") {
        uint8_t val;
        std::memcpy(&val, buffer + offset, 1);
        return static_cast<float>(val) * fac;
    }
    if (type == "short" || type == "int16") {
        int16_t val;
        std::memcpy(&val, buffer + offset, 2);
        return static_cast<float>(val) * fac;
    }
    if (type == "ushort" || type == "uint16") {
        uint16_t val;
        std::memcpy(&val, buffer + offset, 2);
        return static_cast<float>(val) * fac;
    }
    if (type == "int" || type == "int32") {
        int32_t val;
        std::memcpy(&val, buffer + offset, 4);
        return static_cast<float>(val) * fac;
    }
    if (type == "uint" || type == "uint32") {
        uint32_t val;
        std::memcpy(&val, buffer + offset, 4);
        return static_cast<float>(val) * fac;
    }
    if (type == "float" || type == "float32") {
        float val;
        std::memcpy(&val, buffer + offset, 4);
        return val;
    }
    if (type == "double" || type == "float64") {
        double val;
        std::memcpy(&val, buffer + offset, 8);
        return static_cast<float>(val);
    }
    return 0.0f;
}

static float parseAsciiValue(const std::string& token, const std::string& type, bool isFloat) {
    float fac = isFloat ? 1.0f / 255.0f : 1.0f;
    if (token.empty()) return 0.0f;
    if (type == "float" || type == "double" || type == "float32" || type == "float64") {
        return std::stof(token);
    }
    return static_cast<float>(std::stoi(token)) * fac;
}

static std::vector<std::string> splitByWhitespace(const std::string& str) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(str);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::vector<Mesh*> importPLY(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 10) return {};

    std::string asciiData(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    std::vector<std::string> lines;
    std::istringstream stream(asciiData);
    std::string line;
    
    // Parse header to find format, elements, properties and binary offset
    bool isBinary = false;
    std::vector<Element> elements;
    size_t headerBytes = 0;
    size_t lineIndex = 0;

    while (std::getline(stream, line)) {
        headerBytes += line.length() + 1; // +1 for newline character
        lineIndex++;

        // Trim
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, (last - first + 1));

        if (trimmed.rfind("format binary", 0) == 0) {
            isBinary = true;
        } else if (trimmed.rfind("element", 0) == 0) {
            auto split = splitByWhitespace(trimmed);
            if (split.size() >= 3) {
                Element el;
                el.name = split[1];
                el.count = std::stoi(split[2]);
                elements.push_back(el);
            }
        } else if (trimmed.rfind("property", 0) == 0) {
            auto split = splitByWhitespace(trimmed);
            if (split.size() >= 3 && !elements.empty()) {
                Property prop;
                bool isList = (split[1] == "list");
                prop.type = split[isList ? 2 : 1];
                if (isList && split.size() >= 5) {
                    prop.type2 = split[3];
                    prop.name = split[4];
                } else {
                    prop.name = split[2];
                }
                elements.back().properties.push_back(prop);
            }
        } else if (trimmed == "end_header") {
            break;
        }
    }

    // Prepare elements properties offsets and objProperties lookup maps
    for (auto& el : elements) {
        el.offsetOctet = 0;
        int idCount = 0;
        for (auto& prop : el.properties) {
            Property objProp = prop;
            if (isBinary) {
                objProp.offsetOctet = el.offsetOctet;
                el.offsetOctet += typeToOctet(prop.type);
            } else {
                objProp.id = idCount++;
            }
            el.objProperties[prop.name] = objProp;
        }
    }

    std::vector<float> outVerts;
    std::vector<float> outColors;
    std::vector<uint32_t> outFaces;

    size_t binaryOffset = headerBytes;
    size_t currentAsciiLine = lineIndex;

    for (const auto& el : elements) {
        if (el.name == "vertex") {
            outVerts.resize(el.count * 3, 0.0f);
            bool hasColors = (el.objProperties.count("red") > 0 || el.objProperties.count("green") > 0 || el.objProperties.count("blue") > 0);
            if (hasColors) {
                outColors.resize(el.count * 3, 1.0f);
            }

            auto getProp = [&](const std::string& name, Property& p) -> bool {
                auto it = el.objProperties.find(name);
                if (it != el.objProperties.end()) {
                    p = it->second;
                    return true;
                }
                return false;
            };

            Property propX, propY, propZ, propR, propG, propB;
            bool hasX = getProp("x", propX), hasY = getProp("y", propY), hasZ = getProp("z", propZ);
            bool hasR = getProp("red", propR), hasG = getProp("green", propG), hasB = getProp("blue", propB);

            if (isBinary) {
                size_t lenOctet = el.offsetOctet * el.count;
                if (binaryOffset + lenOctet <= buffer.size()) {
                    const uint8_t* binPtr = buffer.data() + binaryOffset;
                    for (int i = 0; i < el.count; ++i) {
                        size_t off = i * el.offsetOctet;
                        int id = i * 3;
                        if (hasX) outVerts[id] = readBinaryValue(binPtr, off + propX.offsetOctet, propX.type, true);
                        if (hasY) outVerts[id + 1] = readBinaryValue(binPtr, off + propY.offsetOctet, propY.type, true);
                        if (hasZ) outVerts[id + 2] = readBinaryValue(binPtr, off + propZ.offsetOctet, propZ.type, true);

                        if (hasColors) {
                            if (hasR) outColors[id] = readBinaryValue(binPtr, off + propR.offsetOctet, propR.type, true);
                            if (hasG) outColors[id + 1] = readBinaryValue(binPtr, off + propG.offsetOctet, propG.type, true);
                            if (hasB) outColors[id + 2] = readBinaryValue(binPtr, off + propB.offsetOctet, propB.type, true);
                        }
                    }
                    binaryOffset += lenOctet;
                }
            } else {
                // ASCII
                for (int i = 0; i < el.count; ++i) {
                    if (currentAsciiLine + i >= lines.size()) {
                        // Need to read more lines if we didn't read them all
                        while (currentAsciiLine + i >= lines.size() && std::getline(stream, line)) {
                            lines.push_back(line);
                        }
                    }
                    if (currentAsciiLine + i >= lines.size()) break;

                    std::string trimmed = lines[currentAsciiLine + i];
                    size_t f = trimmed.find_first_not_of(" \t\r\n");
                    if (f != std::string::npos) {
                        size_t l = trimmed.find_last_not_of(" \t\r\n");
                        trimmed = trimmed.substr(f, (l - f + 1));
                    }
                    auto split = splitByWhitespace(trimmed);
                    int id = i * 3;

                    if (hasX && propX.id < (int)split.size()) outVerts[id] = parseAsciiValue(split[propX.id], propX.type, true);
                    if (hasY && propY.id < (int)split.size()) outVerts[id + 1] = parseAsciiValue(split[propY.id], propY.type, true);
                    if (hasZ && propZ.id < (int)split.size()) outVerts[id + 2] = parseAsciiValue(split[propZ.id], propZ.type, true);

                    if (hasColors) {
                        if (hasR && propR.id < (int)split.size()) outColors[id] = parseAsciiValue(split[propR.id], propR.type, true);
                        if (hasG && propG.id < (int)split.size()) outColors[id + 1] = parseAsciiValue(split[propG.id], propG.type, true);
                        if (hasB && propB.id < (int)split.size()) outColors[id + 2] = parseAsciiValue(split[propB.id], propB.type, true);
                    }
                }
                currentAsciiLine += el.count;
            }
        } else if (el.name == "face") {
            outFaces.reserve(el.count * 4);
            auto it = el.objProperties.find("vertex_indices");
            if (it == el.objProperties.end()) {
                it = el.objProperties.find("vertex_index");
            }
            if (it == el.objProperties.end() && !el.properties.empty()) {
                // default to first property if name is different
                it = el.objProperties.find(el.properties[0].name);
            }

            if (it != el.objProperties.end()) {
                Property propIndex = it->second;
                if (isBinary) {
                    const uint8_t* binPtr = buffer.data();
                    size_t offsetCurrent = binaryOffset;
                    int octetCount = typeToOctet(propIndex.type);
                    int octetIndex = typeToOctet(propIndex.type2);

                    for (int i = 0; i < el.count; ++i) {
                        if (offsetCurrent + octetCount > buffer.size()) break;
                        int nbVert = static_cast<int>(readBinaryValue(binPtr, offsetCurrent, propIndex.type, false));
                        offsetCurrent += octetCount;

                        if (offsetCurrent + nbVert * octetIndex > buffer.size()) break;

                        if (nbVert == 3 || nbVert == 4) {
                            uint32_t iv1 = static_cast<uint32_t>(readBinaryValue(binPtr, offsetCurrent, propIndex.type2, false));
                            uint32_t iv2 = static_cast<uint32_t>(readBinaryValue(binPtr, offsetCurrent + octetIndex, propIndex.type2, false));
                            uint32_t iv3 = static_cast<uint32_t>(readBinaryValue(binPtr, offsetCurrent + 2 * octetIndex, propIndex.type2, false));
                            uint32_t iv4 = (nbVert == 3) ? TRI_INDEX : static_cast<uint32_t>(readBinaryValue(binPtr, offsetCurrent + 3 * octetIndex, propIndex.type2, false));
                            outFaces.push_back(iv1);
                            outFaces.push_back(iv2);
                            outFaces.push_back(iv3);
                            outFaces.push_back(iv4);
                        }
                        offsetCurrent += nbVert * octetIndex;
                    }
                    binaryOffset = offsetCurrent;
                } else {
                    // ASCII
                    for (int i = 0; i < el.count; ++i) {
                        if (currentAsciiLine + i >= lines.size()) {
                            while (currentAsciiLine + i >= lines.size() && std::getline(stream, line)) {
                                lines.push_back(line);
                            }
                        }
                        if (currentAsciiLine + i >= lines.size()) break;

                        std::string trimmed = lines[currentAsciiLine + i];
                        size_t f = trimmed.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos) {
                            size_t l = trimmed.find_last_not_of(" \t\r\n");
                            trimmed = trimmed.substr(f, (l - f + 1));
                        }
                        auto split = splitByWhitespace(trimmed);
                        if (split.empty()) continue;

                        int nbVert = std::stoi(split[0]);
                        if (nbVert == 3 || nbVert == 4) {
                            uint32_t iv1 = std::stoul(split[propIndex.id + 1]);
                            uint32_t iv2 = std::stoul(split[propIndex.id + 2]);
                            uint32_t iv3 = std::stoul(split[propIndex.id + 3]);
                            uint32_t iv4 = (nbVert == 3) ? TRI_INDEX : std::stoul(split[propIndex.id + 4]);
                            outFaces.push_back(iv1);
                            outFaces.push_back(iv2);
                            outFaces.push_back(iv3);
                            outFaces.push_back(iv4);
                        }
                    }
                    currentAsciiLine += el.count;
                }
            }
        } else {
            // skip element
            if (isBinary) {
                // If it has list properties or list elements we skip them by dry-running
                // But usually only face is list.
            } else {
                currentAsciiLine += el.count;
            }
        }
    }

    Mesh* mesh = new Mesh();
    mesh->verts = outVerts;
    mesh->faces = outFaces;
    mesh->nbVerts = outVerts.size() / 3;
    mesh->nbFaces = outFaces.size() / 4;

    if (!outColors.empty()) {
        mesh->colors = outColors;
    } else {
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
    }

    mesh->materials.resize(mesh->nbVerts * 3);
    for (int k = 0; k < mesh->nbVerts; ++k) {
        mesh->materials[k * 3]     = 0.5f; // roughness
        mesh->materials[k * 3 + 1] = 0.0f; // metalness
        mesh->materials[k * 3 + 2] = 1.0f; // mask
    }

    // Compute topology
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    
    computeTopology(
        mesh->nbVerts, mesh->faces.data(), mesh->nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );

    mesh->vrfStartCount = vrfStartCount;
    mesh->vertRingFace = vertRingFace;
    mesh->vrvStartCount = vrvStartCount;
    mesh->vertRingVert = vertRingVert;
    mesh->vertOnEdge = vertOnEdge;
    
    mesh->postInit();

    return {mesh};
}

} // namespace ImportPLY
