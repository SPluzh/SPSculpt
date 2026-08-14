#include "timelapse/TimelapseSerializer.h"
#include "common/Logger.h"
#include "common/StringUtils.h"
#include <fstream>
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)
struct StlapseHeader {
    char magic[4] = {'S', 'T', 'L', 'P'};
    uint32_t version = 1;
    uint32_t flags = 0;
    uint32_t stepCount = 0;
    uint32_t metaSize = 0;
    uint32_t initialStateSize = 0;
    uint64_t reserved = 0;
};
#pragma pack(pop)

template<typename T>
static void writePOD(std::ostream& os, const T& val) {
    os.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

template<typename T>
static void readPOD(std::istream& is, T& val) {
    is.read(reinterpret_cast<char*>(&val), sizeof(T));
}

template<typename T>
static void writeVector(std::ostream& os, const std::vector<T>& vec) {
    uint64_t sz = static_cast<uint64_t>(vec.size());
    writePOD(os, sz);
    if (sz > 0) {
        os.write(reinterpret_cast<const char*>(vec.data()), sz * sizeof(T));
    }
}

template<typename T>
static void readVector(std::istream& is, std::vector<T>& vec) {
    uint64_t sz = 0;
    readPOD(is, sz);
    vec.resize(sz);
    if (sz > 0) {
        is.read(reinterpret_cast<char*>(vec.data()), sz * sizeof(T));
    }
}

static void writeString(std::ostream& os, const std::string& str) {
    uint32_t len = static_cast<uint32_t>(str.size());
    writePOD(os, len);
    if (len > 0) {
        os.write(str.data(), len);
    }
}

static void readString(std::istream& is, std::string& str) {
    uint32_t len = 0;
    readPOD(is, len);
    str.resize(len);
    if (len > 0) {
        is.read(&str[0], len);
    }
}

void TimelapseSerializer::writeMeshState(std::ostream& os, const MeshState& ms) {
    writePOD(os, ms.id);
    writeString(os, ms.outlinerName);
    writePOD(os, ms.visibleV1);
    writePOD(os, ms.visibleV2);
    writePOD(os, ms.nbVerts);
    writePOD(os, ms.nbFaces);
    writePOD(os, ms.matrix);

    writeVector(os, ms.verts);
    writeVector(os, ms.colors);
    writeVector(os, ms.materials);
    writeVector(os, ms.faces);
    writeVector(os, ms.faceGroups);
    writeVector(os, ms.vrfStartCount);
    writeVector(os, ms.vertRingFace);
    writeVector(os, ms.vrvStartCount);
    writeVector(os, ms.vertRingVert);
    writeVector(os, ms.vertOnEdge);
    writeVector(os, ms.vertVisible);
    writeVector(os, ms.faceVisible);
}

void TimelapseSerializer::readMeshState(std::istream& is, MeshState& ms) {
    readPOD(is, ms.id);
    readString(is, ms.outlinerName);
    readPOD(is, ms.visibleV1);
    readPOD(is, ms.visibleV2);
    readPOD(is, ms.nbVerts);
    readPOD(is, ms.nbFaces);
    readPOD(is, ms.matrix);

    readVector(is, ms.verts);
    readVector(is, ms.colors);
    readVector(is, ms.materials);
    readVector(is, ms.faces);
    readVector(is, ms.faceGroups);
    readVector(is, ms.vrfStartCount);
    readVector(is, ms.vertRingFace);
    readVector(is, ms.vrvStartCount);
    readVector(is, ms.vertRingVert);
    readVector(is, ms.vertOnEdge);
    readVector(is, ms.vertVisible);
    readVector(is, ms.faceVisible);
}

void TimelapseSerializer::writeHistoryState(std::ostream& os, const HistoryState& hs) {
    writePOD(os, hs.selectedMeshIdx);
    writeVector(os, hs.selectedMeshIndices);
    uint32_t count = static_cast<uint32_t>(hs.meshes.size());
    writePOD(os, count);
    for (const auto& ms : hs.meshes) {
        writeMeshState(os, ms);
    }
}

void TimelapseSerializer::readHistoryState(std::istream& is, HistoryState& hs) {
    readPOD(is, hs.selectedMeshIdx);
    readVector(is, hs.selectedMeshIndices);
    uint32_t count = 0;
    readPOD(is, count);
    hs.meshes.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        readMeshState(is, hs.meshes[i]);
    }
}

void TimelapseSerializer::writeVertexDelta(std::ostream& os, const VertexDelta& delta) {
    writePOD(os, delta.meshId);
    writePOD(os, delta.hasColors);
    writePOD(os, delta.hasMaterials);
    writeVector(os, delta.indices);
    writeVector(os, delta.prevVerts);
    writeVector(os, delta.prevColors);
    writeVector(os, delta.prevMaterials);
    writeVector(os, delta.nextVerts);
    writeVector(os, delta.nextColors);
    writeVector(os, delta.nextMaterials);
}

void TimelapseSerializer::readVertexDelta(std::istream& is, VertexDelta& delta) {
    readPOD(is, delta.meshId);
    readPOD(is, delta.hasColors);
    readPOD(is, delta.hasMaterials);
    readVector(is, delta.indices);
    readVector(is, delta.prevVerts);
    readVector(is, delta.prevColors);
    readVector(is, delta.prevMaterials);
    readVector(is, delta.nextVerts);
    readVector(is, delta.nextColors);
    readVector(is, delta.nextMaterials);
}

bool TimelapseSerializer::saveToFile(
    const std::string& filepath,
    const HistoryState& initialState,
    const std::vector<std::unique_ptr<UndoEntry>>& timeline,
    const TimelapseMetadata& metadata)
{
#ifdef _WIN32
    std::ofstream os(utf8ToWide(filepath).c_str(), std::ios::binary);
#else
    std::ofstream os(filepath, std::ios::binary);
#endif
    if (!os.is_open()) {
        sculpt_log("[TimelapseSerializer] Failed to open file for writing: %s\n", filepath.c_str());
        return false;
    }

    StlapseHeader hdr;
    hdr.stepCount = static_cast<uint32_t>(timeline.size());

    auto headerPos = os.tellp();
    os.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    auto metaPos = os.tellp();
    writeString(os, metadata.title);
    writeString(os, metadata.author);
    writeString(os, metadata.creationDate);
    writeString(os, metadata.appVersion);
    writePOD(os, metadata.totalStrokes);
    hdr.metaSize = static_cast<uint32_t>(os.tellp() - metaPos);

    auto statePos = os.tellp();
    writeHistoryState(os, initialState);
    hdr.initialStateSize = static_cast<uint32_t>(os.tellp() - statePos);

    for (const auto& entry : timeline) {
        if (!entry) continue;

        uint8_t tag = 0;
        switch (entry->getType()) {
            case UndoEntryType::Sculpt: tag = 0x01; break;
            case UndoEntryType::Topology: tag = 0x02; break;
            case UndoEntryType::SceneMeta: tag = 0x03; break;
        }

        writePOD(os, tag);

        if (tag == 0x01) {
            auto* sculptEntry = dynamic_cast<SculptUndoEntry*>(entry.get());
            if (sculptEntry) {
                writeString(os, sculptEntry->description);
                uint32_t dCount = static_cast<uint32_t>(sculptEntry->deltas.size());
                writePOD(os, dCount);
                for (const auto& d : sculptEntry->deltas) {
                    writeVertexDelta(os, d);
                }
            }
        } else if (tag == 0x02) {
            auto* topoEntry = dynamic_cast<TopologyUndoEntry*>(entry.get());
            if (topoEntry) {
                writeString(os, topoEntry->description);
                writeHistoryState(os, topoEntry->before);
                writeHistoryState(os, topoEntry->after);
            }
        } else if (tag == 0x03) {
            auto* metaEntry = dynamic_cast<SceneMetaUndoEntry*>(entry.get());
            if (metaEntry) {
                writeString(os, metaEntry->description);
                writeHistoryState(os, metaEntry->before);
                writeHistoryState(os, metaEntry->after);
            }
        }
    }

    os.seekp(headerPos);
    os.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    sculpt_log("[TimelapseSerializer] Successfully saved timelapse to %s (%u steps)\n", filepath.c_str(), hdr.stepCount);
    return true;
}

bool TimelapseSerializer::loadFromFile(
    const std::string& filepath,
    HistoryState& outInitialState,
    std::vector<std::unique_ptr<UndoEntry>>& outTimeline,
    TimelapseMetadata& outMetadata)
{
#ifdef _WIN32
    std::ifstream is(utf8ToWide(filepath).c_str(), std::ios::binary);
#else
    std::ifstream is(filepath, std::ios::binary);
#endif
    if (!is.is_open()) {
        sculpt_log("[TimelapseSerializer] Failed to open file for reading: %s\n", filepath.c_str());
        return false;
    }

    StlapseHeader hdr;
    is.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

    if (std::memcmp(hdr.magic, "STLP", 4) != 0) {
        sculpt_log("[TimelapseSerializer] Invalid magic header in file: %s\n", filepath.c_str());
        return false;
    }

    readString(is, outMetadata.title);
    readString(is, outMetadata.author);
    readString(is, outMetadata.creationDate);
    readString(is, outMetadata.appVersion);
    readPOD(is, outMetadata.totalStrokes);

    readHistoryState(is, outInitialState);

    outTimeline.clear();
    outTimeline.reserve(hdr.stepCount);

    for (uint32_t i = 0; i < hdr.stepCount; ++i) {
        uint8_t tag = 0;
        readPOD(is, tag);
        if (!is) break;

        if (tag == 0x01) {
            auto sculptEntry = std::make_unique<SculptUndoEntry>();
            readString(is, sculptEntry->description);
            uint32_t dCount = 0;
            readPOD(is, dCount);
            sculptEntry->deltas.resize(dCount);
            for (uint32_t d = 0; d < dCount; ++d) {
                readVertexDelta(is, sculptEntry->deltas[d]);
            }
            outTimeline.push_back(std::move(sculptEntry));
        } else if (tag == 0x02) {
            auto topoEntry = std::make_unique<TopologyUndoEntry>();
            readString(is, topoEntry->description);
            readHistoryState(is, topoEntry->before);
            readHistoryState(is, topoEntry->after);
            outTimeline.push_back(std::move(topoEntry));
        } else if (tag == 0x03) {
            auto metaEntry = std::make_unique<SceneMetaUndoEntry>();
            readString(is, metaEntry->description);
            readHistoryState(is, metaEntry->before);
            readHistoryState(is, metaEntry->after);
            outTimeline.push_back(std::move(metaEntry));
        }
    }

    sculpt_log("[TimelapseSerializer] Successfully loaded timelapse from %s (%zu steps)\n", filepath.c_str(), outTimeline.size());
    return true;
}
