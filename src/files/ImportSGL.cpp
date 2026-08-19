#include "files/ImportSGL.h"
#include "common/Constants.h"
#include "common/FormatConstants.h"
#include "mesh/Topology.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <future>

namespace ImportSGL {

class BinaryReader {
    const uint8_t* m_data;
    size_t m_size;
    size_t m_wordOffset;
public:
    BinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_wordOffset(0) {}
    
    uint32_t readU32() {
        if ((m_wordOffset + 1) * 4 > m_size) return 0;
        uint32_t val;
        std::memcpy(&val, m_data + m_wordOffset * 4, 4);
        m_wordOffset++;
        return val;
    }
    
    int32_t readI32() {
        if ((m_wordOffset + 1) * 4 > m_size) return 0;
        int32_t val;
        std::memcpy(&val, m_data + m_wordOffset * 4, 4);
        m_wordOffset++;
        return val;
    }
    
    float readF32() {
        if ((m_wordOffset + 1) * 4 > m_size) return 0.0f;
        float val;
        std::memcpy(&val, m_data + m_wordOffset * 4, 4);
        m_wordOffset++;
        return val;
    }
    
    void readF32Array(float* out, size_t count) {
        if ((m_wordOffset + count) * 4 > m_size) return;
        std::memcpy(out, m_data + m_wordOffset * 4, count * 4);
        m_wordOffset += count;
    }
    
    void readU32Array(uint32_t* out, size_t count) {
        if ((m_wordOffset + count) * 4 > m_size) return;
        std::memcpy(out, m_data + m_wordOffset * 4, count * 4);
        m_wordOffset += count;
    }
    
    void readBytes(uint8_t* out, size_t count) {
        if (m_wordOffset * 4 + count > m_size) return;
        std::memcpy(out, m_data + m_wordOffset * 4, count);
        m_wordOffset += (count + 3) / 4;
    }

    size_t getWordOffset() const { return m_wordOffset; }
    void skipWords(size_t count) { m_wordOffset += count; }
    bool hasData() const { return m_wordOffset * 4 < m_size; }
};

std::vector<Mesh*> importSGL(const std::vector<uint8_t>& buffer, Scene& scene, AngleRenderer& renderer, SculptManager* sculpt) {
    if (buffer.size() < 4) return {};

    BinaryReader reader(buffer.data(), buffer.size());
    uint32_t firstWord = reader.readU32();
    uint32_t version = 0;

    if (buffer.size() >= 8 && std::memcmp(buffer.data(), "SPSC", 4) == 0) {
        version = reader.readU32();
    } else {
        version = firstWord;
    }

    if (version > static_cast<uint32_t>(Format::CURRENT_VERSION)) {
        std::cerr << "Unsupported project version: " << version << std::endl;
        return {};
    }

    // camera and renderer settings
    if (version >= 2) {
        renderer.setShowGrid(reader.readU32() != 0);
        renderer.setShowSymmetryLine(reader.readU32() != 0);
        renderer.setShowContour(reader.readU32() != 0);

        Camera& cam = scene.getCamera();
        uint32_t projType = reader.readU32();
        uint32_t mode = reader.readU32();
        float fov = reader.readF32();
        uint32_t usePivot = reader.readU32();

        cam.setProjectionType(projType == 0 ? CameraEnums::Projection::PERSPECTIVE : CameraEnums::Projection::ORTHOGRAPHIC);
        cam.setMode(static_cast<CameraEnums::CameraMode>(mode));
        cam.setFov(fov);
        cam.setUsePivot(usePivot != 0);

        if (version >= 12) {
            Camera::CameraState cs = cam.getCurrentState();
            cs.projectionType = cam.getProjectionType();
            cs.mode = cam.getMode();
            cs.fov = cam.getFov();
            cs.usePivot = cam.getUsePivot();

            cs.quatRot.x = reader.readF32();
            cs.quatRot.y = reader.readF32();
            cs.quatRot.z = reader.readF32();
            cs.quatRot.w = reader.readF32();

            cs.trans.x = reader.readF32();
            cs.trans.y = reader.readF32();
            cs.trans.z = reader.readF32();

            cs.center.x = reader.readF32();
            cs.center.y = reader.readF32();
            cs.center.z = reader.readF32();

            cs.offset.x = reader.readF32();
            cs.offset.y = reader.readF32();
            cs.offset.z = reader.readF32();

            cs.rotX = reader.readF32();
            cs.rotY = reader.readF32();

            cs.view2DOffsetX = reader.readF32();
            cs.view2DOffsetY = reader.readF32();
            cs.view2DZoom = reader.readF32();
            cs.ref2DMode = (reader.readU32() != 0);
            cs.refDrag = (reader.readU32() != 0);

            cam.applyState(cs);
            if (scene.getCameraRight()) {
                scene.getCameraRight()->applyState(cs);
            }
        } else {
            cam.setRefDragEnabled(false);
            if (scene.getCameraRight()) {
                scene.getCameraRight()->setRefDragEnabled(false);
            }
        }
    }

    uint32_t nbMeshes = reader.readU32();
    std::vector<Mesh*> meshes;
    meshes.reserve(nbMeshes);

    for (uint32_t i = 0; i < nbMeshes; ++i) {
        Mesh* mesh = new Mesh();

        if (version >= 2) {
            uint32_t sType = reader.readU32();
            uint32_t mIdx = reader.readU32();
            bool wire = reader.readU32() != 0;
            bool flat = reader.readU32() != 0;
            float alpha = reader.readF32();

            renderer.setShaderType(sType);
            
            // Legacy SGL versions (< 8) used matcap indices before 3 new presets were prepended.
            if (version < 8 && mIdx < 9) {
                mIdx += 3;
            }
            renderer.setMatcap(static_cast<int>(mIdx));
            renderer.setShowWireframe(wire);
            renderer.setFlatShading(flat);
            renderer.setAlpha(alpha);
        }

        if (version >= 4) {
            bool visibleV1 = reader.readU32() != 0;
            bool visibleV2 = reader.readU32() != 0;
            mesh->setVisible(visibleV1, 0);
            mesh->setVisible(visibleV2, 1);
        }

        // center
        float cx = reader.readF32();
        float cy = reader.readF32();
        float cz = reader.readF32();
        mesh->setCenter(glm::vec3(cx, cy, cz));

        // matrix
        float m[16];
        reader.readF32Array(m, 16);
        std::vector<float> mVec(m, m + 16);
        mesh->setMatrix(mVec);

        // scale
        float scale = reader.readF32();
        mesh->setScale(scale);

        // vertices
        uint32_t nbVertices = reader.readU32();
        mesh->verts.resize(nbVertices * 3);
        reader.readF32Array(mesh->verts.data(), nbVertices * 3);
        mesh->nbVerts = nbVertices;

        // vertex visibility (version >= 5)
        mesh->vertVisible.resize(nbVertices, 1);
        if (version >= 5) {
            reader.readBytes(mesh->vertVisible.data(), nbVertices);
        }

        // colors
        uint32_t nbColors = reader.readU32();
        if (nbColors > 0) {
            mesh->colors.resize(nbColors * 3);
            reader.readF32Array(mesh->colors.data(), nbColors * 3);
        } else {
            mesh->colors.assign(nbVertices * 3, 1.0f);
        }

        // materials
        uint32_t nbMaterials = reader.readU32();
        if (nbMaterials > 0) {
            mesh->materials.resize(nbMaterials * 3);
            reader.readF32Array(mesh->materials.data(), nbMaterials * 3);
        } else {
            mesh->materials.resize(nbVertices * 3);
            for (uint32_t k = 0; k < nbVertices; ++k) {
                mesh->materials[k * 3]     = 0.5f; // roughness
                mesh->materials[k * 3 + 1] = 0.0f; // metalness
                mesh->materials[k * 3 + 2] = 1.0f; // mask
            }
        }

        // faces
        uint32_t nbFaces = reader.readU32();
        mesh->faces.resize(nbFaces * 4);
        reader.readU32Array(mesh->faces.data(), nbFaces * 4);
        mesh->nbFaces = nbFaces;

        if (version <= 2) {
            for (uint32_t k = 0; k < nbFaces; ++k) {
                int32_t* f = reinterpret_cast<int32_t*>(&mesh->faces[k * 4]);
                if (f[3] < 0) {
                    mesh->faces[k * 4 + 3] = TRI_INDEX;
                }
            }
        }

        // uvs
        uint32_t nbTexCoords = reader.readU32();
        std::vector<float> uv;
        if (nbTexCoords > 0) {
            uv.resize(nbTexCoords * 2);
            reader.readF32Array(uv.data(), nbTexCoords * 2);
        }

        // face uvs
        uint32_t nbFacesTexCoords = reader.readU32();
        std::vector<uint32_t> fuv;
        if (nbFacesTexCoords > 0) {
            fuv.resize(nbFacesTexCoords * 4);
            reader.readU32Array(fuv.data(), nbFacesTexCoords * 4);

            if (version <= 2) {
                for (uint32_t k = 0; k < nbFacesTexCoords; ++k) {
                    int32_t* f = reinterpret_cast<int32_t*>(&fuv[k * 4]);
                    if (f[3] < 0) {
                        fuv[k * 4 + 3] = TRI_INDEX;
                    }
                }
            }
        }

        if (!uv.empty() && !fuv.empty()) {
            mesh->initTexCoordsDataFromOBJData(uv, fuv);
        }

        if (version >= 7) {
            uint32_t nbLayers = reader.readU32();
            int32_t activeIdx = reader.readI32();
            mesh->layerStack.clear();
            if (nbLayers > 0) {
                mesh->layerStack.initBase(mesh->verts);
            }
            for (uint32_t l = 0; l < nbLayers; ++l) {
                uint32_t nameLen = reader.readU32();
                std::vector<uint8_t> nameBytes(nameLen);
                if (nameLen > 0) {
                    reader.readBytes(nameBytes.data(), nameLen);
                }
                std::string lName(nameBytes.begin(), nameBytes.end());
                bool lVis = (reader.readU32() != 0);
                float lIntensity = reader.readF32();
                uint32_t nbDelta = reader.readU32();
                std::vector<float> deltas;
                if (nbDelta > 0) {
                    deltas.resize(nbDelta);
                    reader.readF32Array(deltas.data(), nbDelta);
                }
                mesh->layerStack.addLayer(mesh->nbVerts, lName);
                Layer* added = mesh->layerStack.getActive();
                if (added) {
                    added->visible = lVis;
                    added->intensity = lIntensity;
                    if (!deltas.empty()) {
                        added->deltaVerts = std::move(deltas);
                    }
                }
            }
            mesh->layerStack.setActiveIdx(activeIdx);
        }

        if (version >= 13) {
            uint32_t nVrfStartCount = reader.readU32();
            if (nVrfStartCount > 0) {
                mesh->vrfStartCount.resize(nVrfStartCount);
                reader.readU32Array(mesh->vrfStartCount.data(), nVrfStartCount);
            }
            uint32_t nVertRingFace = reader.readU32();
            if (nVertRingFace > 0) {
                mesh->vertRingFace.resize(nVertRingFace);
                reader.readU32Array(mesh->vertRingFace.data(), nVertRingFace);
            }
            uint32_t nVrvStartCount = reader.readU32();
            if (nVrvStartCount > 0) {
                mesh->vrvStartCount.resize(nVrvStartCount);
                reader.readU32Array(mesh->vrvStartCount.data(), nVrvStartCount);
            }
            uint32_t nVertRingVert = reader.readU32();
            if (nVertRingVert > 0) {
                mesh->vertRingVert.resize(nVertRingVert);
                reader.readU32Array(mesh->vertRingVert.data(), nVertRingVert);
            }
            uint32_t nVertOnEdge = reader.readU32();
            if (nVertOnEdge > 0) {
                mesh->vertOnEdge.resize(nVertOnEdge);
                reader.readBytes(mesh->vertOnEdge.data(), nVertOnEdge);
            }
        }

        meshes.push_back(mesh);
    }

    if (version < 13) {
        std::vector<std::future<void>> futures;
        for (auto* m : meshes) {
            futures.push_back(std::async(std::launch::async, [m]() {
                computeTopology(
                    m->nbVerts, m->faces.data(), m->nbFaces,
                    m->vrfStartCount, m->vertRingFace, m->vrvStartCount, m->vertRingVert, m->vertOnEdge
                );
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    }

    for (auto* m : meshes) {
        m->postInit();
    }

    if (version >= 6) {
        auto readAnchor = [&]() -> MeasurementAnchor {
            MeasurementAnchor anchor;
            uint32_t type = reader.readU32();
            if (type == 0) {
                anchor.type = MeasurementAnchor::VERTEX;
                uint32_t meshIdx = reader.readU32();
                anchor.vertIdx = reader.readU32();
                reader.readU32(); // skip unused
                if (meshIdx < meshes.size()) {
                    anchor.mesh = meshes[meshIdx];
                } else {
                    anchor.mesh = nullptr;
                }
            } else {
                anchor.type = MeasurementAnchor::FREE;
                anchor.mesh = nullptr;
                float wx = reader.readF32();
                float wy = reader.readF32();
                float wz = reader.readF32();
                anchor.worldPos = glm::vec3(wx, wy, wz);
            }
            return anchor;
        };

        // Measure tool
        bool isMeasureVisibleV1 = reader.readU32() != 0;
        bool isMeasureVisibleV2 = reader.readU32() != 0;
        uint32_t nbMeasureSegments = reader.readU32();
        std::vector<MeasurementSegment> measureSegments;
        measureSegments.reserve(nbMeasureSegments);
        for (uint32_t s = 0; s < nbMeasureSegments; ++s) {
            MeasurementSegment seg;
            seg.vertA = readAnchor();
            seg.vertB = readAnchor();
            seg.isReference = reader.readU32() != 0;
            seg.name = "Measure " + std::to_string(s + 1);
            measureSegments.push_back(seg);
        }

        // Divider tool
        bool isDividerVisibleV1 = reader.readU32() != 0;
        bool isDividerVisibleV2 = reader.readU32() != 0;
        uint32_t dividerDivisions = reader.readU32();
        uint32_t nbDividerSegments = reader.readU32();
        std::vector<MeasurementSegment> dividerSegments;
        dividerSegments.reserve(nbDividerSegments);
        for (uint32_t s = 0; s < nbDividerSegments; ++s) {
            MeasurementSegment seg;
            seg.vertA = readAnchor();
            seg.vertB = readAnchor();
            seg.isReference = false;
            seg.name = "Divider " + std::to_string(s + 1);
            if (reader.hasData()) {
                uint32_t dVal = reader.readU32();
                seg.divisions = (dVal >= 2 && dVal <= 6) ? static_cast<int>(dVal) : (dividerDivisions > 0 ? static_cast<int>(dividerDivisions) : 3);
            } else {
                seg.divisions = (dividerDivisions > 0 ? static_cast<int>(dividerDivisions) : 3);
            }
            dividerSegments.push_back(seg);
        }

        if (sculpt) {
            sculpt->setMeasureVisibleV1(isMeasureVisibleV1);
            sculpt->setMeasureVisibleV2(isMeasureVisibleV2);
            sculpt->getMeasureSegments() = std::move(measureSegments);

            sculpt->setDividerVisibleV1(isDividerVisibleV1);
            sculpt->setDividerVisibleV2(isDividerVisibleV2);
            sculpt->setDividerDivisions(dividerDivisions > 0 ? (int)dividerDivisions : 3);
            sculpt->getDividerSegments() = std::move(dividerSegments);
        }
    } else {
        if (sculpt) {
            sculpt->setMeasureVisibleV1(true);
            sculpt->setMeasureVisibleV2(true);
            sculpt->getMeasureSegments().clear();

            sculpt->setDividerVisibleV1(true);
            sculpt->setDividerVisibleV2(true);
            sculpt->setDividerDivisions(3);
            sculpt->getDividerSegments().clear();
        }
    }

    if (version >= 9 && reader.hasData()) {
        scene.getCameraBookmarks().clear();
        uint32_t nbBookmarks = reader.readU32();
        for (uint32_t b = 0; b < nbBookmarks; ++b) {
            CameraBookmark bm;
            uint32_t nameLen = reader.readU32();
            if (nameLen > 0) {
                std::vector<uint8_t> nBytes(nameLen);
                reader.readBytes(nBytes.data(), nameLen);
                bm.name = std::string(nBytes.begin(), nBytes.end());
            }

            bm.camState.quatRot.x = reader.readF32();
            bm.camState.quatRot.y = reader.readF32();
            bm.camState.quatRot.z = reader.readF32();
            bm.camState.quatRot.w = reader.readF32();

            bm.camState.trans.x = reader.readF32();
            bm.camState.trans.y = reader.readF32();
            bm.camState.trans.z = reader.readF32();

            bm.camState.center.x = reader.readF32();
            bm.camState.center.y = reader.readF32();
            bm.camState.center.z = reader.readF32();

            bm.camState.offset.x = reader.readF32();
            bm.camState.offset.y = reader.readF32();
            bm.camState.offset.z = reader.readF32();

            bm.camState.rotX = reader.readF32();
            bm.camState.rotY = reader.readF32();

            bm.camState.fov = reader.readF32();
            uint32_t projType = reader.readU32();
            bm.camState.projectionType = (projType == 0 ? CameraEnums::Projection::PERSPECTIVE : CameraEnums::Projection::ORTHOGRAPHIC);
            uint32_t mode = reader.readU32();
            bm.camState.mode = static_cast<CameraEnums::CameraMode>(mode);
            bm.camState.usePivot = (reader.readU32() != 0);

            bm.camState.view2DOffsetX = reader.readF32();
            bm.camState.view2DOffsetY = reader.readF32();
            bm.camState.view2DZoom = reader.readF32();
            bm.camState.ref2DMode = (reader.readU32() != 0);
            bm.camState.refDrag = (reader.readU32() != 0);

            uint32_t nbRefSnaps = reader.readU32();
            for (uint32_t r = 0; r < nbRefSnaps; ++r) {
                RefImageSnapshot snap;
                uint32_t pathLen = reader.readU32();
                if (pathLen > 0) {
                    std::vector<uint8_t> pBytes(pathLen);
                    reader.readBytes(pBytes.data(), pathLen);
                    snap.path = std::string(pBytes.begin(), pBytes.end());
                }
                snap.visible   = (reader.readU32() != 0);
                snap.visibleV1 = (reader.readU32() != 0);
                snap.visibleV2 = (reader.readU32() != 0);
                snap.offsetX   = reader.readF32();
                snap.offsetY   = reader.readF32();
                snap.scale     = reader.readF32();
                snap.rotation  = reader.readF32();
                snap.opacity   = reader.readF32();
                bm.refImages.push_back(snap);
            }

            if (version >= 11) {
                uint32_t previewSize = reader.readU32();
                if (previewSize > 0) {
                    bm.previewData.resize(previewSize);
                    reader.readBytes(bm.previewData.data(), previewSize);
                }
            }

            if (version >= 15) {
                uint32_t nbHideSnaps = reader.readU32();
                for (uint32_t h = 0; h < nbHideSnaps; ++h) {
                    MeshFaceHideSnapshot snap;
                    snap.meshId = reader.readU32();
                    uint32_t mNameLen = reader.readU32();
                    if (mNameLen > 0) {
                        std::vector<uint8_t> nBytes(mNameLen);
                        reader.readBytes(nBytes.data(), mNameLen);
                        snap.meshName = std::string(nBytes.begin(), nBytes.end());
                    }
                    snap.visibleV1 = (reader.readU32() != 0);
                    snap.visibleV2 = (reader.readU32() != 0);
                    uint32_t fCount = reader.readU32();
                    if (fCount > 0) {
                        snap.faceVisible.resize(fCount);
                        reader.readBytes(snap.faceVisible.data(), fCount);
                    }
                    bm.meshHideSnapshots.push_back(snap);
                }
            }

            scene.addBookmark(bm);
        }
    }

    if (version >= 10 && reader.hasData()) {
        for (auto& oldImg : scene.getReferenceImages()) {
            if (oldImg.texId != 0) {
                glDeleteTextures(1, &oldImg.texId);
            }
        }
        scene.getReferenceImages().clear();

        uint32_t nbRefImages = reader.readU32();
        for (uint32_t r = 0; r < nbRefImages; ++r) {
            ReferenceImage img;
            uint32_t pathLen = reader.readU32();
            if (pathLen > 0) {
                std::vector<uint8_t> pBytes(pathLen);
                reader.readBytes(pBytes.data(), pathLen);
                img.path = std::string(pBytes.begin(), pBytes.end());
            }

            img.visible   = (reader.readU32() != 0);
            img.visibleV1 = (reader.readU32() != 0);
            img.visibleV2 = (reader.readU32() != 0);
            img.pinned2D  = (reader.readU32() != 0);
            img.locked    = (reader.readU32() != 0);

            img.opacity   = reader.readF32();
            img.scale     = reader.readF32();
            img.offsetX   = reader.readF32();
            img.offsetY   = reader.readF32();
            img.rotation  = reader.readF32();

            uint32_t embedSize = reader.readU32();
            if (embedSize > 0) {
                img.embeddedData.resize(embedSize);
                reader.readBytes(img.embeddedData.data(), embedSize);
            }

            int w = 0, h = 0;
            GLuint texId = 0;
            if (!img.path.empty()) {
                texId = loadTextureFromFile(img.path, &w, &h);
            }
            if (texId == 0 && !img.embeddedData.empty()) {
                texId = loadTextureFromMemory(img.embeddedData.data(), img.embeddedData.size(), &w, &h);
            }

            if (texId != 0) {
                img.texId = texId;
                img.width = w;
                img.height = h;
                scene.getReferenceImages().push_back(img);
            }
        }
    }

    return meshes;
}

} // namespace ImportSGL

