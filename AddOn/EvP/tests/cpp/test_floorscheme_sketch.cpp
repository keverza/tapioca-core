// The skeleton as a drawing the virtual editor's kernel edits (FloorSchemeSketch.hpp): a shape for
// each wing, corridor and stair; a part dragged by the kernel's own edits; and the drawing read
// back into the design through the floor plan's edits, with their refusals. And the access graph.
#include "ArchViz/FloorSchemeSketch.hpp"
#include "Edit/EditPolyline.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;
namespace sk = geomsrv::archviz::floorscheme::sketch;
namespace ge = geomsrv::edit;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
struct Bar {
    std::vector<fs::Ring> outline { Rect (0, 0, 60, 16) };
    fs::Scheme scheme = fs::Generate (outline, fp::Default ());
    sk::Sketch sketch = sk::Of (scheme);
    const ge::Entity& Shape (sk::What what, int index = 0) const
    {
        return *ge::Find (sketch.doc, sketch.Find (what, index)->entity);
    }
    // The side of `e` whose middle is nearest `at`; the corner nearest `at`.
    static ge::Ref Side (const ge::Entity& e, fs::Vec at)
    {
        ge::Ref best { e.id, ge::Part::Segment };
        double near = 1e300;
        for (size_t k = 0; k < ge::SegmentCount (e); ++k) {
            size_t a = 0, b = 0;
            ge::SegmentEnds (e, k, a, b);
            const double d = std::hypot ((e.vertices[a].p.x + e.vertices[b].p.x) / 2 - at.x,
                                         (e.vertices[a].p.y + e.vertices[b].p.y) / 2 - at.y);
            if (d < near)
                near = d, best.item = e.segments[k].id;
        }
        return best;
    }
    static ge::Ref Corner (const ge::Entity& e, fs::Vec at)
    {
        ge::Ref best { e.id, ge::Part::Vertex };
        double near = 1e300;
        for (const auto& v : e.vertices)
            if (const double d = std::hypot (v.p.x - at.x, v.p.y - at.y); d < near)
                near = d, best.item = v.id;
        return best;
    }
    // The design a drag of `ref` by `by` comes to.
    fe::Design Dragged (const ge::Ref& ref, fs::Vec by, std::string* why = nullptr) const
    {
        ge::Document doc = sketch.doc;
        std::string error;
        if (!sk::Drag (doc, sketch, ref, by, error)) {
            if (why)
                *why = error;
            return {};
        }
        return sk::Read (scheme, {}, sketch, doc, why);
    }
};
} // namespace

TEST (FloorSchemeSketch, EveryWingCorridorAndStairIsAShapeAndUntouchedItAsksForNothing)
{
    const Bar b;
    ASSERT_EQ (b.scheme.wings.size (), 1u);
    EXPECT_EQ (b.sketch.items.size (), b.scheme.wings.size () + b.scheme.corridors.size () + b.scheme.cores.size ());
    EXPECT_TRUE (b.Shape (sk::What::Wing).closed);
    EXPECT_EQ (b.Shape (sk::What::Wing).vertices.size (), 4u);
    EXPECT_FALSE (b.Shape (sk::What::Corridor).closed);
    std::string error;
    EXPECT_TRUE (ge::Validate (b.sketch.doc, error)) << error;
    EXPECT_EQ (sk::Read (b.scheme, {}, b.sketch, b.sketch.doc), fe::Design {});
}

TEST (FloorSchemeSketch, AWingsSideAndCornerAreMovedByTheKernelAndReadAsTheWing)
{
    const Bar b;
    const auto& wing = b.Shape (sk::What::Wing);
    // Its end at x = 60 pulled in 4 m; the pull's part along the side changes nothing.
    std::string why;
    auto d = b.Dragged (Bar::Side (wing, { 60, 8 }), { -4, 3 }, &why);
    ASSERT_EQ (d.pins.wings.size (), 1u) << why;
    EXPECT_NEAR ((std::max) (d.pins.wings[0].a.x, d.pins.wings[0].b.x), 56, 1e-9);
    EXPECT_NEAR ((std::min) (d.pins.wings[0].a.x, d.pins.wings[0].b.x), 0, 1e-9);
    EXPECT_NEAR (d.pins.wings[0].depth, 16, 1e-9);
    // The corner at (60, 16) pulled in both ways: its two sides follow, the wing stays a rectangle.
    d = b.Dragged (Bar::Corner (wing, { 60, 16 }), { -4, -2 }, &why);
    ASSERT_EQ (d.pins.wings.size (), 1u) << why;
    EXPECT_NEAR ((std::max) (d.pins.wings[0].a.x, d.pins.wings[0].b.x), 56, 1e-9);
    EXPECT_NEAR (d.pins.wings[0].depth, 14, 1e-9);
    EXPECT_NEAR (d.pins.wings[0].a.y, 7, 1e-9);
    const auto planned = fs::Generate (b.outline, fp::Default (), d.pins);
    ASSERT_EQ (planned.wings.size (), 1u);
    EXPECT_NEAR (planned.wings[0].length, 56, 1e-6);
    EXPECT_NEAR (planned.wings[0].depth, 14, 1e-6);
}

TEST (FloorSchemeSketch, AWingTheFloorCannotTakeIsRefusedWithTheFloorPlansOwnWords)
{
    const Bar b;
    const auto& wing = b.Shape (sk::What::Wing);
    std::string why;
    // The whole wing 2 m along: 2 m of it off the floor.
    EXPECT_EQ (b.Dragged (ge::Ref { wing.id }, { 2, 0 }, &why), fe::Design {});
    EXPECT_NE (why.find ("outside the floor"), std::string::npos) << why;
    // A side pushed through the one opposite: the kernel's own refusal.
    why.clear ();
    EXPECT_EQ (b.Dragged (Bar::Side (wing, { 60, 8 }), { -70, 0 }, &why), fe::Design {});
    EXPECT_FALSE (why.empty ());
}

TEST (FloorSchemeSketch, ACorridorsEndRunsAlongItAndItsLegMovesAcrossTheWing)
{
    const Bar b;
    ASSERT_FALSE (b.scheme.corridors.empty ());
    const auto& line = b.Shape (sk::What::Corridor);
    const fs::Vec first { line.vertices.front ().p.x, line.vertices.front ().p.y };
    const bool low = first.x < line.vertices.back ().p.x; // the end at the lower x
    const fs::Vec end = low ? first : fs::Vec { line.vertices.back ().p.x, line.vertices.back ().p.y };
    std::string why;
    // Its end pulled 3 m in, the pointer drifting 1 m across: the end stays on its line.
    auto d = b.Dragged (Bar::Corner (line, end), { 3, 1 }, &why);
    ASSERT_EQ (d.pins.ends.size (), 1u) << why;
    EXPECT_NEAR (d.pins.ends[0].at.x, end.x + 3, 1e-9);
    EXPECT_NEAR (d.pins.ends[0].at.y, end.y, 1e-9);
    EXPECT_TRUE (d.pins.corridors.empty ());
    // Its leg moved 1.2 m across the wing.
    d = b.Dragged (ge::Ref { line.id, ge::Part::Segment, line.segments[0].id }, { 5, 1.2 }, &why);
    ASSERT_EQ (d.pins.corridors.size (), 1u) << why;
    EXPECT_NEAR (d.pins.corridors[0].at.y, 9.2, 1e-9);
    EXPECT_TRUE (d.pins.ends.empty ());
    const auto planned = fs::Generate (b.outline, fp::Default (), d.pins);
    ASSERT_FALSE (planned.corridors.empty ());
    EXPECT_NEAR (planned.corridors[0].axis.front ().y, 9.2, 1e-6);
    // Too far across: the floor plan's own rule and words.
    why.clear ();
    EXPECT_EQ (b.Dragged (ge::Ref { line.id, ge::Part::Segment, line.segments[0].id }, { 0, 4.5 }, &why),
               fe::Design {});
    EXPECT_NE (why.find ("at least 5.5 m"), std::string::npos) << why;
}

TEST (FloorSchemeSketch, AStairIsMovedAlongItsWingAndASideChangesItsSize)
{
    const Bar b;
    ASSERT_FALSE (b.scheme.cores.empty ());
    const auto& core = b.scheme.cores[0];
    const auto& shape = b.Shape (sk::What::Stair);
    std::string why;
    auto d = b.Dragged (ge::Ref { shape.id }, { 2.4, 0 }, &why);
    ASSERT_FALSE (d.pins.cores.empty ()) << why;
    EXPECT_TRUE (std::any_of (d.pins.cores.begin (), d.pins.cores.end (), [&] (const fs::Pins::Core& p) {
        return std::abs (p.centre.x - (core.centre.x + 2.4)) < 1e-9 && std::abs (p.centre.y - core.centre.y) < 1e-9;
    }));
    // Off the floor: said.
    why.clear ();
    EXPECT_EQ (b.Dragged (ge::Ref { shape.id }, { 0, 40 }, &why), fe::Design {});
    EXPECT_NE (why.find ("on the floor"), std::string::npos) << why;
    // The side facing +x pushed out 0.8 m: 0.8 m wider along the wing, and its middle 0.4 m on.
    const fs::Vec side { core.centre.x + core.width / 2, core.centre.y };
    d = b.Dragged (Bar::Side (shape, side), { 0.8, 0 }, &why);
    const auto pin =
        std::find_if (d.pins.cores.begin (), d.pins.cores.end (), [] (const fs::Pins::Core& p) { return p.width > 0; });
    ASSERT_NE (pin, d.pins.cores.end ()) << why;
    EXPECT_NEAR (pin->width, core.width + 0.8, 1e-9);
    EXPECT_NEAR (pin->depth, core.depth, 1e-9);
    EXPECT_NEAR (pin->centre.x, core.centre.x + 0.4, 1e-9);
}

TEST (FloorSchemeSketch, TheAccessGraphJoinsEveryFlatToCirculation)
{
    for (const auto& outline :
         { std::vector<fs::Ring> { Rect (0, 0, 60, 16) }, std::vector<fs::Ring> { Rect (0, 0, 48, 12) },
           std::vector<fs::Ring> { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) } }) {
        const auto s = fs::Generate (outline, fp::Default ());
        const auto g = sk::Access (s);
        size_t legs = 0;
        for (const auto& c : s.corridors)
            legs += c.axis.size () - 1;
        EXPECT_EQ (g.nodes.size (), s.flats.size () + s.cores.size () + legs);
        // An edge leaves every flat's node and ends on circulation: a corridor's line or a stair.
        for (size_t i = 0; i < s.flats.size (); ++i) {
            const auto node = std::find_if (g.nodes.begin (), g.nodes.end (), [&] (const sk::Graph::Node& n) {
                return n.kind == sk::Graph::Kind::Flat && n.index == int (i);
            });
            ASSERT_NE (node, g.nodes.end ());
            const auto edge = std::find_if (g.edges.begin (), g.edges.end (),
                                            [&] (const sk::Graph::Edge& e) { return e.line.front () == node->at; });
            ASSERT_NE (edge, g.edges.end ()) << "flat " << i;
            const fs::Vec to = edge->line.back ();
            bool on = false;
            for (const auto& core : s.cores)
                on = on || std::hypot (core.centre.x - to.x, core.centre.y - to.y) < 1e-6;
            for (const auto& lobby : s.lobbies) {
                fs::Vec m;
                for (const auto& q : lobby)
                    m.x += q.x / lobby.size (), m.y += q.y / lobby.size ();
                on = on || std::hypot (m.x - to.x, m.y - to.y) < 1e-6;
            }
            for (const auto& c : s.corridors)
                for (size_t k = 0; k + 1 < c.axis.size (); ++k) {
                    const fs::Vec a = c.axis[k], b = c.axis[k + 1];
                    const double l = std::hypot (b.x - a.x, b.y - a.y);
                    const double t = ((to.x - a.x) * (b.x - a.x) + (to.y - a.y) * (b.y - a.y)) / (l * l);
                    const double off = std::abs ((to.x - a.x) * (b.y - a.y) - (to.y - a.y) * (b.x - a.x)) / l;
                    on = on || (t > -1e-9 && t < 1 + 1e-9 && off < 1e-6);
                }
            EXPECT_TRUE (on) << "flat " << i << " is joined to (" << to.x << ", " << to.y << ")";
        }
    }
}
