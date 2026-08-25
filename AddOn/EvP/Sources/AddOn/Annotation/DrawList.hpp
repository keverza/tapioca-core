#ifndef EVP_ANNOTATION_DRAWLIST_HPP
#define EVP_ANNOTATION_DRAWLIST_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv {
namespace annotation {

struct Point3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ColourRgba {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 255;
};

enum class SemanticRole {
    None,
    Add,
    Remove,
    Modify,
    Context,
    Guide,
};

ColourRgba RoleColour (SemanticRole role);
uint32_t PackRgba (ColourRgba colour);

struct ScreenPoint {
    double x = 0.0;
    double y = 0.0;
};

struct ScreenTriangle {
    ScreenPoint first;
    ScreenPoint second;
    ScreenPoint third;
};

struct ArchitecturalAngleGlyph {
    std::vector<ScreenPoint> arc;
    ScreenTriangle arrowheads[2];
    ScreenPoint labelAnchor;
    double degrees = 0.0;
    double arcWidthPixels = 1.0;
    double fontSizePixels = 18.0;
};

// Constructs fixed-pixel annotation furniture from already projected anchors.
// Arrowheads are tangent to the signed minor arc and the label lies on its radial bisector.
bool BuildArchitecturalAngleGlyph (const ScreenPoint& center, const ScreenPoint& firstRayPoint,
                                   const ScreenPoint& secondRayPoint, double dpiScale, ArchitecturalAngleGlyph& glyph);

enum class PrimitiveKind {
    Point,
    Polyline,
    Arrow,
    Dimension,
    Angle,
    Label,
    Element,
};

struct Primitive {
    PrimitiveKind kind = PrimitiveKind::Point;
    SemanticRole role = SemanticRole::None;
    std::vector<Point3> points;
    std::string text;
    std::string guid;
    bool closed = false;
    bool direction = false;
    double offset = 0.0;
};

struct Frame {
    uint32_t index = 0;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string name;
    std::vector<Frame> frames;
};

class DrawList {
  public:
    std::vector<Node> nodes;

    bool SelectNode (size_t nodeIndex);
    bool SelectFrame (size_t nodeIndex, size_t frameIndex);
    void ClearSelection ();

    const Node* SelectedNode () const;
    const Frame* SelectedFrame () const;

    // Invalid primitives are not partly included: bounds describe only geometry
    // that a renderer can draw as a complete primitive.
    bool GetBounds (Point3& minimum, Point3& maximum) const;

  private:
    std::optional<size_t> selectedNode_;
    std::optional<size_t> selectedFrame_;
};

bool IsFinite (const Point3& point);
bool IsDrawable (const Primitive& primitive);
bool GetBounds (const Frame& frame, Point3& minimum, Point3& maximum);

struct Transform2D {
    double scaleX = 1.0;
    double scaleY = -1.0;
    double offX = 0.0;
    double offY = 0.0;
    double xFromY = 0.0;
    double yFromX = 0.0;
};

// Fits drawable XY geometry inside a pixel surface without stretching it.
// Returns false, and leaves `transform` unchanged, when the frame has no bounds.
bool FitFrame (const Frame& frame, double width, double height, double margin, Transform2D& transform);

// Produces {tip, left endpoint, tip, right endpoint}. `planeNormal` fixes the
// arrowhead plane; all vectors and lengths are in the caller's coordinate unit.
// On refusal, `outLegs` is unchanged.
bool BuildArrowheadLegs (const Point3& tail, const Point3& tip, const Point3& planeNormal, double legLength,
                         double halfAngleRadians, std::vector<Point3>& outLegs);

// Samples the minor oriented arc from firstRayPoint to secondRayPoint, including
// both endpoints. `segmentCount` is exact and limited to 4096. On refusal,
// `outPoints` is unchanged.
bool SampleAngleArc (const Point3& center, const Point3& firstRayPoint, const Point3& secondRayPoint,
                     const Point3& planeNormal, double radius, size_t segmentCount, std::vector<Point3>& outPoints);

} // namespace annotation
} // namespace geomsrv

#endif
