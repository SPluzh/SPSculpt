#include "files/ExportSGL.h"
#include "common/Constants.h"
#include "common/FormatConstants.h"
#include "common/StringUtils.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>

namespace ExportSGL {

static std::vector<uint8_t> readBinaryFileHelper(const std::string& path) {
    if (path.empty()) return {};
#ifdef _WIN32
    std::ifstream file(utf8ToWide(path).c_str(), std::ios::binary | std::ios::ate);
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) return {};
    std::streamsize size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }
    return {};
}

class BinaryWriter {

    std::vector<uint8_t> m_buffer;
public:
    void writeU32(uint32_t val) {
        uint8_t bytes[4];
        std::memcpy(bytes, &val, 4);
        m_buffer.insert(m_buffer.end(), bytes, bytes + 4);
    }
    
    void writeI32(int32_t val) {
        uint8_t bytes[4];
        std::memcpy(bytes, &val, 4);
        m_buffer.insert(m_buffer.end(), bytes, bytes + 4);
    }
    
    void writeF32(float val) {
        uint8_t bytes[4];
        std::memcpy(bytes, &val, 4);
        m_buffer.insert(m_buffer.end(), bytes, bytes + 4);
    }
    
    void writeF32Array(const float* data, size_t count) {
        if (count == 0) return;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        m_buffer.insert(m_buffer.end(), bytes, bytes + count * 4);
    }
    
    void writeU32Array(const uint32_t* data, size_t count) {
        if (count == 0) return;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        m_buffer.insert(m_buffer.end(), bytes, bytes + count * 4);
    }
    
    void writeBytes(const uint8_t* data, size_t count) {
        if (count == 0) return;
        m_buffer.insert(m_buffer.end(), data, data + count);
        size_t rem = count % 4;
        if (rem > 0) {
            for (size_t i = 0; i < 4 - rem; ++i) {
                m_buffer.push_back(0);
            }
        }
    }

    const std::vector<uint8_t>& getBuffer() const { return m_buffer; }
};

std::vector<uint8_t> exportSGL(const std::vector<Mesh*>& meshes, const Scene& scene, const AngleRenderer& renderer, const SculptManager& sculpt) {
    BinaryWriter writer;
    
    // Magic "SPSC" + Version 14
    writer.writeBytes(reinterpret_cast<const uint8_t*>("SPSC"), 4);
    writer.writeU32(Format::CURRENT_VERSION);

    // Misc settings
    writer.writeU32(renderer.getShowGrid() ? 1 : 0);
    writer.writeU32(renderer.getShowSymmetryLine() ? 1 : 0);
    writer.writeU32(renderer.getShowContour() ? 1 : 0);

    // Camera settings
    const Camera& cam = scene.getCamera();
    writer.writeU32(cam.getProjectionType() == CameraEnums::Projection::PERSPECTIVE ? 0 : 1);
    writer.writeU32(static_cast<uint32_t>(cam.getMode()));
    writer.writeF32(cam.getFov());
    writer.writeU32(cam.getUsePivot() ? 1 : 0);

    // Camera transform & state (Version >= 12)
    Camera::CameraState camState = cam.getCurrentState();
    writer.writeF32(camState.quatRot.x);
    writer.writeF32(camState.quatRot.y);
    writer.writeF32(camState.quatRot.z);
    writer.writeF32(camState.quatRot.w);

    writer.writeF32(camState.trans.x);
    writer.writeF32(camState.trans.y);
    writer.writeF32(camState.trans.z);

    writer.writeF32(camState.center.x);
    writer.writeF32(camState.center.y);
    writer.writeF32(camState.center.z);

    writer.writeF32(camState.offset.x);
    writer.writeF32(camState.offset.y);
    writer.writeF32(camState.offset.z);

    writer.writeF32(camState.rotX);
    writer.writeF32(camState.rotY);

    writer.writeF32(camState.view2DOffsetX);
    writer.writeF32(camState.view2DOffsetY);
    writer.writeF32(camState.view2DZoom);
    writer.writeU32(camState.ref2DMode ? 1 : 0);
    writer.writeU32(camState.refDrag ? 1 : 0);

    // Meshes
    uint32_t nbMeshes = static_cast<uint32_t>(meshes.size());
    writer.writeU32(nbMeshes);

    for (const auto* mesh : meshes) {
        if (!mesh) continue;

        // Render settings
        writer.writeU32(renderer.getShaderType());
        writer.writeU32(renderer.getMatcap());
        writer.writeU32(renderer.getShowWireframe() ? 1 : 0);
        writer.writeU32(renderer.getFlatShading() ? 1 : 0);
        writer.writeF32(renderer.getAlpha());

        // Visibility
        writer.writeU32(mesh->isVisible(0) ? 1 : 0);
        writer.writeU32(mesh->isVisible(1) ? 1 : 0);

        // Center, matrix, scale
        writer.writeF32(mesh->getCenter().x);
        writer.writeF32(mesh->getCenter().y);
        writer.writeF32(mesh->getCenter().z);
        writer.writeF32Array(glm::value_ptr(mesh->matrix), 16);
        writer.writeF32(mesh->getScale());

        // Vertices
        uint32_t nbVertices = mesh->nbVerts;
        writer.writeU32(nbVertices);
        writer.writeF32Array(mesh->verts.data(), nbVertices * 3);

        // Vertex visibility (padded to 4-byte boundaries)
        std::vector<uint8_t> vertVis(nbVertices, 1);
        if (!mesh->vertVisible.empty()) {
            std::copy(mesh->vertVisible.begin(), mesh->vertVisible.end(), vertVis.begin());
        }
        writer.writeBytes(vertVis.data(), nbVertices);

        // Colors
        uint32_t nbColors = (mesh->colors.size() == nbVertices * 3) ? nbVertices : 0;
        writer.writeU32(nbColors);
        if (nbColors > 0) {
            writer.writeF32Array(mesh->colors.data(), nbVertices * 3);
        }

        // Materials
        uint32_t nbMaterials = (mesh->materials.size() == nbVertices * 3) ? nbVertices : 0;
        writer.writeU32(nbMaterials);
        if (nbMaterials > 0) {
            writer.writeF32Array(mesh->materials.data(), nbVertices * 3);
        }

        // Faces
        uint32_t nbFaces = mesh->nbFaces;
        writer.writeU32(nbFaces);
        writer.writeU32Array(mesh->faces.data(), nbFaces * 4);

        // UVs
        bool hasUV = mesh->hasUV && !mesh->texCoords.empty() && !mesh->facesTexCoord.empty();
        uint32_t nbTexCoords = hasUV ? mesh->getNbTexCoords() : 0;
        writer.writeU32(nbTexCoords);
        if (nbTexCoords > 0) {
            writer.writeF32Array(mesh->texCoords.data(), nbTexCoords * 2);
        }

        // Face UVs
        uint32_t nbFacesTexCoords = hasUV ? nbFaces : 0;
        writer.writeU32(nbFacesTexCoords);
        if (nbFacesTexCoords > 0) {
            writer.writeU32Array(mesh->facesTexCoord.data(), nbFaces * 4);
        }

        // Layers (Version >= 7)
        const auto& layerStack = mesh->layerStack;
        uint32_t nbLayers = static_cast<uint32_t>(layerStack.count());
        writer.writeU32(nbLayers);
        writer.writeI32(layerStack.getActiveIdx());
        for (int lIdx = 0; lIdx < static_cast<int>(nbLayers); ++lIdx) {
            const auto& layer = layerStack.at(lIdx);
            uint32_t nameLen = static_cast<uint32_t>(layer.name.size());
            writer.writeU32(nameLen);
            writer.writeBytes(reinterpret_cast<const uint8_t*>(layer.name.data()), nameLen);
            writer.writeU32(layer.visible ? 1 : 0);
            writer.writeF32(layer.intensity);
            uint32_t nbDelta = static_cast<uint32_t>(layer.deltaVerts.size());
            writer.writeU32(nbDelta);
            if (nbDelta > 0) {
                writer.writeF32Array(layer.deltaVerts.data(), nbDelta);
            }
        }

        // Topology (Version >= 13)
        uint32_t nVrfStartCount = static_cast<uint32_t>(mesh->vrfStartCount.size());
        writer.writeU32(nVrfStartCount);
        if (nVrfStartCount > 0) {
            writer.writeU32Array(mesh->vrfStartCount.data(), nVrfStartCount);
        }

        uint32_t nVertRingFace = static_cast<uint32_t>(mesh->vertRingFace.size());
        writer.writeU32(nVertRingFace);
        if (nVertRingFace > 0) {
            writer.writeU32Array(mesh->vertRingFace.data(), nVertRingFace);
        }

        uint32_t nVrvStartCount = static_cast<uint32_t>(mesh->vrvStartCount.size());
        writer.writeU32(nVrvStartCount);
        if (nVrvStartCount > 0) {
            writer.writeU32Array(mesh->vrvStartCount.data(), nVrvStartCount);
        }

        uint32_t nVertRingVert = static_cast<uint32_t>(mesh->vertRingVert.size());
        writer.writeU32(nVertRingVert);
        if (nVertRingVert > 0) {
            writer.writeU32Array(mesh->vertRingVert.data(), nVertRingVert);
        }

        uint32_t nVertOnEdge = static_cast<uint32_t>(mesh->vertOnEdge.size());
        writer.writeU32(nVertOnEdge);
        if (nVertOnEdge > 0) {
            writer.writeBytes(mesh->vertOnEdge.data(), nVertOnEdge);
        }
    }

    auto writeAnchor = [&](const MeasurementAnchor& anchor) {
        if (anchor.type == MeasurementAnchor::VERTEX && anchor.mesh != nullptr) {
            writer.writeU32(0); // vertex type
            int meshIdx = -1;
            for (size_t m = 0; m < meshes.size(); ++m) {
                if (meshes[m] == anchor.mesh) {
                    meshIdx = static_cast<int>(m);
                    break;
                }
            }
            writer.writeU32(meshIdx >= 0 ? static_cast<uint32_t>(meshIdx) : 0);
            writer.writeU32(anchor.vertIdx);
            writer.writeU32(0); // unused padding
        } else {
            writer.writeU32(1); // free type
            writer.writeF32(anchor.worldPos.x);
            writer.writeF32(anchor.worldPos.y);
            writer.writeF32(anchor.worldPos.z);
        }
    };

    // Measure tool data
    writer.writeU32(sculpt.getMeasureVisibleV1() ? 1 : 0);
    writer.writeU32(sculpt.getMeasureVisibleV2() ? 1 : 0);
    const auto& measureSegments = sculpt.getMeasureSegments();
    writer.writeU32(static_cast<uint32_t>(measureSegments.size()));
    for (const auto& seg : measureSegments) {
        writeAnchor(seg.vertA);
        writeAnchor(seg.vertB);
        writer.writeU32(seg.isReference ? 1 : 0);
    }

    // Divider tool data
    writer.writeU32(sculpt.getDividerVisibleV1() ? 1 : 0);
    writer.writeU32(sculpt.getDividerVisibleV2() ? 1 : 0);
    writer.writeU32(static_cast<uint32_t>(sculpt.getDividerDivisions()));
    const auto& dividerSegments = sculpt.getDividerSegments();
    writer.writeU32(static_cast<uint32_t>(dividerSegments.size()));
    for (const auto& seg : dividerSegments) {
        writeAnchor(seg.vertA);
        writeAnchor(seg.vertB);
        writer.writeU32(static_cast<uint32_t>(seg.divisions));
    }

    // Camera Bookmarks (Version >= 9)
    const auto& bookmarks = scene.getCameraBookmarks();
    writer.writeU32(static_cast<uint32_t>(bookmarks.size()));
    for (const auto& bm : bookmarks) {
        uint32_t nameLen = static_cast<uint32_t>(bm.name.size());
        writer.writeU32(nameLen);
        writer.writeBytes(reinterpret_cast<const uint8_t*>(bm.name.data()), nameLen);

        writer.writeF32(bm.camState.quatRot.x);
        writer.writeF32(bm.camState.quatRot.y);
        writer.writeF32(bm.camState.quatRot.z);
        writer.writeF32(bm.camState.quatRot.w);

        writer.writeF32(bm.camState.trans.x);
        writer.writeF32(bm.camState.trans.y);
        writer.writeF32(bm.camState.trans.z);

        writer.writeF32(bm.camState.center.x);
        writer.writeF32(bm.camState.center.y);
        writer.writeF32(bm.camState.center.z);

        writer.writeF32(bm.camState.offset.x);
        writer.writeF32(bm.camState.offset.y);
        writer.writeF32(bm.camState.offset.z);

        writer.writeF32(bm.camState.rotX);
        writer.writeF32(bm.camState.rotY);

        writer.writeF32(bm.camState.fov);
        writer.writeU32(bm.camState.projectionType == CameraEnums::Projection::PERSPECTIVE ? 0 : 1);
        writer.writeU32(static_cast<uint32_t>(bm.camState.mode));
        writer.writeU32(bm.camState.usePivot ? 1 : 0);

        writer.writeF32(bm.camState.view2DOffsetX);
        writer.writeF32(bm.camState.view2DOffsetY);
        writer.writeF32(bm.camState.view2DZoom);
        writer.writeU32(bm.camState.ref2DMode ? 1 : 0);
        writer.writeU32(bm.camState.refDrag ? 1 : 0);

        writer.writeU32(static_cast<uint32_t>(bm.refImages.size()));
        for (const auto& snap : bm.refImages) {
            uint32_t pathLen = static_cast<uint32_t>(snap.path.size());
            writer.writeU32(pathLen);
            writer.writeBytes(reinterpret_cast<const uint8_t*>(snap.path.data()), pathLen);

            writer.writeU32(snap.visible ? 1 : 0);
            writer.writeU32(snap.visibleV1 ? 1 : 0);
            writer.writeU32(snap.visibleV2 ? 1 : 0);
            writer.writeF32(snap.offsetX);
            writer.writeF32(snap.offsetY);
            writer.writeF32(snap.scale);
            writer.writeF32(snap.rotation);
            writer.writeF32(snap.opacity);
        }

        uint32_t previewSize = static_cast<uint32_t>(bm.previewData.size());
        writer.writeU32(previewSize);
        if (previewSize > 0) {
            writer.writeBytes(bm.previewData.data(), previewSize);
        }

        // Polygon Hiding Snapshots (Version >= 15)
        uint32_t nbHideSnaps = static_cast<uint32_t>(bm.meshHideSnapshots.size());
        writer.writeU32(nbHideSnaps);
        for (const auto& snap : bm.meshHideSnapshots) {
            writer.writeU32(snap.meshId);
            uint32_t mNameLen = static_cast<uint32_t>(snap.meshName.size());
            writer.writeU32(mNameLen);
            if (mNameLen > 0) {
                writer.writeBytes(reinterpret_cast<const uint8_t*>(snap.meshName.data()), mNameLen);
            }
            writer.writeU32(snap.visibleV1 ? 1 : 0);
            writer.writeU32(snap.visibleV2 ? 1 : 0);
            uint32_t fCount = static_cast<uint32_t>(snap.faceVisible.size());
            writer.writeU32(fCount);
            if (fCount > 0) {
                writer.writeBytes(snap.faceVisible.data(), fCount);
            }
        }
    }

    // Scene Reference Images (Version >= 10)
    const auto& refImages = scene.getReferenceImages();
    writer.writeU32(static_cast<uint32_t>(refImages.size()));
    for (const auto& img : refImages) {
        uint32_t pathLen = static_cast<uint32_t>(img.path.size());
        writer.writeU32(pathLen);
        if (pathLen > 0) {
            writer.writeBytes(reinterpret_cast<const uint8_t*>(img.path.data()), pathLen);
        }

        writer.writeU32(img.visible ? 1 : 0);
        writer.writeU32(img.visibleV1 ? 1 : 0);
        writer.writeU32(img.visibleV2 ? 1 : 0);
        writer.writeU32(img.pinned2D ? 1 : 0);
        writer.writeU32(img.locked ? 1 : 0);

        writer.writeF32(img.opacity);
        writer.writeF32(img.scale);
        writer.writeF32(img.offsetX);
        writer.writeF32(img.offsetY);
        writer.writeF32(img.rotation);

        std::vector<uint8_t> embedData = img.embeddedData;
        if (embedData.empty() && !img.path.empty()) {
            embedData = readBinaryFileHelper(img.path);
        }

        uint32_t embedSize = static_cast<uint32_t>(embedData.size());
        writer.writeU32(embedSize);
        if (embedSize > 0) {
            writer.writeBytes(embedData.data(), embedSize);
        }
    }

    return writer.getBuffer();
}

} // namespace ExportSGL

