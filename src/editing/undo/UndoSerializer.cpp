#include "editing/undo/UndoSerializer.h"
#include "editing/undo/SculptUndoEntry.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "editing/undo/DynamicTopologyUndoEntry.h"
#include "editing/undo/SceneMetaUndoEntry.h"
#include "editing/undo/TransformUndoEntry.h"
#include "editing/undo/MeasurementUndoEntry.h"
#include "common/Logger.h"

#include <lz4.h>
#include <omp.h>
#include <cstring>
#include <algorithm>
#include <limits>

namespace UndoSerializer {

namespace {

constexpr char MAGIC[4] = {'U', 'N', 'D', 'O'};
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t LZ4_MIN_BYTES = 256;

enum class EntryTag : uint8_t {
    Sculpt          = 1,
    Topology        = 2,
    DynamicTopology = 3,
    SceneMeta       = 4,
    Transform       = 5,
    Measurement     = 6
};

class BufWriter {
public:
    std::vector<uint8_t> buf;

    template <typename T>
    void writePOD(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

    void writeBytes(const void* data, size_t n) {
        if (n == 0) return;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + n);
    }

    void writeString(const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        writePOD(len);
        if (len > 0) writeBytes(s.data(), len);
    }

    template <typename T>
    void writeVector(const std::vector<T>& v) {
        uint64_t n = static_cast<uint64_t>(v.size());
        writePOD(n);
        if (n > 0) writeBytes(v.data(), static_cast<size_t>(n) * sizeof(T));
    }
};

class BufReader {
public:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;
    bool ok = true;

    BufReader(const uint8_t* d, size_t s, size_t off = 0)
        : data(d), size(s), offset(off) {}

    bool ensure(size_t n) {
        if (!ok || offset > size || n > size - offset) {
            ok = false;
            return false;
        }
        return true;
    }

    template <typename T>
    void readPOD(T& v) {
        if (!ensure(sizeof(T))) return;
        std::memcpy(&v, data + offset, sizeof(T));
        offset += sizeof(T);
    }

    void readBytes(void* dst, size_t n) {
        if (n == 0) return;
        if (!ensure(n)) return;
        std::memcpy(dst, data + offset, n);
        offset += n;
    }

    void readString(std::string& s) {
        uint32_t len = 0;
        readPOD(len);
        if (!ok) return;
        if (!ensure(len)) return;
        s.resize(len);
        if (len > 0) {
            std::memcpy(s.data(), data + offset, len);
            offset += len;
        }
    }

    template <typename T>
    void readVector(std::vector<T>& v) {
        uint64_t n = 0;
        readPOD(n);
        if (!ok) return;
        if (n > (size - offset) / sizeof(T)) {
            ok = false;
            return;
        }
        v.resize(static_cast<size_t>(n));
        if (n > 0) {
            size_t bytes = static_cast<size_t>(n) * sizeof(T);
            std::memcpy(v.data(), data + offset, bytes);
            offset += bytes;
        }
    }
};

// --- Helpers for nested structs ---

void writeMeasurementAnchor(BufWriter& w, const MeasurementAnchor& a) {
    uint32_t type = static_cast<uint32_t>(a.type);
    w.writePOD(type);
    w.writePOD(a.meshId);
    w.writePOD(a.vertIdx);
    w.writePOD(a.worldPos);
}

void readMeasurementAnchor(BufReader& r, MeasurementAnchor& a, const std::unordered_map<uint32_t, Mesh*>& meshIdMap) {
    uint32_t type = 0;
    r.readPOD(type);
    a.type = (type == static_cast<uint32_t>(MeasurementAnchor::VERTEX))
        ? MeasurementAnchor::VERTEX
        : MeasurementAnchor::FREE;
    r.readPOD(a.meshId);
    r.readPOD(a.vertIdx);
    r.readPOD(a.worldPos);
    a.mesh = nullptr;
    if (a.type == MeasurementAnchor::VERTEX && a.meshId != 0) {
        auto it = meshIdMap.find(a.meshId);
        if (it != meshIdMap.end()) a.mesh = it->second;
    }
}

void writeMeasurementSegments(BufWriter& w, const std::vector<MeasurementSegment>& segs) {
    uint32_t count = static_cast<uint32_t>(segs.size());
    w.writePOD(count);
    for (const auto& s : segs) {
        writeMeasurementAnchor(w, s.vertA);
        writeMeasurementAnchor(w, s.vertB);
        uint8_t isRef = s.isReference ? 1 : 0;
        w.writePOD(isRef);
        w.writePOD(s.divisions);
        w.writeString(s.name);
    }
}

void readMeasurementSegments(BufReader& r, std::vector<MeasurementSegment>& segs, const std::unordered_map<uint32_t, Mesh*>& meshIdMap) {
    uint32_t count = 0;
    r.readPOD(count);
    if (!r.ok) return;
    segs.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        readMeasurementAnchor(r, segs[i].vertA, meshIdMap);
        readMeasurementAnchor(r, segs[i].vertB, meshIdMap);
        uint8_t isRef = 0;
        r.readPOD(isRef);
        segs[i].isReference = (isRef != 0);
        r.readPOD(segs[i].divisions);
        r.readString(segs[i].name);
        if (!r.ok) return;
    }
}

void writeLayerStack(BufWriter& w, const LayerStack& ls) {
    w.writePOD(ls.getActiveIdx());
    w.writeVector(ls.getBase());
    uint32_t count = static_cast<uint32_t>(ls.count());
    w.writePOD(count);
    for (uint32_t i = 0; i < count; ++i) {
        const Layer& l = ls.at(static_cast<int>(i));
        w.writePOD(l.id);
        w.writeString(l.name);
        uint8_t vis = l.visible ? 1 : 0;
        uint8_t rec = l.recording ? 1 : 0;
        w.writePOD(vis);
        w.writePOD(rec);
        w.writePOD(l.intensity);
        w.writeVector(l.deltaVerts);
    }
}

void readLayerStack(BufReader& r, LayerStack& ls) {
    ls.clear();
    int activeIdx = -1;
    r.readPOD(activeIdx);
    r.readVector(ls.getBase());
    uint32_t count = 0;
    r.readPOD(count);
    if (!r.ok) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        std::string name;
        uint8_t vis = 1;
        uint8_t rec = 1;
        float intensity = 1.0f;
        r.readPOD(id);
        r.readString(name);
        r.readPOD(vis);
        r.readPOD(rec);
        r.readPOD(intensity);
        int idx = ls.addLayer(0, name);
        Layer* l = ls.getLayer(idx);
        if (l) {
            l->id = id;
            l->visible = (vis != 0);
            l->recording = (rec != 0);
            l->intensity = intensity;
            r.readVector(l->deltaVerts);
        } else {
            std::vector<float> dummy;
            r.readVector(dummy);
        }
        if (!r.ok) return;
    }
    ls.setActiveIdx(activeIdx);
}

void writeTubeData(BufWriter& w, const TubeData& td) {
    w.writeVector(td.spline.getPoints());
    uint32_t creationMode = static_cast<uint32_t>(td.creationMode);
    uint32_t profileType = static_cast<uint32_t>(td.profileType);
    uint32_t capType = static_cast<uint32_t>(td.capType);
    w.writePOD(creationMode);
    w.writePOD(profileType);
    w.writePOD(capType);
    w.writePOD(td.strokeSmoothing);
    w.writePOD(td.curveFitTolerance);
    w.writePOD(td.baseRadius);
    w.writePOD(td.startRadiusScale);
    w.writePOD(td.endRadiusScale);
    w.writePOD(td.curveSegments);
    uint8_t autoDensity = td.autoDensity ? 1 : 0;
    w.writePOD(autoDensity);
    w.writePOD(td.cornerSegments);
    w.writePOD(td.radialSegments);
    w.writePOD(td.ribbonThickness);
    w.writePOD(td.twistDegrees);
    uint8_t snapToSurface = td.snapToSurface ? 1 : 0;
    w.writePOD(snapToSurface);
    w.writePOD(td.surfaceOffset);
    uint8_t useRadiusProfile = td.useRadiusProfile ? 1 : 0;
    w.writePOD(useRadiusProfile);
    w.writeVector(td.radiusProfile);
    uint8_t radiusProfileSmooth = td.radiusProfileSmooth ? 1 : 0;
    w.writePOD(radiusProfileSmooth);
    w.writeVector(td.crossSectionProfile);
    uint8_t crossSectionSmooth = td.crossSectionSmooth ? 1 : 0;
    uint8_t showProfileAtEnd = td.showProfileAtEnd ? 1 : 0;
    w.writePOD(crossSectionSmooth);
    w.writePOD(showProfileAtEnd);
}

void readTubeData(BufReader& r, TubeData& td) {
    r.readVector(td.spline.getPoints());
    uint32_t creationMode = 0, profileType = 0, capType = 0;
    r.readPOD(creationMode);
    r.readPOD(profileType);
    r.readPOD(capType);
    td.creationMode = static_cast<TubeCreationMode>(creationMode);
    td.profileType = static_cast<TubeProfileType>(profileType);
    td.capType = static_cast<TubeCapType>(capType);
    r.readPOD(td.strokeSmoothing);
    r.readPOD(td.curveFitTolerance);
    r.readPOD(td.baseRadius);
    r.readPOD(td.startRadiusScale);
    r.readPOD(td.endRadiusScale);
    r.readPOD(td.curveSegments);
    uint8_t autoDensity = 1;
    r.readPOD(autoDensity);
    td.autoDensity = (autoDensity != 0);
    r.readPOD(td.cornerSegments);
    r.readPOD(td.radialSegments);
    r.readPOD(td.ribbonThickness);
    r.readPOD(td.twistDegrees);
    uint8_t snapToSurface = 1;
    r.readPOD(snapToSurface);
    td.snapToSurface = (snapToSurface != 0);
    r.readPOD(td.surfaceOffset);
    uint8_t useRadiusProfile = 0;
    r.readPOD(useRadiusProfile);
    td.useRadiusProfile = (useRadiusProfile != 0);
    r.readVector(td.radiusProfile);
    uint8_t radiusProfileSmooth = 1;
    r.readPOD(radiusProfileSmooth);
    td.radiusProfileSmooth = (radiusProfileSmooth != 0);
    r.readVector(td.crossSectionProfile);
    uint8_t crossSectionSmooth = 1, showProfileAtEnd = 1;
    r.readPOD(crossSectionSmooth);
    r.readPOD(showProfileAtEnd);
    td.crossSectionSmooth = (crossSectionSmooth != 0);
    td.showProfileAtEnd = (showProfileAtEnd != 0);
}

void writeDynamicSubdivSettings(BufWriter& w, const DynamicSubdivSettings& ds) {
    w.writePOD(ds);
}

void readDynamicSubdivSettings(BufReader& r, DynamicSubdivSettings& ds) {
    r.readPOD(ds);
}

void writePrimitiveData(BufWriter& w, const PrimitiveData& pd) {
    w.writePOD(pd);
}

void readPrimitiveData(BufReader& r, PrimitiveData& pd) {
    r.readPOD(pd);
}

void writeMeshState(BufWriter& w, const MeshState& ms) {
    w.writePOD(ms.id);
    w.writeString(ms.outlinerName);
    w.writePOD(ms.nbVerts);
    w.writePOD(ms.nbFaces);
    w.writePOD(ms.vertsGeneration);
    w.writePOD(ms.matrix);
    writeLayerStack(w, ms.layerStack);

    uint8_t flags = 0;
    if (ms.visibleV1) flags |= 1 << 0;
    if (ms.visibleV2) flags |= 1 << 1;
    if (ms.isArmature) flags |= 1 << 2;
    if (ms.isTube && ms.tubeData) flags |= 1 << 3;
    if (ms.isPrimitive && ms.primitiveData) flags |= 1 << 4;
    if (ms.symEnabled) flags |= 1 << 5;
    if (ms.symX) flags |= 1 << 6;
    if (ms.symY) flags |= 1 << 7;
    w.writePOD(flags);
    uint8_t symZ = ms.symZ ? 1 : 0;
    w.writePOD(symZ);
    w.writePOD(ms.symMode);

    if (ms.isTube && ms.tubeData) writeTubeData(w, *ms.tubeData);
    if (ms.isPrimitive && ms.primitiveData) writePrimitiveData(w, *ms.primitiveData);
    writeDynamicSubdivSettings(w, ms.dynamicSettings);

    w.writeVector(ms.verts);
    w.writeVector(ms.colors);
    w.writeVector(ms.materials);
    w.writeVector(ms.faces);
    w.writeVector(ms.faceGroups);
    w.writeVector(ms.vrfStartCount);
    w.writeVector(ms.vertRingFace);
    w.writeVector(ms.vrvStartCount);
    w.writeVector(ms.vertRingVert);
    w.writeVector(ms.vertOnEdge);
    w.writeVector(ms.vertVisible);
    w.writeVector(ms.faceVisible);
    w.writeVector(ms.edgeCreases);
}

void readMeshState(BufReader& r, MeshState& ms) {
    r.readPOD(ms.id);
    r.readString(ms.outlinerName);
    r.readPOD(ms.nbVerts);
    r.readPOD(ms.nbFaces);
    r.readPOD(ms.vertsGeneration);
    r.readPOD(ms.matrix);
    readLayerStack(r, ms.layerStack);

    uint8_t flags = 0;
    uint8_t symZ = 0;
    r.readPOD(flags);
    r.readPOD(symZ);
    r.readPOD(ms.symMode);
    if (!r.ok) return;

    ms.visibleV1 = (flags & (1 << 0)) != 0;
    ms.visibleV2 = (flags & (1 << 1)) != 0;
    ms.isArmature = (flags & (1 << 2)) != 0;
    ms.isTube = (flags & (1 << 3)) != 0;
    ms.isPrimitive = (flags & (1 << 4)) != 0;
    ms.symEnabled = (flags & (1 << 5)) != 0;
    ms.symX = (flags & (1 << 6)) != 0;
    ms.symY = (flags & (1 << 7)) != 0;
    ms.symZ = (symZ != 0);

    if (ms.isTube) {
        ms.tubeData = std::make_unique<TubeData>();
        readTubeData(r, *ms.tubeData);
    }
    if (ms.isPrimitive) {
        ms.primitiveData = std::make_unique<PrimitiveData>();
        readPrimitiveData(r, *ms.primitiveData);
    }
    readDynamicSubdivSettings(r, ms.dynamicSettings);

    r.readVector(ms.verts);
    r.readVector(ms.colors);
    r.readVector(ms.materials);
    r.readVector(ms.faces);
    r.readVector(ms.faceGroups);
    r.readVector(ms.vrfStartCount);
    r.readVector(ms.vertRingFace);
    r.readVector(ms.vrvStartCount);
    r.readVector(ms.vertRingVert);
    r.readVector(ms.vertOnEdge);
    r.readVector(ms.vertVisible);
    r.readVector(ms.faceVisible);
    r.readVector(ms.edgeCreases);
}

void writeHistoryState(BufWriter& w, const HistoryState& hs) {
    w.writePOD(hs.selectedMeshIdx);
    w.writeVector(hs.selectedMeshIndices);
    writeMeasurementSegments(w, hs.measureSegments);
    writeMeasurementSegments(w, hs.dividerSegments);
    uint32_t count = static_cast<uint32_t>(hs.meshes.size());
    w.writePOD(count);
    for (const auto& ms : hs.meshes) {
        writeMeshState(w, ms);
    }
}

void readHistoryState(BufReader& r, HistoryState& hs, const std::unordered_map<uint32_t, Mesh*>& meshIdMap) {
    r.readPOD(hs.selectedMeshIdx);
    r.readVector(hs.selectedMeshIndices);
    readMeasurementSegments(r, hs.measureSegments, meshIdMap);
    readMeasurementSegments(r, hs.dividerSegments, meshIdMap);
    uint32_t count = 0;
    r.readPOD(count);
    if (!r.ok) return;
    hs.meshes.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        readMeshState(r, hs.meshes[i]);
        if (!r.ok) return;
    }
}

void writeCompressedHistoryState(BufWriter& w, const TopologyUndoEntry::CompressedHistoryState& chs) {
    w.writePOD(chs.selectedMeshIdx);
    w.writeVector(chs.selectedMeshIndices);
    writeMeasurementSegments(w, chs.measureSegments);
    writeMeasurementSegments(w, chs.dividerSegments);
    uint32_t count = static_cast<uint32_t>(chs.meshes.size());
    w.writePOD(count);
    for (const auto& cm : chs.meshes) {
        w.writePOD(cm.id);
        w.writeString(cm.outlinerName);
        w.writePOD(cm.nbVerts);
        w.writePOD(cm.nbFaces);
        w.writePOD(cm.vertsGeneration);
        w.writePOD(cm.matrix);
        writeLayerStack(w, cm.layerStack);

        uint8_t flags = 0;
        if (cm.visibleV1) flags |= 1 << 0;
        if (cm.visibleV2) flags |= 1 << 1;
        if (cm.isArmature) flags |= 1 << 2;
        if (cm.isTube && cm.tubeData) flags |= 1 << 3;
        if (cm.isPrimitive && cm.primitiveData) flags |= 1 << 4;
        if (cm.symEnabled) flags |= 1 << 5;
        if (cm.symX) flags |= 1 << 6;
        if (cm.symY) flags |= 1 << 7;
        w.writePOD(flags);
        uint8_t symZ = cm.symZ ? 1 : 0;
        w.writePOD(symZ);
        w.writePOD(cm.symMode);

        if (cm.isTube && cm.tubeData) writeTubeData(w, *cm.tubeData);
        if (cm.isPrimitive && cm.primitiveData) writePrimitiveData(w, *cm.primitiveData);
        writeDynamicSubdivSettings(w, cm.dynamicSettings);

        uint64_t counts[13] = {
            cm.vertsCount, cm.colorsCount, cm.materialsCount, cm.facesCount,
            cm.faceGroupsCount, cm.vrfStartCountCount, cm.vertRingFaceCount,
            cm.vrvStartCountCount, cm.vertRingVertCount, cm.vertOnEdgeCount,
            cm.vertVisibleCount, cm.faceVisibleCount, cm.edgeCreasesCount
        };
        w.writeBytes(counts, sizeof(counts));

        uint64_t totalRaw = static_cast<uint64_t>(cm.payload.totalRawBytes);
        w.writePOD(totalRaw);
        uint32_t numChunks = static_cast<uint32_t>(cm.payload.chunks.size());
        w.writePOD(numChunks);
        for (const auto& c : cm.payload.chunks) {
            w.writePOD(c.rawSize);
            w.writeVector(c.data);
        }
    }
}

void readCompressedHistoryState(BufReader& r, TopologyUndoEntry::CompressedHistoryState& chs, const std::unordered_map<uint32_t, Mesh*>& meshIdMap) {
    r.readPOD(chs.selectedMeshIdx);
    r.readVector(chs.selectedMeshIndices);
    readMeasurementSegments(r, chs.measureSegments, meshIdMap);
    readMeasurementSegments(r, chs.dividerSegments, meshIdMap);
    uint32_t count = 0;
    r.readPOD(count);
    if (!r.ok) return;
    chs.meshes.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto& cm = chs.meshes[i];
        r.readPOD(cm.id);
        r.readString(cm.outlinerName);
        r.readPOD(cm.nbVerts);
        r.readPOD(cm.nbFaces);
        r.readPOD(cm.vertsGeneration);
        r.readPOD(cm.matrix);
        readLayerStack(r, cm.layerStack);

        uint8_t flags = 0;
        uint8_t symZ = 0;
        r.readPOD(flags);
        r.readPOD(symZ);
        r.readPOD(cm.symMode);
        if (!r.ok) return;

        cm.visibleV1 = (flags & (1 << 0)) != 0;
        cm.visibleV2 = (flags & (1 << 1)) != 0;
        cm.isArmature = (flags & (1 << 2)) != 0;
        cm.isTube = (flags & (1 << 3)) != 0;
        cm.isPrimitive = (flags & (1 << 4)) != 0;
        cm.symEnabled = (flags & (1 << 5)) != 0;
        cm.symX = (flags & (1 << 6)) != 0;
        cm.symY = (flags & (1 << 7)) != 0;
        cm.symZ = (symZ != 0);

        if (cm.isTube) {
            cm.tubeData = std::make_unique<TubeData>();
            readTubeData(r, *cm.tubeData);
        }
        if (cm.isPrimitive) {
            cm.primitiveData = std::make_unique<PrimitiveData>();
            readPrimitiveData(r, *cm.primitiveData);
        }
        readDynamicSubdivSettings(r, cm.dynamicSettings);

        uint64_t counts[13] = {};
        r.readBytes(counts, sizeof(counts));
        if (!r.ok) return;
        cm.vertsCount = static_cast<size_t>(counts[0]);
        cm.colorsCount = static_cast<size_t>(counts[1]);
        cm.materialsCount = static_cast<size_t>(counts[2]);
        cm.facesCount = static_cast<size_t>(counts[3]);
        cm.faceGroupsCount = static_cast<size_t>(counts[4]);
        cm.vrfStartCountCount = static_cast<size_t>(counts[5]);
        cm.vertRingFaceCount = static_cast<size_t>(counts[6]);
        cm.vrvStartCountCount = static_cast<size_t>(counts[7]);
        cm.vertRingVertCount = static_cast<size_t>(counts[8]);
        cm.vertOnEdgeCount = static_cast<size_t>(counts[9]);
        cm.vertVisibleCount = static_cast<size_t>(counts[10]);
        cm.faceVisibleCount = static_cast<size_t>(counts[11]);
        cm.edgeCreasesCount = static_cast<size_t>(counts[12]);

        uint64_t totalRaw = 0;
        r.readPOD(totalRaw);
        cm.payload.totalRawBytes = static_cast<size_t>(totalRaw);
        uint32_t numChunks = 0;
        r.readPOD(numChunks);
        if (!r.ok) return;
        cm.payload.chunks.resize(numChunks);
        for (uint32_t c = 0; c < numChunks; ++c) {
            r.readPOD(cm.payload.chunks[c].rawSize);
            r.readVector(cm.payload.chunks[c].data);
            if (!r.ok) return;
        }
    }
}

// --- Per-entry serialization ---

struct SerializedEntryBlob {
    uint8_t tag = 0;
    uint8_t compressed = 0;
    uint32_t rawSize = 0;
    std::vector<uint8_t> payload;
};

bool serializeSingleEntry(const UndoEntry* entry, SerializedEntryBlob& out) {
    if (!entry) return false;

    BufWriter w;
    bool allowLz4 = true;

    if (const auto* sculpt = dynamic_cast<const SculptUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::Sculpt);
        w.writeString(sculpt->description);
        uint32_t dCount = static_cast<uint32_t>(sculpt->deltas.size());
        w.writePOD(dCount);
        for (const auto& d : sculpt->deltas) {
            w.writePOD(d.meshId);
            uint8_t flags = 0;
            if (d.hasPositions) flags |= 1 << 0;
            if (d.hasColors) flags |= 1 << 1;
            if (d.hasMaterials) flags |= 1 << 2;
            if (d.hasLayerDeltas) flags |= 1 << 3;
            w.writePOD(flags);
            w.writePOD(d.activeLayerIdx);
            w.writeVector(d.indices);
            if (d.hasPositions) {
                w.writeVector(d.prevVerts);
                w.writeVector(d.nextVerts);
            }
            if (d.hasColors) {
                w.writeVector(d.prevColors);
                w.writeVector(d.nextColors);
            }
            if (d.hasMaterials) {
                w.writeVector(d.prevMaterials);
                w.writeVector(d.nextMaterials);
            }
            if (d.hasLayerDeltas) {
                w.writeVector(d.prevLayerDeltas);
                w.writeVector(d.nextLayerDeltas);
            }
        }
    } else if (const auto* topo = dynamic_cast<const TopologyUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::Topology);
        allowLz4 = false; // Payload inside CompressedMeshState is already LZ4-compressed
        w.writeString(topo->description);
        writeCompressedHistoryState(w, topo->getBeforeCompressed());
        writeCompressedHistoryState(w, topo->getAfterCompressed());
    } else if (const auto* dyntopo = dynamic_cast<const DynamicTopologyUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::DynamicTopology);
        w.writePOD(dyntopo->meshId);
        w.writeString(dyntopo->description);
        uint8_t hasRec = dyntopo->record ? 1 : 0;
        w.writePOD(hasRec);
        if (dyntopo->record) {
            const auto& rec = *dyntopo->record;
            w.writePOD(rec.vertexCount);
            w.writePOD(rec.triangleCount);
            w.writeVector(rec.vertexSlots);
            w.writeVector(rec.triangleSlots);
            w.writeVector(rec.vertices.positions);
            w.writeVector(rec.vertices.colors);
            w.writeVector(rec.vertices.materials);
            w.writeVector(rec.vertices.visible);
            w.writeVector(rec.triangles.triangles);
            w.writeVector(rec.triangles.faceGroups);
            w.writeVector(rec.triangles.visible);
        }
    } else if (const auto* transform = dynamic_cast<const TransformUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::Transform);
        w.writeString(transform->getDescription());
        uint32_t count = static_cast<uint32_t>(transform->m_meshStates.size());
        w.writePOD(count);
        for (const auto& ms : transform->m_meshStates) {
            w.writePOD(ms.meshId);
            w.writePOD(ms.beforeMatrix);
            w.writePOD(ms.afterMatrix);
            uint8_t flags = 0;
            if (ms.bakedScale) flags |= 1 << 0;
            if (ms.hasVertexDeformation) flags |= 1 << 1;
            if (ms.isSparse) flags |= 1 << 2;
            if (ms.isPrimitive) flags |= 1 << 3;
            w.writePOD(flags);
            if (ms.bakedScale) {
                w.writePOD(ms.scaleX);
                w.writePOD(ms.scaleY);
                w.writePOD(ms.scaleZ);
            }
            if (ms.hasVertexDeformation) {
                if (ms.isSparse) {
                    w.writeVector(ms.sparseVertIndices);
                    w.writeVector(ms.sparseBeforeCoords);
                    w.writeVector(ms.sparseAfterCoords);
                    w.writeVector(ms.affectedFaces);
                } else {
                    w.writeVector(ms.beforeVerts);
                    w.writeVector(ms.afterVerts);
                    w.writeVector(ms.beforeNormals);
                    w.writeVector(ms.afterNormals);
                }
            }
            if (ms.isPrimitive) {
                w.writePOD(ms.beforeLocalTransform);
                w.writePOD(ms.afterLocalTransform);
            }
        }
    } else if (const auto* meta = dynamic_cast<const SceneMetaUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::SceneMeta);
        w.writeString(meta->description);
        writeHistoryState(w, meta->before);
        writeHistoryState(w, meta->after);
    } else if (const auto* meas = dynamic_cast<const MeasurementUndoEntry*>(entry)) {
        out.tag = static_cast<uint8_t>(EntryTag::Measurement);
        w.writeString(meas->description);
        writeMeasurementSegments(w, meas->beforeMeasure);
        writeMeasurementSegments(w, meas->afterMeasure);
        writeMeasurementSegments(w, meas->beforeDivider);
        writeMeasurementSegments(w, meas->afterDivider);
    } else {
        return false;
    }

    out.rawSize = static_cast<uint32_t>(w.buf.size());
    out.compressed = 0;

    if (allowLz4 && w.buf.size() >= LZ4_MIN_BYTES && w.buf.size() <= static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
        int rawSz = static_cast<int>(w.buf.size());
        int maxDst = LZ4_compressBound(rawSz);
        if (maxDst > 0) {
            std::vector<uint8_t> comp(static_cast<size_t>(maxDst));
            int compSz = LZ4_compress_default(
                reinterpret_cast<const char*>(w.buf.data()),
                reinterpret_cast<char*>(comp.data()),
                rawSz,
                maxDst
            );
            if (compSz > 0 && static_cast<size_t>(compSz) < w.buf.size()) {
                comp.resize(static_cast<size_t>(compSz));
                out.compressed = 1;
                out.payload = std::move(comp);
                return true;
            }
        }
    }

    out.payload = std::move(w.buf);
    return true;
}

std::unique_ptr<UndoEntry> deserializeSingleEntry(
    uint8_t tag,
    uint8_t compressed,
    uint32_t rawSize,
    const uint8_t* payload,
    size_t payloadSize,
    const std::unordered_map<uint32_t, Mesh*>& meshIdMap)
{
    std::vector<uint8_t> decompressed;
    const uint8_t* rawPtr = payload;
    size_t rawLen = payloadSize;

    if (compressed == 1) {
        if (rawSize == 0) return nullptr;
        decompressed.resize(rawSize);
        int res = LZ4_decompress_safe(
            reinterpret_cast<const char*>(payload),
            reinterpret_cast<char*>(decompressed.data()),
            static_cast<int>(payloadSize),
            static_cast<int>(rawSize)
        );
        if (res < 0 || static_cast<uint32_t>(res) != rawSize) {
            sculpt_log_lvl(LogLevel::Warning, "[UndoSerializer] LZ4 decompression failed for entry tag %u (res=%d, expected=%u)\n", tag, res, rawSize);
            return nullptr;
        }
        rawPtr = decompressed.data();
        rawLen = decompressed.size();
    }

    BufReader r(rawPtr, rawLen);
    switch (static_cast<EntryTag>(tag)) {
        case EntryTag::Sculpt: {
            auto e = std::make_unique<SculptUndoEntry>();
            r.readString(e->description);
            uint32_t dCount = 0;
            r.readPOD(dCount);
            if (!r.ok) return nullptr;
            e->deltas.resize(dCount);
            for (uint32_t i = 0; i < dCount; ++i) {
                auto& d = e->deltas[i];
                r.readPOD(d.meshId);
                uint8_t flags = 0;
                r.readPOD(flags);
                r.readPOD(d.activeLayerIdx);
                if (!r.ok) return nullptr;
                d.hasPositions = (flags & (1 << 0)) != 0;
                d.hasColors = (flags & (1 << 1)) != 0;
                d.hasMaterials = (flags & (1 << 2)) != 0;
                d.hasLayerDeltas = (flags & (1 << 3)) != 0;
                r.readVector(d.indices);
                if (d.hasPositions) {
                    r.readVector(d.prevVerts);
                    r.readVector(d.nextVerts);
                }
                if (d.hasColors) {
                    r.readVector(d.prevColors);
                    r.readVector(d.nextColors);
                }
                if (d.hasMaterials) {
                    r.readVector(d.prevMaterials);
                    r.readVector(d.nextMaterials);
                }
                if (d.hasLayerDeltas) {
                    r.readVector(d.prevLayerDeltas);
                    r.readVector(d.nextLayerDeltas);
                }
                if (!r.ok) return nullptr;
            }
            return e;
        }
        case EntryTag::Topology: {
            auto e = std::make_unique<TopologyUndoEntry>();
            r.readString(e->description);
            TopologyUndoEntry::CompressedHistoryState before, after;
            readCompressedHistoryState(r, before, meshIdMap);
            readCompressedHistoryState(r, after, meshIdMap);
            if (!r.ok) return nullptr;
            e->setCompressedStates(std::move(before), std::move(after));
            return e;
        }
        case EntryTag::DynamicTopology: {
            uint32_t meshId = 0;
            std::string desc;
            uint8_t hasRec = 0;
            r.readPOD(meshId);
            r.readString(desc);
            r.readPOD(hasRec);
            if (!r.ok) return nullptr;
            std::unique_ptr<dynamic::TopologyUndo> rec;
            if (hasRec != 0) {
                rec = std::make_unique<dynamic::TopologyUndo>();
                r.readPOD(rec->vertexCount);
                r.readPOD(rec->triangleCount);
                r.readVector(rec->vertexSlots);
                r.readVector(rec->triangleSlots);
                r.readVector(rec->vertices.positions);
                r.readVector(rec->vertices.colors);
                r.readVector(rec->vertices.materials);
                r.readVector(rec->vertices.visible);
                r.readVector(rec->triangles.triangles);
                r.readVector(rec->triangles.faceGroups);
                r.readVector(rec->triangles.visible);
                if (!r.ok) return nullptr;
            }
            return std::make_unique<DynamicTopologyUndoEntry>(meshId, std::move(rec), desc);
        }
        case EntryTag::Transform: {
            std::string desc;
            r.readString(desc);
            uint32_t count = 0;
            r.readPOD(count);
            if (!r.ok) return nullptr;
            auto e = std::make_unique<TransformUndoEntry>(desc);
            e->m_meshStates.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                auto& ms = e->m_meshStates[i];
                r.readPOD(ms.meshId);
                r.readPOD(ms.beforeMatrix);
                r.readPOD(ms.afterMatrix);
                uint8_t flags = 0;
                r.readPOD(flags);
                if (!r.ok) return nullptr;
                ms.bakedScale = (flags & (1 << 0)) != 0;
                ms.hasVertexDeformation = (flags & (1 << 1)) != 0;
                ms.isSparse = (flags & (1 << 2)) != 0;
                ms.isPrimitive = (flags & (1 << 3)) != 0;
                if (ms.bakedScale) {
                    r.readPOD(ms.scaleX);
                    r.readPOD(ms.scaleY);
                    r.readPOD(ms.scaleZ);
                }
                if (ms.hasVertexDeformation) {
                    if (ms.isSparse) {
                        r.readVector(ms.sparseVertIndices);
                        r.readVector(ms.sparseBeforeCoords);
                        r.readVector(ms.sparseAfterCoords);
                        r.readVector(ms.affectedFaces);
                    } else {
                        r.readVector(ms.beforeVerts);
                        r.readVector(ms.afterVerts);
                        r.readVector(ms.beforeNormals);
                        r.readVector(ms.afterNormals);
                    }
                }
                if (ms.isPrimitive) {
                    r.readPOD(ms.beforeLocalTransform);
                    r.readPOD(ms.afterLocalTransform);
                }
                if (!r.ok) return nullptr;
            }
            return e;
        }
        case EntryTag::SceneMeta: {
            auto e = std::make_unique<SceneMetaUndoEntry>();
            r.readString(e->description);
            readHistoryState(r, e->before, meshIdMap);
            readHistoryState(r, e->after, meshIdMap);
            if (!r.ok) return nullptr;
            return e;
        }
        case EntryTag::Measurement: {
            auto e = std::make_unique<MeasurementUndoEntry>();
            r.readString(e->description);
            readMeasurementSegments(r, e->beforeMeasure, meshIdMap);
            readMeasurementSegments(r, e->afterMeasure, meshIdMap);
            readMeasurementSegments(r, e->beforeDivider, meshIdMap);
            readMeasurementSegments(r, e->afterDivider, meshIdMap);
            if (!r.ok) return nullptr;
            return e;
        }
    }
    return nullptr;
}

} // namespace

bool serializeStacks(
    const std::deque<std::unique_ptr<UndoEntry>>& undoStack,
    const std::deque<std::unique_ptr<UndoEntry>>& redoStack,
    std::vector<uint8_t>& outBuffer)
{
    const size_t totalEntries = undoStack.size() + redoStack.size();
    std::vector<const UndoEntry*> flatPtrs;
    flatPtrs.reserve(totalEntries);
    for (const auto& e : undoStack) flatPtrs.push_back(e.get());
    for (const auto& e : redoStack) flatPtrs.push_back(e.get());

    std::vector<SerializedEntryBlob> blobs(totalEntries);
    std::vector<uint8_t> valid(totalEntries, 0);

    #pragma omp parallel for schedule(dynamic) if(totalEntries > 4)
    for (int64_t i = 0; i < static_cast<int64_t>(totalEntries); ++i) {
        if (serializeSingleEntry(flatPtrs[static_cast<size_t>(i)], blobs[static_cast<size_t>(i)])) {
            valid[static_cast<size_t>(i)] = 1;
        }
    }

    uint32_t undoCount = 0;
    for (size_t i = 0; i < undoStack.size(); ++i) {
        if (valid[i]) ++undoCount;
    }
    uint32_t redoCount = 0;
    for (size_t i = undoStack.size(); i < totalEntries; ++i) {
        if (valid[i]) ++redoCount;
    }

    BufWriter hdr;
    hdr.writeBytes(MAGIC, 4);
    hdr.writePOD(FORMAT_VERSION);
    hdr.writePOD(undoCount);
    hdr.writePOD(redoCount);

    size_t totalPayloadBytes = hdr.buf.size();
    for (size_t i = 0; i < totalEntries; ++i) {
        if (!valid[i]) continue;
        // tag(1) + compressed(1) + rawSize(4) + payloadSize(4) + payload
        totalPayloadBytes += 10 + blobs[i].payload.size();
    }

    outBuffer.reserve(outBuffer.size() + totalPayloadBytes);
    outBuffer.insert(outBuffer.end(), hdr.buf.begin(), hdr.buf.end());

    for (size_t i = 0; i < totalEntries; ++i) {
        if (!valid[i]) continue;
        const auto& b = blobs[i];
        outBuffer.push_back(b.tag);
        outBuffer.push_back(b.compressed);
        uint32_t rawSz = b.rawSize;
        uint32_t paySz = static_cast<uint32_t>(b.payload.size());
        const uint8_t* pRaw = reinterpret_cast<const uint8_t*>(&rawSz);
        const uint8_t* pPay = reinterpret_cast<const uint8_t*>(&paySz);
        outBuffer.insert(outBuffer.end(), pRaw, pRaw + 4);
        outBuffer.insert(outBuffer.end(), pPay, pPay + 4);
        outBuffer.insert(outBuffer.end(), b.payload.begin(), b.payload.end());
    }

    sculpt_log("[UndoSerializer] Serialized %u undo + %u redo entries (%zu bytes)\n",
               undoCount, redoCount, totalPayloadBytes);
    return true;
}

bool deserializeStacks(
    const uint8_t* data,
    size_t size,
    size_t& byteOffset,
    std::deque<std::unique_ptr<UndoEntry>>& outUndoStack,
    std::deque<std::unique_ptr<UndoEntry>>& outRedoStack,
    const std::unordered_map<uint32_t, Mesh*>& meshIdMap)
{
    outUndoStack.clear();
    outRedoStack.clear();

    BufReader r(data, size, byteOffset);
    char magic[4] = {};
    r.readBytes(magic, 4);
    if (!r.ok || std::memcmp(magic, MAGIC, 4) != 0) {
        sculpt_log_lvl(LogLevel::Warning, "[UndoSerializer] Invalid magic header\n");
        return false;
    }

    uint32_t version = 0;
    uint32_t undoCount = 0;
    uint32_t redoCount = 0;
    r.readPOD(version);
    r.readPOD(undoCount);
    r.readPOD(redoCount);
    if (!r.ok || version > FORMAT_VERSION) {
        sculpt_log_lvl(LogLevel::Warning, "[UndoSerializer] Unsupported undo history version: %u\n", version);
        return false;
    }

    const size_t total = static_cast<size_t>(undoCount) + static_cast<size_t>(redoCount);
    struct RawEntry {
        uint8_t tag = 0;
        uint8_t compressed = 0;
        uint32_t rawSize = 0;
        uint32_t payloadSize = 0;
        size_t payloadOffset = 0;
    };
    std::vector<RawEntry> rawEntries(total);
    for (size_t i = 0; i < total; ++i) {
        auto& re = rawEntries[i];
        r.readPOD(re.tag);
        r.readPOD(re.compressed);
        r.readPOD(re.rawSize);
        r.readPOD(re.payloadSize);
        if (!r.ok || !r.ensure(re.payloadSize)) {
            sculpt_log_lvl(LogLevel::Warning, "[UndoSerializer] Truncated undo entry #%zu\n", i);
            return false;
        }
        re.payloadOffset = r.offset;
        r.offset += re.payloadSize;
    }

    std::vector<std::unique_ptr<UndoEntry>> decoded(total);
    #pragma omp parallel for schedule(dynamic) if(total > 4)
    for (int64_t i = 0; i < static_cast<int64_t>(total); ++i) {
        const auto& re = rawEntries[static_cast<size_t>(i)];
        decoded[static_cast<size_t>(i)] = deserializeSingleEntry(
            re.tag,
            re.compressed,
            re.rawSize,
            data + re.payloadOffset,
            re.payloadSize,
            meshIdMap
        );
    }

    for (size_t i = 0; i < undoCount; ++i) {
        if (decoded[i]) outUndoStack.push_back(std::move(decoded[i]));
    }
    for (size_t i = undoCount; i < total; ++i) {
        if (decoded[i]) outRedoStack.push_back(std::move(decoded[i]));
    }

    byteOffset = r.offset;
    sculpt_log("[UndoSerializer] Restored %zu undo + %zu redo entries\n",
               outUndoStack.size(), outRedoStack.size());
    return true;
}

} // namespace UndoSerializer

