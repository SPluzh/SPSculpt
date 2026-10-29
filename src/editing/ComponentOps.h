#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

class Mesh;
class SelectionManager;

enum class BevelType {
    Chamfer = 0,
    Solid = 1
};

class ComponentOps {
public:
    // Delete selected components (vertices, edges, or faces).
    // Cleans up orphan vertices and completely rebuilds mesh topology and octree.
    static bool deleteSelection(Mesh& mesh, SelectionManager& selMgr);

    // Extrude selected faces or edges along their normal by offsetMagnitude.
    // Side quads are generated, and newly extruded caps remain selected.
    static bool extrudeSelection(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude);

    // Bevel selected components (Face Inset/Bevel or Edge Chamfer).
    // width: bevel width/offset
    // segments: number of cuts/divisions along the bevel profile (1..10)
    // type: Chamfer (replaces edge with flat/rounded fillet) or Solid (keeps edge sharp, inserts support loops)
    // heightOffset: normal displacement offset (positive for outward bevel, negative for inset)
    static bool bevelSelection(Mesh& mesh, SelectionManager& selMgr, float width, int segments = 1, BevelType type = BevelType::Chamfer, float heightOffset = 0.0f);

    // Bridge: Connects 2 selected faces (with a hollow quad tunnel) or 2 selected edges (with a quad strip)
    static bool bridgeSelection(Mesh& mesh, SelectionManager& selMgr);
    static bool bridgeFaces(Mesh& mesh, SelectionManager& selMgr);
    static bool bridgeEdges(Mesh& mesh, SelectionManager& selMgr);

    // Merge / Collapse:
    // Merges all selected vertices (or vertices of selected components) to their centroid
    static bool mergeToCenter(Mesh& mesh, SelectionManager& selMgr);

    // Collapses selected edges into vertices (quads become triangles, triangles are deleted)
    static bool collapseEdges(Mesh& mesh, SelectionManager& selMgr);

    // Close Holes: Detects open boundary loops and fills them with caps
    // If components are selected, closes holes touching the selection; otherwise closes all open holes in the mesh.
    static bool closeHoles(Mesh& mesh, SelectionManager& selMgr);

    // Specific helpers
    static bool extrudeFaces(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude);
    static bool extrudeEdges(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude);
    static bool bevelFaces(Mesh& mesh, SelectionManager& selMgr, float width, int segments = 1, BevelType type = BevelType::Chamfer, float heightOffset = 0.0f);
    static bool bevelEdges(Mesh& mesh, SelectionManager& selMgr, float width, int segments = 1, BevelType type = BevelType::Chamfer, float heightOffset = 0.0f);
};
