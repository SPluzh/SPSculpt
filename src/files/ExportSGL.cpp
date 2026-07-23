#include "files/ExportSGL.h"
#include "common/Constants.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

namespace ExportSGL {

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

std::vector<uint8_t> exportSGL(const std::vector<Mesh*>& meshes, const Scene& scene, const AngleRenderer& renderer) {
    BinaryWriter writer;
    
    // Version 6
    writer.writeU32(6);

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
    }

    // Measure tool stubs
    writer.writeU32(1); // isMeasureVisibleV1 = 1
    writer.writeU32(1); // isMeasureVisibleV2 = 1
    writer.writeU32(0); // measureSegments.length = 0

    // Divider tool stubs
    writer.writeU32(1); // isDividerVisibleV1 = 1
    writer.writeU32(1); // isDividerVisibleV2 = 1
    writer.writeU32(3); // dividerDivisions = 3
    writer.writeU32(0); // dividerSegments.length = 0

    return writer.getBuffer();
}

} // namespace ExportSGL
