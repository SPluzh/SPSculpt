#pragma once
#include "UndoEntry.h"
#include "scene/Scene.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <lz4.h>
#include <omp.h>

class TopologyUndoEntry : public UndoEntry {
public:
    struct CompressedMeshPayload {
        static constexpr size_t CHUNK_MAX_RAW = 64 * 1024 * 1024; // 64 MB per chunk

        struct Chunk {
            uint32_t rawSize = 0;
            std::vector<char> data;
        };
        std::vector<Chunk> chunks;
        size_t totalRawBytes = 0;

        void compress(const char* rawSrc, size_t rawSize) {
            totalRawBytes = rawSize;
            chunks.clear();
            if (rawSize == 0) return;

            size_t numChunks = (rawSize + CHUNK_MAX_RAW - 1) / CHUNK_MAX_RAW;
            chunks.resize(numChunks);

            #pragma omp parallel for schedule(dynamic) if(numChunks > 1)
            for (size_t c = 0; c < numChunks; ++c) {
                size_t offset = c * CHUNK_MAX_RAW;
                size_t cRawSize = std::min(CHUNK_MAX_RAW, rawSize - offset);
                chunks[c].rawSize = static_cast<uint32_t>(cRawSize);
                int maxDst = LZ4_compressBound(static_cast<int>(cRawSize));
                chunks[c].data.resize(maxDst);
                int compSize = LZ4_compress_default(
                    rawSrc + offset,
                    chunks[c].data.data(),
                    static_cast<int>(cRawSize),
                    maxDst
                );
                if (compSize > 0) {
                    chunks[c].data.resize(compSize);
                    chunks[c].data.shrink_to_fit();
                } else {
                    chunks[c].data.assign(rawSrc + offset, rawSrc + offset + cRawSize);
                }
            }
        }

        void decompress(char* rawDst, size_t rawSize) const {
            if (rawSize == 0 || chunks.empty()) return;
            size_t numChunks = chunks.size();

            #pragma omp parallel for schedule(dynamic) if(numChunks > 1)
            for (size_t c = 0; c < numChunks; ++c) {
                size_t offset = c * CHUNK_MAX_RAW;
                size_t cRawSize = chunks[c].rawSize;
                int decomp = LZ4_decompress_safe(
                    chunks[c].data.data(),
                    rawDst + offset,
                    static_cast<int>(chunks[c].data.size()),
                    static_cast<int>(cRawSize)
                );
                if (decomp <= 0) {
                    std::memcpy(rawDst + offset, chunks[c].data.data(), std::min(cRawSize, chunks[c].data.size()));
                }
            }
        }

        size_t getMemoryUsage() const {
            size_t total = sizeof(*this);
            for (const auto& c : chunks) {
                total += sizeof(Chunk) + c.data.capacity() * sizeof(char);
            }
            return total;
        }
    };

    struct CompressedMeshState {
        uint32_t id = 0;
        std::string outlinerName;
        int nbVerts = 0;
        int nbFaces = 0;
        uint64_t vertsGeneration = 0;
        glm::mat4 matrix = glm::mat4(1.0f);
        LayerStack layerStack;
        bool visibleV1 = true;
        bool visibleV2 = true;
        bool isArmature = false;
        bool isTube = false;
        std::unique_ptr<TubeData> tubeData;
        bool isPrimitive = false;
        std::unique_ptr<PrimitiveData> primitiveData;
        bool symEnabled = false;
        bool symX = true;
        bool symY = false;
        bool symZ = false;
        int symMode = 0;
        DynamicSubdivSettings dynamicSettings;

        size_t vertsCount = 0;
        size_t colorsCount = 0;
        size_t materialsCount = 0;
        size_t facesCount = 0;
        size_t faceGroupsCount = 0;
        size_t vrfStartCountCount = 0;
        size_t vertRingFaceCount = 0;
        size_t vrvStartCountCount = 0;
        size_t vertRingVertCount = 0;
        size_t vertOnEdgeCount = 0;
        size_t vertVisibleCount = 0;
        size_t faceVisibleCount = 0;
        size_t edgeCreasesCount = 0;

        CompressedMeshPayload payload;

        CompressedMeshState() = default;
        CompressedMeshState(const CompressedMeshState& o) {
            *this = o;
        }
        CompressedMeshState& operator=(const CompressedMeshState& o) {
            if (this == &o) return *this;
            id = o.id;
            outlinerName = o.outlinerName;
            nbVerts = o.nbVerts;
            nbFaces = o.nbFaces;
            vertsGeneration = o.vertsGeneration;
            matrix = o.matrix;
            layerStack = o.layerStack;
            visibleV1 = o.visibleV1;
            visibleV2 = o.visibleV2;
            isArmature = o.isArmature;
            isTube = o.isTube;
            tubeData = o.tubeData ? std::make_unique<TubeData>(*o.tubeData) : nullptr;
            isPrimitive = o.isPrimitive;
            primitiveData = o.primitiveData ? std::make_unique<PrimitiveData>(*o.primitiveData) : nullptr;
            symEnabled = o.symEnabled;
            symX = o.symX;
            symY = o.symY;
            symZ = o.symZ;
            symMode = o.symMode;
            dynamicSettings = o.dynamicSettings;

            vertsCount = o.vertsCount;
            colorsCount = o.colorsCount;
            materialsCount = o.materialsCount;
            facesCount = o.facesCount;
            faceGroupsCount = o.faceGroupsCount;
            vrfStartCountCount = o.vrfStartCountCount;
            vertRingFaceCount = o.vertRingFaceCount;
            vrvStartCountCount = o.vrvStartCountCount;
            vertRingVertCount = o.vertRingVertCount;
            vertOnEdgeCount = o.vertOnEdgeCount;
            vertVisibleCount = o.vertVisibleCount;
            faceVisibleCount = o.faceVisibleCount;
            edgeCreasesCount = o.edgeCreasesCount;

            payload = o.payload;
            return *this;
        }
        CompressedMeshState(CompressedMeshState&&) noexcept = default;
        CompressedMeshState& operator=(CompressedMeshState&&) noexcept = default;

        void compress(const MeshState& ms) {
            id = ms.id;
            outlinerName = ms.outlinerName;
            nbVerts = ms.nbVerts;
            nbFaces = ms.nbFaces;
            vertsGeneration = ms.vertsGeneration;
            matrix = ms.matrix;
            layerStack = ms.layerStack;
            visibleV1 = ms.visibleV1;
            visibleV2 = ms.visibleV2;
            isArmature = ms.isArmature;
            isTube = ms.isTube;
            tubeData = ms.tubeData ? std::make_unique<TubeData>(*ms.tubeData) : nullptr;
            isPrimitive = ms.isPrimitive;
            primitiveData = ms.primitiveData ? std::make_unique<PrimitiveData>(*ms.primitiveData) : nullptr;
            symEnabled = ms.symEnabled;
            symX = ms.symX;
            symY = ms.symY;
            symZ = ms.symZ;
            symMode = ms.symMode;
            dynamicSettings = ms.dynamicSettings;

            vertsCount = ms.verts.size();
            colorsCount = ms.colors.size();
            materialsCount = ms.materials.size();
            facesCount = ms.faces.size();
            faceGroupsCount = ms.faceGroups.size();
            vrfStartCountCount = ms.vrfStartCount.size();
            vertRingFaceCount = ms.vertRingFace.size();
            vrvStartCountCount = ms.vrvStartCount.size();
            vertRingVertCount = ms.vertRingVert.size();
            vertOnEdgeCount = ms.vertOnEdge.size();
            vertVisibleCount = ms.vertVisible.size();
            faceVisibleCount = ms.faceVisible.size();
            edgeCreasesCount = ms.edgeCreases.size();

            size_t bVerts = vertsCount * sizeof(float);
            size_t bColors = colorsCount * sizeof(float);
            size_t bMats = materialsCount * sizeof(float);
            size_t bFaces = facesCount * sizeof(uint32_t);
            size_t bGroups = faceGroupsCount * sizeof(uint32_t);
            size_t bVRF = vrfStartCountCount * sizeof(uint32_t);
            size_t bRF = vertRingFaceCount * sizeof(uint32_t);
            size_t bVRV = vrvStartCountCount * sizeof(uint32_t);
            size_t bRV = vertRingVertCount * sizeof(uint32_t);
            size_t bVOE = vertOnEdgeCount * sizeof(uint8_t);
            size_t bVV = vertVisibleCount * sizeof(uint8_t);
            size_t bFV = faceVisibleCount * sizeof(uint8_t);
            size_t bEC = edgeCreasesCount * sizeof(uint8_t);

            size_t totalBytes = bVerts + bColors + bMats + bFaces + bGroups +
                                bVRF + bRF + bVRV + bRV + bVOE + bVV + bFV + bEC;

            std::vector<char> raw(totalBytes);
            char* ptr = raw.data();
            if (bVerts)  { std::memcpy(ptr, ms.verts.data(), bVerts); ptr += bVerts; }
            if (bColors) { std::memcpy(ptr, ms.colors.data(), bColors); ptr += bColors; }
            if (bMats)   { std::memcpy(ptr, ms.materials.data(), bMats); ptr += bMats; }
            if (bFaces)  { std::memcpy(ptr, ms.faces.data(), bFaces); ptr += bFaces; }
            if (bGroups) { std::memcpy(ptr, ms.faceGroups.data(), bGroups); ptr += bGroups; }
            if (bVRF)    { std::memcpy(ptr, ms.vrfStartCount.data(), bVRF); ptr += bVRF; }
            if (bRF)     { std::memcpy(ptr, ms.vertRingFace.data(), bRF); ptr += bRF; }
            if (bVRV)    { std::memcpy(ptr, ms.vrvStartCount.data(), bVRV); ptr += bVRV; }
            if (bRV)     { std::memcpy(ptr, ms.vertRingVert.data(), bRV); ptr += bRV; }
            if (bVOE)    { std::memcpy(ptr, ms.vertOnEdge.data(), bVOE); ptr += bVOE; }
            if (bVV)     { std::memcpy(ptr, ms.vertVisible.data(), bVV); ptr += bVV; }
            if (bFV)     { std::memcpy(ptr, ms.faceVisible.data(), bFV); ptr += bFV; }
            if (bEC)     { std::memcpy(ptr, ms.edgeCreases.data(), bEC); ptr += bEC; }

            payload.compress(raw.data(), totalBytes);
        }

        void decompress(MeshState& ms) const {
            ms.id = id;
            ms.outlinerName = outlinerName;
            ms.nbVerts = nbVerts;
            ms.nbFaces = nbFaces;
            ms.vertsGeneration = vertsGeneration;
            ms.matrix = matrix;
            ms.layerStack = layerStack;
            ms.visibleV1 = visibleV1;
            ms.visibleV2 = visibleV2;
            ms.isArmature = isArmature;
            ms.isTube = isTube;
            ms.tubeData = tubeData ? std::make_unique<TubeData>(*tubeData) : nullptr;
            ms.isPrimitive = isPrimitive;
            ms.primitiveData = primitiveData ? std::make_unique<PrimitiveData>(*primitiveData) : nullptr;
            ms.symEnabled = symEnabled;
            ms.symX = symX;
            ms.symY = symY;
            ms.symZ = symZ;
            ms.symMode = symMode;
            ms.dynamicSettings = dynamicSettings;

            ms.verts.resize(vertsCount);
            ms.colors.resize(colorsCount);
            ms.materials.resize(materialsCount);
            ms.faces.resize(facesCount);
            ms.faceGroups.resize(faceGroupsCount);
            ms.vrfStartCount.resize(vrfStartCountCount);
            ms.vertRingFace.resize(vertRingFaceCount);
            ms.vrvStartCount.resize(vrvStartCountCount);
            ms.vertRingVert.resize(vertRingVertCount);
            ms.vertOnEdge.resize(vertOnEdgeCount);
            ms.vertVisible.resize(vertVisibleCount);
            ms.faceVisible.resize(faceVisibleCount);
            ms.edgeCreases.resize(edgeCreasesCount);

            size_t bVerts = vertsCount * sizeof(float);
            size_t bColors = colorsCount * sizeof(float);
            size_t bMats = materialsCount * sizeof(float);
            size_t bFaces = facesCount * sizeof(uint32_t);
            size_t bGroups = faceGroupsCount * sizeof(uint32_t);
            size_t bVRF = vrfStartCountCount * sizeof(uint32_t);
            size_t bRF = vertRingFaceCount * sizeof(uint32_t);
            size_t bVRV = vrvStartCountCount * sizeof(uint32_t);
            size_t bRV = vertRingVertCount * sizeof(uint32_t);
            size_t bVOE = vertOnEdgeCount * sizeof(uint8_t);
            size_t bVV = vertVisibleCount * sizeof(uint8_t);
            size_t bFV = faceVisibleCount * sizeof(uint8_t);
            size_t bEC = edgeCreasesCount * sizeof(uint8_t);

            size_t totalBytes = bVerts + bColors + bMats + bFaces + bGroups +
                                bVRF + bRF + bVRV + bRV + bVOE + bVV + bFV + bEC;

            if (totalBytes > 0) {
                std::vector<char> raw(totalBytes);
                payload.decompress(raw.data(), totalBytes);

                const char* ptr = raw.data();
                if (bVerts)  { std::memcpy(ms.verts.data(), ptr, bVerts); ptr += bVerts; }
                if (bColors) { std::memcpy(ms.colors.data(), ptr, bColors); ptr += bColors; }
                if (bMats)   { std::memcpy(ms.materials.data(), ptr, bMats); ptr += bMats; }
                if (bFaces)  { std::memcpy(ms.faces.data(), ptr, bFaces); ptr += bFaces; }
                if (bGroups) { std::memcpy(ms.faceGroups.data(), ptr, bGroups); ptr += bGroups; }
                if (bVRF)    { std::memcpy(ms.vrfStartCount.data(), ptr, bVRF); ptr += bVRF; }
                if (bRF)     { std::memcpy(ms.vertRingFace.data(), ptr, bRF); ptr += bRF; }
                if (bVRV)    { std::memcpy(ms.vrvStartCount.data(), ptr, bVRV); ptr += bVRV; }
                if (bRV)     { std::memcpy(ms.vertRingVert.data(), ptr, bRV); ptr += bRV; }
                if (bVOE)    { std::memcpy(ms.vertOnEdge.data(), ptr, bVOE); ptr += bVOE; }
                if (bVV)     { std::memcpy(ms.vertVisible.data(), ptr, bVV); ptr += bVV; }
                if (bFV)     { std::memcpy(ms.faceVisible.data(), ptr, bFV); ptr += bFV; }
                if (bEC)     { std::memcpy(ms.edgeCreases.data(), ptr, bEC); ptr += bEC; }
            }
        }

        size_t getMemoryUsage() const {
            return sizeof(*this) + payload.getMemoryUsage();
        }
    };

    struct CompressedHistoryState {
        std::vector<CompressedMeshState> meshes;
        int selectedMeshIdx = -1;
        std::vector<int> selectedMeshIndices;
        std::vector<MeasurementSegment> measureSegments;
        std::vector<MeasurementSegment> dividerSegments;

        void compress(const HistoryState& hs) {
            selectedMeshIdx = hs.selectedMeshIdx;
            selectedMeshIndices = hs.selectedMeshIndices;
            measureSegments = hs.measureSegments;
            dividerSegments = hs.dividerSegments;
            meshes.resize(hs.meshes.size());
            for (size_t i = 0; i < hs.meshes.size(); ++i) {
                meshes[i].compress(hs.meshes[i]);
            }
        }

        HistoryState decompress() const {
            HistoryState hs;
            hs.selectedMeshIdx = selectedMeshIdx;
            hs.selectedMeshIndices = selectedMeshIndices;
            hs.measureSegments = measureSegments;
            hs.dividerSegments = dividerSegments;
            hs.meshes.resize(meshes.size());
            for (size_t i = 0; i < meshes.size(); ++i) {
                meshes[i].decompress(hs.meshes[i]);
            }
            return hs;
        }

        size_t getMemoryUsage() const {
            size_t total = sizeof(*this);
            total += selectedMeshIndices.capacity() * sizeof(int);
            total += measureSegments.capacity() * sizeof(MeasurementSegment);
            total += dividerSegments.capacity() * sizeof(MeasurementSegment);
            for (const auto& m : meshes) {
                total += m.getMemoryUsage();
            }
            return total;
        }
    };

private:
    CompressedHistoryState m_beforeCompressed;
    CompressedHistoryState m_afterCompressed;

public:
    std::string description = "Topology change";

    UndoEntryType getType() const override { return UndoEntryType::Topology; }

    void setStates(const HistoryState& beforeState, const HistoryState& afterState) {
        m_beforeCompressed.compress(beforeState);
        m_afterCompressed.compress(afterState);
    }

    HistoryState getBeforeState() const {
        return m_beforeCompressed.decompress();
    }

    HistoryState getAfterState() const {
        return m_afterCompressed.decompress();
    }

    const CompressedHistoryState& getBeforeCompressed() const { return m_beforeCompressed; }
    const CompressedHistoryState& getAfterCompressed() const { return m_afterCompressed; }
    void setCompressedStates(CompressedHistoryState before, CompressedHistoryState after) {
        m_beforeCompressed = std::move(before);
        m_afterCompressed = std::move(after);
    }

    size_t getMemoryUsage() const override {
        return sizeof(*this) + m_beforeCompressed.getMemoryUsage() + m_afterCompressed.getMemoryUsage();
    }

    std::string getDescription() const override { return description; }
};
