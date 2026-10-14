#ifndef EVP_ARCHVIZ_EDITLAYERS_HPP
#define EVP_ARCHVIZ_EDITLAYERS_HPP

// The virtual editor drawn as one overlay layer: its geometry, what is selected and
// hovered, the snap marker with its guides and hint, live dimensions, and the gizmo.
// Rebuilt from the session whenever its revision or the feedback changes; the layer is
// command-scoped, so it disappears with the editing command. Both overlays draw it
// through their own transforms; nothing here knows a camera.
#include "ArchViz/OverlayLayers.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditSnap.hpp"
#include <string>
#include <vector>

namespace geomsrv::archviz::editlayers {

constexpr char kLayer[] = "virtual-editor";

struct Style {
    uint32_t geometry = 0x3FA7F5FFu, selected = 0xFFB020FFu, hover = 0xFFFFFFFFu;
    uint32_t solid = 0x3FA7F540u;
    uint32_t guide = 0xB46EF0FFu, snap = 0xFFD60AFFu;
    uint32_t axis[3] = { 0xE5484DFFu, 0x46A758FFu, 0x3E63DDFFu };
    uint32_t activeHandle = 0xFFD60AFFu;
    uint32_t text = 0xFFFFFFFFu;
    float lineWidth = 2.0f, selectedWidth = 3.5f, vertexPixels = 7.0f;
    float textPixels = 12.0f;
};

struct GizmoView {
    bool shown = false;
    edit::Frame frame;
    edit::GizmoMode mode = edit::GizmoMode::Move;
    double size = 1.0; // metres: the host's metres per pixel times the wanted pixel size
    int active = -1;   // the handle being hovered or dragged
};

struct Feedback {
    const edit::Selection* selection = nullptr;
    const edit::SnapResult* snap = nullptr;
    std::vector<edit::Dimension> dimensions;
    GizmoView gizmo;
};

// "4.20 m", "2.0 %", "53.1°": what a dimension label says.
std::string FormatValue (edit::Quantity quantity, double value);

// Triangles of a counter-clockwise plan polygon (ear clipping), as index triples.
std::vector<uint32_t> TriangulateXY (const std::vector<edit::Vec3>& polygon);

overlaylayers::Layer Build (const edit::Document& doc, const Feedback& feedback, const Style& style = {},
                            const std::string& name = kLayer);

} // namespace geomsrv::archviz::editlayers
#endif
