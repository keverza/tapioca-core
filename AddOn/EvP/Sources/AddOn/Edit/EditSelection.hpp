#ifndef EVP_EDIT_EDITSELECTION_HPP
#define EVP_EDIT_EDITSELECTION_HPP

// What the user has picked and what they can do with it. Picking happens in screen
// pixels through a projector the host supplies (the plan's transform, the 3D camera,
// or a panel canvas), so the same rules hold in every view. Context actions are rows
// in a table keyed by what is selected and where, so a tool adds an action by adding
// a row.
#include "Edit/EditSession.hpp"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::edit {

enum PickFilter : uint32_t {
    kPickVertex = 1u << 0,
    kPickSegment = 1u << 1,
    kPickFace = 1u << 2,
    kPickEntity = 1u << 3,
    kPickAll = 0xFu,
};

struct PickView {
    // World to screen pixels; false when the point is behind the eye.
    std::function<bool (Vec3 world, double& x, double& y)> toScreen;
    // Direction the view looks along; faces turned away from it cannot be picked.
    Vec3 viewDirection { 0.0, 0.0, -1.0 };
};

struct PickHit {
    Ref ref;
    double distance = 0.0; // pixels
    // Hit only because the pointer is inside a closed outline, with no edge of it near.
    // Its distance is `radius` plus the pixels to that outline, so any edge within the
    // radius outranks it and, of nested outlines, the innermost wins.
    bool inside = false;
};

// A caller-owned cache of the MODEL-SPACE path outlines a Pick needs, so a pick that only
// moved the camera, the cursor or the selection does not re-tessellate every path's
// `Outline`. It holds no screen position, no hit, no camera and touches no thread global,
// and it is bounded to one entry per path in the document. Its key is the document revision
// for the cheap reset AND the cached path value for correctness when a different document
// arrives with the same revision or ids: an entry is reused only while the path's own value
// is unchanged, so a stale outline cannot outlive its input. Callers that pass nothing keep
// the uncached Pick.
struct PickGeometryCache {
    struct Entry {
        Id path = kNoId;
        Path input; // the path the outline was built from; compared on every lookup
        PathOutline outline;
        uint64_t touched = 0; // the pick that last used it, for the prune
    };
    double step = -1.0; // the outline quality the entries were built at; a different one rebuilds
    uint64_t revision = 0;
    uint64_t builds = 0; // outlines actually tessellated: the misses this cache paid
    uint64_t tick = 0;   // the current pick, stamped into each entry it uses
    std::vector<Entry> entries;
};

// Hits within `radius` pixels, ordered vertex > segment > face > entity, then nearest.
// A click inside a closed outline (or on any of its edges) hits the entity; an inside-only
// hit ranks after every entity whose edge is within the radius. `cache` is
// optional and spares only the paths' outline tessellation: the projection, the scan and
// the returned hits are identical with or without it.
std::vector<PickHit> Pick (const Document& doc, const PickView& view, double x, double y, uint32_t filter,
                           double radius, PickGeometryCache* cache = nullptr);

enum class SelectionKind : uint8_t {
    None,
    Vertex,
    Segment,
    Face,
    Entity,
    Mixed,
    PathPoint,
    Break,
    Range,
    LandscapeFace
};

class Selection {
  public:
    const std::vector<Ref>& Items () const;
    bool Contains (const Ref& ref) const;
    SelectionKind Kind () const;
    void Clear ();
    void Set (const std::vector<Ref>& refs);
    void Toggle (const Ref& ref);
    // A click selects the best hit; clicking again at the same spot with the same
    // candidates cycles to the next one, so stacked items stay reachable. With
    // `additive` the hit is toggled into the selection instead.
    void Click (const std::vector<PickHit>& hits, double x, double y, bool additive);
    // Dragging right selects what lies fully inside the rectangle; dragging left
    // (x1 < x0) also selects what the rectangle touches. A filter that admits whole objects
    // selects whole objects only: parts are taken only when the filter excludes objects.
    void Marquee (const Document& doc, const PickView& view, double x0, double y0, double x1, double y1,
                  uint32_t filter, bool additive);
    // What lies fully inside a free-hand screen outline (closed implicitly), by the same
    // filter rule as the marquee. Fewer than three points select nothing.
    void Lasso (const Document& doc, const PickView& view, const std::vector<std::pair<double, double>>& outline,
                uint32_t filter, bool additive);
    // Drops references that an edit or an undo removed, hid or locked.
    void Prune (const Document& doc);

    Ref hover;

  private:
    std::vector<Ref> items;
    std::vector<PickHit> lastHits;
    double lastX = 0.0, lastY = 0.0;
    size_t cycle = 0;
};

enum class ViewKind : uint8_t { Plan, ThreeD, Profile, Section };

constexpr uint32_t Bit (SelectionKind kind)
{
    return 1u << static_cast<uint32_t> (kind);
}
constexpr uint32_t Bit (ViewKind view)
{
    return 1u << static_cast<uint32_t> (view);
}

struct Action {
    std::string id, label;
    uint32_t kinds = 0;                // Bit(SelectionKind) set
    uint32_t views = 0xFu;             // Bit(ViewKind) set
    size_t minItems = 1, maxItems = 0; // 0: no upper bound
    // Builds the operation for this selection; an empty Operation means not applicable.
    std::function<Operation (const Document&, const std::vector<Ref>&)> make;
};
// Rows that apply to the selection in this view, in table order.
std::vector<const Action*> Applicable (const std::vector<Action>& table, const Selection& selection, ViewKind view);

} // namespace geomsrv::edit
#endif
