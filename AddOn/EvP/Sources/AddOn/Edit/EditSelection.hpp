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
};
// Hits within `radius` pixels, ordered vertex > segment > face > entity, then nearest.
// A click inside a closed outline (or on any of its edges) hits the entity.
std::vector<PickHit> Pick (const Document& doc, const PickView& view, double x, double y, uint32_t filter,
                           double radius);

enum class SelectionKind : uint8_t { None, Vertex, Segment, Face, Entity, Mixed };

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
    // (x1 < x0) also selects what the rectangle touches.
    void Marquee (const Document& doc, const PickView& view, double x0, double y0, double x1, double y1,
                  uint32_t filter, bool additive);
    // Drops references that an edit or an undo removed.
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
