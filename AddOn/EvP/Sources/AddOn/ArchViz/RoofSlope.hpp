#ifndef EVP_ARCHVIZ_ROOFSLOPE_HPP
#define EVP_ARCHVIZ_ROOFSLOPE_HPP

// Virtual roof cells, shared elevation nodes and transactional edits. Uses the massing
// topology's Ring/Vec and Clipper2, but none of its apartment or wall-offset rules.
// Coordinates are roof-local XY in metres. Only World crosses the overlay boundary.
#include "ArchViz/FloorScheme.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::archviz::roofslope {
using floorscheme::Ring;
using floorscheme::Vec;
constexpr char kLayer[] = "slope-designer-roof";
constexpr double kMaxAspect = 1.5; // roof envelopes are near-square, not long grading strips
struct Node {
    Vec xy;
    double z = 0;
    bool lockXY = false, lockZ = false;
};
struct Cell {
    std::array<double, 4> box {}; // u0, v0, u1, v1; divider centre lines
    std::vector<int> rim;
    int drain = -1;
    double area = 0;
    bool rectangle = false;
    bool drainInside = true;
};
struct Triangle {
    std::array<int, 3> nodes {};
    std::array<Node, 3> vertices {};
    int cell = -1;
    double gx = 0, gy = 0, area = 0;
};
struct Data {
    Vec origin;
    double bearing = 0, topZ = 0, ridgeZ = 0, cap = 200, targetPercent = 2, textHeight = 0.18;
    std::vector<Ring> outline; // local, outer CCW and holes CW
    std::vector<Node> nodes;
    std::vector<Cell> cells;
    std::vector<Triangle> triangles;
};
struct Snap {
    Vec at;
    std::string label;
    Vec guideFrom;
    bool guide = false;
};
bool Plan (const std::vector<Ring>& worldOutline, double topZ, double percent, double cap, Data& out,
           std::string& error, std::optional<double> bearing = {});
Vec World (const Data& data, Vec local);
Vec Local (const Data& data, Vec world);
bool Rebuild (Data& data, std::string& error);
bool SetElevation (Data& data, int node, double z, std::string& error);
bool MoveDrain (Data& data, int cell, Vec xy, std::string& error);
// Moves every connected divider at that coordinate. Refuses footprint borders,
// collapsed cells, locked nodes and a cap/coverage violation instead of warping a box.
bool MoveDivider (Data& data, int cell, int side, double value, std::string& error);
bool RemoveDivider (Data& data, int cell, int side, std::string& error);
bool EditOutline (Data& data, int ring, int vertex, Vec at, bool insert, bool remove, std::string& error);
bool LiftCell (Data& data, int cell, double delta, std::string& error);
bool Reorient (Data& data, double bearing, std::string& error);
Snap Infer (const Data& data, Vec at, Vec anchor, double tolerance, int exclude = -1);
overlaylayers::Layer Layer (const Data& data);
overlaylayers::Panel PanelStyle ();
overlaylayers::Panel EditorStyle ();
struct Measures {
    double maxAspect = 0, maxSpan = 0, maxBorderRise = 0;
    int faces = 0;
};
Measures Measure (const Data& data);

// Gesture updates are evaluated from `before`, not accumulated screen deltas.
// Commit creates one virtual undo entry; rejected updates leave the last valid state.
struct Editor {
    Data data, before;
    std::vector<Data> undo, redo;
    uint64_t revision = 1, published = 0;
    uint64_t startedRevision = 0;
    bool gesture = false;
    int selectedCell = -1, selectedNode = -1, selectedSide = -1;
    std::string message;
    void Begin ();
    void Commit ();
    void Cancel ();
    bool Undo (bool backwards);
    bool Apply (Data candidate, const std::string& error = {});
};
} // namespace geomsrv::archviz::roofslope
#endif
