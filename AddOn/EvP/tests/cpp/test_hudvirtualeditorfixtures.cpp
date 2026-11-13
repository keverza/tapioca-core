// #191 U0: the UI Playground's virtual editor fixtures -- one per Inspector context, plus the
// #188 dump -- written from the edit API here so they stay reproducible, and read back through
// the same dump reader the Playground uses. Regenerate with
//   TAPIOCA_PLAYGROUND_FIXTURES_OUT=<dir> EvPGeomTests --gtest_filter=HudVirtualEditorFixtures.Write*
#include "ArchViz/HudVirtualEditor.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPrimitives.hpp"
#include "Edit/EditVirtualEditorDump.hpp"
#include "virtual_editor_debug_replay.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace js = evp::nodegraph::json;
using Value = js::JsonValue;

std::filesystem::path FixtureFolder ()
{
    return std::filesystem::path (__FILE__).parent_path () / "../../tools/UIPlayground/fixtures";
}

// The 11 Inspector contexts of #191, in its table's order, then the live dump.
const std::vector<std::string>& Contexts ()
{
    static const std::vector<std::string> names { "close-prompt",  "command-running", "draw-tool",       "path",
                                                  "region",        "boolean-result",  "reference-line",  "ellipse",
                                                  "fixed-heights", "several-objects", "nothing-selected" };
    return names;
}

struct Scene {
    edit::Document doc;
    edit::Id path = edit::kNoId, regionA = edit::kNoId, regionB = edit::kNoId, reference = edit::kNoId,
             ellipse = edit::kNoId;
};

edit::Id Role (edit::Document& doc, edit::Id entity, lg::SourceRole role)
{
    std::string error;
    EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, entity, role, error)) << error;
    return entity;
}

// One small site with something of every kind the Inspector shows: a design path, two
// overlapping regions, a reference line and an ellipse. Every coordinate is local.
Scene Site ()
{
    Scene s;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (s.doc, { 0x19, 0x1, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x1 }, error))
        << error;
    s.path = edit::AddPath (s.doc, { { 0, -6, 0 }, { 20, -6, 0.5 }, { 28, 0, 1 } }, 2, "fixture path");
    lg::LandscapeSource centreline;
    centreline.pathId = s.path;
    centreline.role = lg::SourceRole::PathCenterline;
    edit::Id id = 0;
    EXPECT_TRUE (lg::AddLandscapeSource (s.doc, centreline, id, error)) << error;
    s.regionA = Role (s.doc, edit::AddPolyline (s.doc, { { 0, 0, 0 }, { 8, 0, 0 }, { 8, 8, 0 }, { 0, 8, 0 } }, true),
                      lg::SourceRole::RegionBoundary);
    s.regionB =
        Role (s.doc, edit::AddPolyline (s.doc, { { 5, 3, 0 }, { 13, 3, 0 }, { 13, 11, 0 }, { 5, 11, 0 } }, true),
              lg::SourceRole::RegionBoundary);
    s.reference =
        Role (s.doc, edit::AddPolyline (s.doc, { { -5, -2, 0 }, { -5, 10, 0 } }, false), lg::SourceRole::ReferenceLine);
    edit::PrimitiveInput ellipse;
    ellipse.kind = edit::Primitive::Ellipse;
    ellipse.first = { -12, 4, 0 };
    ellipse.second = { -9, 6, 0 };
    s.ellipse = edit::AddPrimitive (s.doc, ellipse, "fixture ellipse", error);
    EXPECT_NE (s.ellipse, edit::kNoId) << error;
    return s;
}

Value RefValue (const edit::Ref& ref)
{
    return Value::Object ({ { "entity", Value::Integer (int64_t (ref.entity)) },
                            { "part", Value::Integer (int (ref.part)) },
                            { "item", Value::Integer (int64_t (ref.item)) } });
}

struct Fixture {
    std::string variant, note;
    edit::Document doc;
    std::vector<edit::Ref> selection;
    js::JsonObject session; // what the Playground sets on the editor session after loading
};

std::string Write (const Fixture& f)
{
    const auto parsed = js::Parse (lg::WriteLandscapeDebugSnapshot (f.doc));
    EXPECT_TRUE (parsed.ok) << parsed.error;
    js::JsonArray selection;
    for (const auto& ref : f.selection)
        selection.push_back (RefValue (ref));
    const auto root = Value::Object (
        { { "format", Value::String (lg::kVirtualEditorDumpFormat) },
          { "version", Value::Integer (1) },
          { "units", Value::String ("metres") },
          { "axes", Value::String ("right-handed Z-up") },
          { "createdUtc", Value::String ("fixture") },
          { "hadLandscape", Value::Bool (f.doc.landscape.has_value ()) },
          { "document", parsed.value },
          { "status", Value::Object ({ { "error", Value::String ("") }, { "message", Value::String ("") } }) },
          { "playground", Value::Object ({ { "variant", Value::String (f.variant) },
                                           { "note", Value::String (f.note) },
                                           { "selection", Value::Array (std::move (selection)) },
                                           { "session", Value::Object (f.session) } }) } });
    return js::Write (root, 1) + "\n";
}

std::vector<Fixture> Fixtures ()
{
    std::vector<Fixture> out;
    const auto add = [&] (const std::string& variant, const std::string& note, edit::Document doc,
                          std::vector<edit::Ref> selection = {}, js::JsonObject session = {}) {
        out.push_back ({ variant, note, std::move (doc), std::move (selection), std::move (session) });
    };
    const auto whole = [] (edit::Id id) { return edit::Ref { id, edit::Part::Entity, 0 }; };
    const auto site = Site ();
    add ("close-prompt", "Close asked with unsaved work: Save and close / Discard and close / Cancel.", site.doc, {},
         { { "closePrompt", Value::Bool (true) } });
    add (
        "command-running", "A Union picking whole shapes; region A is picked, B is next.", site.doc, {},
        { { "command", Value::String ("union") },
          { "picks", Value::Array ({ Value::Array ({ Value::Double (1), Value::Double (1), Value::Double (0) }) }) } });
    add ("draw-tool", "The polyline tool active, nothing drawn yet.", site.doc, {},
         { { "tool", Value::String ("polyline") } });
    add ("path", "The design path selected: its kerb radius, fall, sections and profile.", site.doc,
         { whole (site.path) });
    add ("region", "Region A's boundary selected: role, effect, elevation, slope, material.", site.doc,
         { whole (site.regionA) });
    {
        // The Boolean applied through the editor's own command, so its operation and object are
        // exactly what the panel makes; its two input lines selected (#189 decides how a result
        // itself is selected).
        eh::Session session (site.doc);
        EXPECT_TRUE (eh::BeginLandscapeCommand (session, lg::OperationKind::Intersection));
        EXPECT_TRUE (eh::PickLandscapeShape (session, { 1, 1, 0 }, true));
        EXPECT_TRUE (eh::PickLandscapeShape (session, { 12, 10, 0 }, true));
        EXPECT_TRUE (eh::AdvanceLandscapeCommand (session)) << session.message;
        add ("boolean-result", "Regions A and B intersected; the inputs are selected.", session.edits.Doc (),
             { whole (site.regionA), whole (site.regionB) });
    }
    add ("reference-line", "A reference line selected: its role and display style.", site.doc,
         { whole (site.reference) });
    add ("ellipse", "An ellipse selected: its own fields.", site.doc, { whole (site.ellipse) });
    {
        auto doc = site.doc;
        const auto* entity = edit::Find (doc, site.regionA);
        const auto vertex = entity->vertices.front ().id;
        edit::Id source = 0;
        for (const auto& s : doc.landscape->sources)
            if (s.entityId == site.regionA)
                source = s.id;
        std::string error;
        EXPECT_TRUE (lg::SetLandscapeTerrainControl (doc, source, vertex, true, error)) << error;
        add ("fixed-heights", "One corner of region A fixed in the terrain, and selected.", doc,
             { { site.regionA, edit::Part::Vertex, vertex } });
    }
    add ("several-objects", "A path, a region line and an ellipse selected together.", site.doc,
         { whole (site.path), whole (site.regionA), whole (site.ellipse) });
    add ("nothing-selected", "Nothing selected: terrain, rebuild options, Add from Archicad.", site.doc);
    return out;
}

Value Load (const std::filesystem::path& file)
{
    std::ifstream in (file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf ();
    const auto parsed = js::Parse (text.str ());
    EXPECT_TRUE (parsed.ok) << file << ": " << parsed.error;
    return parsed.value;
}
} // namespace

TEST (HudVirtualEditorFixtures, WritePlaygroundFixturesFromEnvironment)
{
    const char* out = std::getenv ("TAPIOCA_PLAYGROUND_FIXTURES_OUT");
    if (!out || !*out)
        GTEST_SKIP () << "Set TAPIOCA_PLAYGROUND_FIXTURES_OUT to regenerate the Playground editor fixtures";
    for (const auto& f : Fixtures ())
        std::ofstream (std::filesystem::path (out) / ("editor-ctx-" + f.variant + ".json"), std::ios::binary)
            << Write (f);
}

// Every context has its fixture, each reads through the shared dump reader, and what it selects
// exists in its document; the committed files are what the generator writes today.
TEST (HudVirtualEditorFixtures, EveryInspectorContextHasAReadableFixture)
{
    const auto generated = Fixtures ();
    ASSERT_EQ (generated.size (), Contexts ().size ());
    std::set<std::string> variants;
    for (size_t i = 0; i < generated.size (); ++i) {
        const auto& f = generated[i];
        EXPECT_EQ (f.variant, Contexts ()[i]);
        const auto file = FixtureFolder () / ("editor-ctx-" + f.variant + ".json");
        ASSERT_TRUE (std::filesystem::exists (file)) << file;
        const auto value = Load (file);
        EXPECT_EQ (js::Write (value, 1) + "\n", Write (f)) << f.variant << ": regenerate the fixture";
        edit::Document doc;
        lg::ReadVirtualEditorDump (lg::landscape_json::Reader (value, f.variant), doc);
        EXPECT_TRUE (doc.landscape);
        for (const auto& ref : f.selection)
            EXPECT_TRUE (edit::Exists (doc, ref)) << f.variant;
        variants.insert (f.variant);
    }
    EXPECT_EQ (variants.size (), generated.size ());
}

// #188: the reduced live dump -- two regions and their Intersection, no path and no property line
// -- still reads, and still carries the refusal it was taken for.
TEST (HudVirtualEditorFixtures, BooleanRefusedDumpReadsWithItsRefusal188)
{
    const auto value = Load (FixtureFolder () / "editor-boolean-refused.json");
    edit::Document doc;
    lg::VirtualEditorDumpHeader header;
    lg::ReadVirtualEditorDump (lg::landscape_json::Reader (value, "boolean-refused"), doc, &header);
    ASSERT_TRUE (doc.landscape);
    EXPECT_EQ (doc.landscape->propertyLineSourceId, edit::kNoId);
    EXPECT_TRUE (doc.paths.empty ());
    EXPECT_EQ (doc.landscape->objects.size (), 3u);
    EXPECT_EQ (header.error.rfind ("PropertyLineOutside", 0), 0u) << header.error;
}

#if defined(TAPIOCA_GEOMETRY_RUST)
// #188, the cause. The Intersection in this dump kept its operands' Boundary curve Z, which a derived
// region cannot resolve, and the build then stopped on the attributed region report: that stage splits
// the mesh once per ring, and the Intersection's corners sit on the crossings its operands' splits
// already placed, so it refused with PropertyLineOutside although there is no property line. The report
// no longer refuses the build; the Boundary result refuses with what to do; once it is Constant (what a
// Boolean now gives it) the scene builds.
TEST (HudVirtualEditorFixtures, BooleanRefusedFixtureBuildsOnceTheResultIsConstant188)
{
    auto scene = debugreplay::Read (Load (FixtureFolder () / "editor-boolean-refused.json"));
    const auto refused = debugreplay::Run (scene);
    EXPECT_FALSE (refused.built);
    EXPECT_NE (refused.error.find ("derived region"), std::string::npos) << refused.error;
    EXPECT_EQ (refused.error.find ("PropertyLineOutside"), std::string::npos) << refused.error;
    bool found = false;
    for (auto& object : scene.doc.landscape->objects)
        if (object.name == "Intersection") {
            object.surface->elevation = lg::RegionElevationMode::Constant;
            found = true;
        }
    ASSERT_TRUE (found);
    const auto built = debugreplay::Run (scene);
    EXPECT_TRUE (built.built) << built.error;
    EXPECT_GT (built.result.build.triangles.size (), 0u);
}
#endif
