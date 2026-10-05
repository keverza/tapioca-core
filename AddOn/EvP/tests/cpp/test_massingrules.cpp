#include "ArchViz/MassingRules.hpp"
#include "ArchViz/HudMassingRules.hpp"
#include "ArchViz/MassingCalculation.hpp"
#include "NodeGraph/Json.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "hud_fixture.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace rules = geomsrv::archviz::massingrules;
namespace widgets = geomsrv::archviz::hudmassingrules;
namespace meta = geomsrv::metadata;

namespace {
std::vector<rules::Edge> Square ()
{
    return { { 0, 0, 10, 0 }, { 10, 0, 10, 10 }, { 10, 10, 0, 10 }, { 0, 10, 0, 0 } };
}
meta::EntityMetadata Saved ()
{
    meta::EntityMetadata entity;
    meta::Property property;
    std::string error;
    EXPECT_TRUE (rules::Encode (Square (),
                                { { rules::Mode::Custom, 1 },
                                  { rules::Mode::None, 0 },
                                  { rules::Mode::Default, 3 },
                                  { rules::Mode::Custom, 5 } },
                                property, error));
    meta::SetProperty (entity, std::move (property));
    return entity;
}
rules::Page Page (const meta::EntityMetadata& entity = {})
{
    rules::Page page;
    page.guid = "parcel";
    page.edges = Square ();
    EXPECT_TRUE (rules::Restore (page, entity));
    return page;
}
} // namespace

TEST (MassingRules, FingerprintsMatchPythonAndCanonicalSignedArcs)
{
    EXPECT_EQ (rules::Fingerprint ({ 0, 0, 10, 0, 0 }), "4c8614dd4ebea5bd");
    EXPECT_EQ (rules::Fingerprint ({ 10, 0, 10, 10, 0 }), "c42c30ca48be93fb");
    EXPECT_EQ (rules::Fingerprint ({ 1.25, -2.5, 6.5, 3.125, 1.5707963267948966 }), "9dc154f016070e64");
    EXPECT_EQ (rules::Fingerprint ({ 6.5, 3.125, 1.25, -2.5, -1.5707963267948966 }), "9dc154f016070e64");
    EXPECT_EQ (rules::Fingerprint ({ 0.00005, -0.00015, 1000000, 0.00025, 0.00025 }), "04d546ae107913a7");
    EXPECT_NE (rules::Fingerprint ({ 0, 0, 10, 0, 0.1 }), rules::Fingerprint ({ 0, 0, 10, 0, -0.1 }));
}

TEST (MassingCalculation, OffsetDimensionsMeasureSurvivingInsetEdgesInEitherWinding)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    calc::Preview preview;
    preview.inputs.before.edges = Square ();
    preview.inputs.assignments.assign (4, { rules::Mode::Custom, 2 });
    preview.result.offsetXY = { 2, 2, 8, 2, 8, 8, 2, 8 };
    preview.result.hasMeanZ = true;
    preview.result.meanZ = 7;
    auto layer = calc::OffsetDimensions (preview);
    EXPECT_TRUE (geomsrv::archviz::overlaylayers::Validate (layer).empty ());
    ASSERT_EQ (layer.dimensions.size (), 4u);
    for (const auto& dimension : layer.dimensions) {
        EXPECT_NEAR (std::hypot (dimension.to[0] - dimension.from[0], dimension.to[1] - dimension.from[1]), 2, 1e-9);
        EXPECT_EQ (dimension.from[2], 7);
        EXPECT_TRUE (dimension.showUnit);
        EXPECT_EQ (dimension.decimals, 2u);
    }
    std::reverse (preview.inputs.before.edges.begin (), preview.inputs.before.edges.end ());
    for (auto& edge : preview.inputs.before.edges) {
        std::swap (edge.ax, edge.bx);
        std::swap (edge.ay, edge.by);
    }
    EXPECT_EQ (calc::OffsetDimensions (preview).dimensions.size (), 4u);
    preview.result.offsetXY = { 6, 2, 8, 2, 8, 4, 6, 4 };
    EXPECT_TRUE (calc::OffsetDimensions (preview).dimensions.empty ()) << "do not label a removed midpoint";
    preview.result.offsetXY.clear ();
    EXPECT_TRUE (calc::OffsetDimensions (preview).dimensions.empty ());
}

TEST (MassingRules, FreshGeometryStartsDefaultWithoutWriting)
{
    const auto page = Page ();
    EXPECT_FALSE (page.hasStored);
    ASSERT_EQ (page.assignments.size (), 4u);
    for (const auto& assignment : page.assignments) {
        EXPECT_EQ (assignment.mode, rules::Mode::Default);
        EXPECT_EQ (assignment.distance, 3);
        EXPECT_FALSE (assignment.review);
    }
}

TEST (MassingRules, SaveContainsOnlyTheFourMinimalSegmentFields)
{
    const auto entity = Saved ();
    ASSERT_EQ (entity.properties.size (), 1u);
    const auto& property = entity.properties[0];
    EXPECT_EQ (property.key, "setback.segments");
    EXPECT_EQ (property.state, meta::State::Authored);
    ASSERT_EQ (property.value.list.size (), 4u);
    for (size_t i = 0; i < 4; ++i) {
        const auto& fields = property.value.list[i].fields;
        ASSERT_EQ (fields.size (), 4u);
        EXPECT_EQ (fields.at ("segmentIndex").i, int64_t (i));
        EXPECT_EQ (fields.at ("distance").type, meta::ValueType::Length);
        EXPECT_EQ (fields.at ("geometryFingerprint").s, rules::Fingerprint (Square ()[i]));
    }
    std::string error;
    meta::EntityMetadata restored;
    ASSERT_TRUE (meta::FromJson (meta::ToJson (entity), restored, error));
    const auto page = Page (restored);
    EXPECT_EQ (page.assignments[0].distance, 1);
    EXPECT_EQ (page.assignments[1].mode, rules::Mode::None);
    EXPECT_EQ (page.assignments[3].distance, 5);
}

TEST (MassingRules, UniqueAssignmentsSurviveReorderingAndReversal)
{
    auto page = Page ();
    std::reverse (page.edges.begin (), page.edges.end ());
    for (auto& edge : page.edges) {
        std::swap (edge.ax, edge.bx);
        std::swap (edge.ay, edge.by);
        edge.arcAngle = -edge.arcAngle;
    }
    std::rotate (page.edges.begin (), page.edges.begin () + 1, page.edges.end ());
    ASSERT_TRUE (rules::Restore (page, Saved ()));
    EXPECT_EQ (page.assignments[0].distance, 3);
    EXPECT_EQ (page.assignments[1].mode, rules::Mode::None);
    EXPECT_EQ (page.assignments[2].distance, 1);
    EXPECT_EQ (page.assignments[3].distance, 5);
    for (const auto& assignment : page.assignments)
        EXPECT_FALSE (assignment.review);
}

TEST (MassingRules, ActualGeometryChangesAndDuplicateHashesNeedReview)
{
    auto page = Page ();
    page.edges[0].ax = page.edges[3].bx = 0.01;
    ASSERT_TRUE (rules::Restore (page, Saved ()));
    for (const auto& assignment : page.assignments)
        EXPECT_TRUE (assignment.review); // unmatched saved edges demand review of the whole set
    auto entity = Saved ();
    entity.properties[0].value.list.push_back (entity.properties[0].value.list[0]);
    page.edges = Square ();
    ASSERT_TRUE (rules::Restore (page, entity));
    for (const auto& assignment : page.assignments)
        EXPECT_TRUE (assignment.review);
}

TEST (MassingRules, MalformedStoredDataIsNeverSilentlyReplaced)
{
    const auto saved = Saved ();
    for (const char* key : { "segmentIndex", "mode", "distance", "geometryFingerprint" }) {
        auto entity = saved;
        entity.properties[0].value.list[0].fields.erase (key);
        auto page = Page ();
        EXPECT_FALSE (rules::Restore (page, entity)) << key;
        EXPECT_FALSE (page.known);
        EXPECT_TRUE (page.assignments.empty ());
        EXPECT_FALSE (page.note.empty ());
    }
    for (double distance :
         { -1.0, 1001.0, std::numeric_limits<double>::infinity (), std::numeric_limits<double>::quiet_NaN () }) {
        auto entity = saved;
        entity.properties[0].value.list[0].fields["distance"].d = distance;
        auto page = Page ();
        EXPECT_FALSE (rules::Restore (page, entity));
    }
}

TEST (MassingRules, EncodeRefusesUnreviewedAndInvalidValuesAtomically)
{
    const auto page = Page ();
    for (auto bad :
         { rules::Assignment { rules::Mode::Default, 3, true }, rules::Assignment { rules::Mode::Custom, -1 },
           rules::Assignment { rules::Mode::None, 3 }, rules::Assignment { rules::Mode (9), 3 } }) {
        auto assignments = page.assignments;
        assignments[0] = bad;
        meta::Property property;
        property.key = "unchanged";
        std::string error;
        EXPECT_FALSE (rules::Encode (page.edges, assignments, property, error));
        EXPECT_EQ (property.key, "unchanged");
        EXPECT_FALSE (error.empty ());
    }
}

TEST (MassingRules, GeometryGuardsRejectInvalidOrRetargetedSnapshots)
{
    auto edges = Square ();
    EXPECT_TRUE (rules::ValidEdges (edges));
    EXPECT_TRUE (rules::SameGeometry (edges, Square ()));
    edges[0].arcAngle = 0.00001;
    EXPECT_FALSE (rules::SameGeometry (edges, Square ())); // even sub-quantization changes block queued Save
    edges[0].ax = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (rules::ValidEdges (edges));
    EXPECT_TRUE (rules::Fingerprint (edges[0]).empty ());
    edges = Square ();
    edges.pop_back ();
    EXPECT_FALSE (rules::ValidEdges (edges));
}

TEST (MassingRules, DraftSurvivesUnrelatedSelectionAndDiscardsStaleGeometry)
{
    widgets::Draft draft;
    auto page = Page (Saved ());
    widgets::Sync (page, draft);
    draft.assignments[0].distance = 2;
    draft.endpoints[0] = false;
    draft.dirty = true;
    widgets::Sync (page, draft);
    EXPECT_TRUE (draft.dirty);
    EXPECT_EQ (draft.assignments[0].distance, 2);
    page.edges[0].ax = page.edges[3].bx = 0.01;
    ASSERT_TRUE (rules::Restore (page, Saved ()));
    widgets::Sync (page, draft);
    EXPECT_FALSE (draft.dirty);
    EXPECT_FALSE (draft.note.empty ());
    EXPECT_TRUE (draft.assignments[0].review);
}

TEST (MassingRules, QueuedSaveRefusesChangedGeometryRoleOrSavedAssignments)
{
    auto entity = Saved ();
    meta::Property role;
    role.key = "tapioca.role";
    role.value = meta::Value::Text ("PropertyLine");
    meta::SetProperty (entity, role);
    const auto page = Page (entity);
    const rules::Edit edit { page, page.assignments };
    std::string error;
    EXPECT_TRUE (rules::CheckSource (edit, page.edges, entity, error));
    auto moved = page.edges;
    moved[0].ax += 1e-8;
    EXPECT_FALSE (rules::CheckSource (edit, moved, entity, error));
    EXPECT_FALSE (error.empty ());
    role.value = meta::Value::Text ("ExistingTerrain");
    meta::SetProperty (entity, role);
    EXPECT_FALSE (rules::CheckSource (edit, page.edges, entity, error));
    role.value = meta::Value::Text ("PropertyLine");
    meta::SetProperty (entity, role);
    entity.properties[0].value.list[0].fields["distance"].d = 9;
    EXPECT_FALSE (rules::CheckSource (edit, page.edges, entity, error));
    meta::RemoveProperty (entity, "setback.segments");
    EXPECT_FALSE (rules::CheckSource (edit, page.edges, entity, error));
}

TEST (MassingRules, SaveMergesFreshUnrelatedMetadataWithoutOverwritingIt)
{
    auto entity = Saved ();
    meta::Property role;
    role.key = "tapioca.role";
    role.value = meta::Value::Text ("PropertyLine");
    meta::SetProperty (entity, role);
    const auto page = Page (entity);
    rules::Edit edit { page, page.assignments };
    edit.assignments[0].distance = 2;
    meta::Property unrelated;
    unrelated.key = "custom.owner";
    unrelated.value = meta::Value::Text ("new owner assigned after layout");
    meta::SetProperty (entity, unrelated);
    std::string error;
    ASSERT_TRUE (rules::CheckSource (edit, page.edges, entity, error));
    meta::Property property;
    ASSERT_TRUE (rules::Encode (page.edges, edit.assignments, property, error));
    meta::SetProperty (entity, std::move (property));
    ASSERT_NE (meta::FindProperty (entity, "custom.owner"), nullptr);
    EXPECT_EQ (meta::FindProperty (entity, "custom.owner")->value.s, unrelated.value.s);
    EXPECT_EQ (meta::FindProperty (entity, "tapioca.role")->value.s, "PropertyLine");
    EXPECT_EQ (Page (entity).assignments[0].distance, 2);
}

TEST (MassingRules, SuccessfulSaveAcknowledgesDraftAndPreservesRunLocalEndpoints)
{
    auto page = Page (Saved ());
    widgets::Draft draft;
    widgets::Sync (page, draft);
    draft.endpoints[0] = false;
    draft.assignments[0].distance = 2;
    draft.dirty = true;
    meta::EntityMetadata entity;
    meta::Property property;
    std::string error;
    ASSERT_TRUE (rules::Encode (page.edges, draft.assignments, property, error));
    meta::SetProperty (entity, std::move (property));
    ASSERT_TRUE (rules::Restore (page, entity));
    widgets::Sync (page, draft);
    EXPECT_FALSE (draft.dirty);
    EXPECT_FALSE (draft.endpoints[0]);
    EXPECT_TRUE (draft.note.empty ());
    EXPECT_EQ (draft.assignments[0].distance, 2);
}

TEST (MassingRules, NativePageDrawsWithoutCommandAndStateClearDropsRequests)
{
    using namespace hudtest;
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.massing.known = true;
    pages.massing.rules = Page (Saved ());
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, geomsrv::archviz::hudmassing::kTabKey);
    hud.Lay ({}, At (600, 600));
    const auto layout = hud.Lay ({}, At (600, 600));
    EXPECT_TRUE (std::any_of (layout.host.vertices.begin (), layout.host.vertices.end (),
                              [] (const hud::Vertex& vertex) { return vertex.rgba == 0xAA4465FFu; }));
    EXPECT_TRUE (hud::TakeMassingRuleEdits (*hud.state).empty ());
    auto& draft = hud.state->massingRules;
    draft.assignments[0].distance = 2;
    draft.dirty = true;
    hud.state->massingRuleEdits.push_back ({ pages.massing.rules, draft.assignments });
    auto edits = hud::TakeMassingRuleEdits (*hud.state);
    ASSERT_EQ (edits.size (), 1u);
    EXPECT_EQ (edits[0].before.guid, "parcel");
    EXPECT_EQ (edits[0].assignments[0].distance, 2);
    EXPECT_TRUE (hud::TakeMassingRuleEdits (*hud.state).empty ());
    hud.state->massingRuleEdits.push_back ({ pages.massing.rules, draft.assignments });
    hud::ClearState (*hud.state);
    EXPECT_TRUE (hud::TakeMassingRuleEdits (*hud.state).empty ());
    EXPECT_TRUE (hud.state->massingRules.source.guid.empty ());
}

namespace {
struct RulesWidget {
    ImGuiContext* context = ImGui::CreateContext ();
    rules::Page page = Page (Saved ());
    widgets::Draft draft;
    std::shared_ptr<const geomsrv::archviz::massingcalculation::Preview> preview;
    ImRect canvas;
    ImGuiID lastItem = 0;
    RulesWidget ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 1200, 1200 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        widgets::Sync (page, draft);
        Frame ({ 850, 850 });
        Frame ({ 850, 850 });
    }
    ~RulesWidget ()
    {
        ImGui::DestroyContext (context);
    }
    std::vector<rules::Edit> Frame (ImVec2 pointer, bool left = false, bool right = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, left);
        io.AddMouseButtonEvent (1, right);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 750 });
        ImGui::Begin ("rules-test");
        auto edits = widgets::Draw (page, draft, false, {}, preview);
        lastItem = context->LastItemData.ID;
        if (context->OpenPopupStack.empty ())
            canvas = context->LastItemData.Rect;
        ImGui::End ();
        ImGui::Render ();
        return edits;
    }
    void Open (ImVec2 pointer)
    {
        EXPECT_TRUE (Frame (pointer, false, true).empty ());
        EXPECT_TRUE (Frame (pointer).empty ());
        EXPECT_TRUE (Frame (pointer).empty ());
        ASSERT_FALSE (context->OpenPopupStack.empty ());
    }
    ImVec2 Find (const char* label)
    {
        if (context->OpenPopupStack.empty ())
            return {};
        auto* popup = context->OpenPopupStack[0].Window;
        if (popup == nullptr)
            return {};
        const auto id = popup->GetID (label);
        const auto min = popup->Pos, max = ImVec2 (min.x + popup->Size.x, min.y + popup->Size.y);
        const float x = min.x + 50;
        for (float y = min.y + 5; y < max.y - 5; y += 2) {
            Frame ({ x, y });
            if (context->HoveredId == id)
                return { x, y + 2 };
        }
        return {};
    }
};
} // namespace

TEST (MassingRules, OnlyContourRemainsInTheExpandedRulesSection)
{
    RulesWidget widget;
    const auto* window = ImGui::FindWindowByName ("rules-test");
    const ImGuiID section = ImHashStr ("massing.rules", 0, window->IDStack[0]);
    EXPECT_EQ (widget.lastItem, ImHashStr ("##massing.site", 0, section));
    EXPECT_NEAR (widget.canvas.GetHeight (), 200, 0.01);
}

TEST (MassingRules, OffsetContourDrawsAlongsideParcelOnlyForMatchingDraftInputs)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    RulesWidget widget;
    calc::Preview preview;
    ASSERT_TRUE (widget.draft.lastRequested);
    preview.inputs = *widget.draft.lastRequested;
    preview.result.offsetXY = { 2, 2, 8, 2, 8, 8, 2, 8 };
    widget.preview = std::make_shared<const calc::Preview> (preview);
    widget.Frame ({ 850, 850 });
    const auto hasColour = [&] (ImU32 colour) {
        const auto& vertices = ImGui::FindWindowByName ("rules-test")->DrawList->VtxBuffer;
        return std::any_of (vertices.begin (), vertices.end (),
                            [colour] (const ImDrawVert& v) { return v.col == colour; });
    };
    EXPECT_TRUE (hasColour (IM_COL32 (170, 68, 101, 255)));
    EXPECT_TRUE (hasColour (IM_COL32 (166, 98, 38, 255)));
    widget.draft.assignments[0].distance = 2.5;
    widget.Frame ({ 850, 850 });
    EXPECT_TRUE (hasColour (IM_COL32 (170, 68, 101, 255)));
    EXPECT_FALSE (hasColour (IM_COL32 (166, 98, 38, 255)));
}

TEST (MassingRules, PreviewQueueCoalescesAndRetainsLatestEditWhileBusy)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    calc::PreviewQueue queue;
    calc::Request request;
    request.before = Page ();
    request.assignments = request.before.assignments;
    request.endpoints.assign (4, true);
    request.regulated.assign (4, true);
    EXPECT_TRUE (queue.Follow (request, 0));
    const auto first = queue.Revision ();
    EXPECT_FALSE (queue.Follow (request, 20));
    EXPECT_EQ (queue.Revision (), first);
    EXPECT_FALSE (queue.TakeReady (149, false));
    EXPECT_TRUE (queue.TakeReady (150, false));
    request.capZ = 20;
    EXPECT_TRUE (queue.Follow (request, 160));
    EXPECT_GT (queue.Revision (), first); // previous completion invalidated now, not at debounce
    request.capZ = 18;
    EXPECT_TRUE (queue.Follow (request, 200));
    EXPECT_FALSE (queue.TakeReady (500, true));
    EXPECT_TRUE (queue.Pending ());
    const auto latest = queue.TakeReady (501, false);
    ASSERT_TRUE (latest);
    EXPECT_EQ (latest->capZ, 18);
    EXPECT_FALSE (queue.TakeReady (600, false));
    queue.Refresh (700);
    EXPECT_FALSE (queue.TakeReady (849, false));
    EXPECT_TRUE (queue.TakeReady (850, false));
    queue.Reset ();
    EXPECT_FALSE (queue.Desired ());
    EXPECT_FALSE (queue.Pending ());
}

TEST (MassingRules, PartialPreviewDecodesWithoutAFakeEnvelopeOrTerrain)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    calc::Result result;
    std::string error;
    const std::string partial = R"({"ok":true,"outputs":{"payload":{"version":1,
      "hasEnvelope":false,"note":"Define one terrain Mesh","offsetXY":[3,3,7,3,7,7,3,7],
      "vertices":[],"normals":[],"triangles":[],"boundary":[],"offsets":[],"wires":[],"references":[],
      "parcelArea":100,"allowedArea":16,"meanZ":null,"meanASL":null,"faces":0}}})";
    ASSERT_TRUE (calc::Decode (partial, result, error)) << error;
    EXPECT_FALSE (result.hasEnvelope);
    EXPECT_EQ (result.allowedArea, 16);
    EXPECT_EQ (result.offsetXY.size (), 8u);
    EXPECT_TRUE (result.layer.meshes.empty ());
    EXPECT_TRUE (result.site.polylines.empty ());
    EXPECT_FALSE (result.hasMeanZ);
    calc::Request request;
    request.before = Page ();
    request.assignments = request.before.assignments;
    request.endpoints.assign (4, true);
    request.regulated.assign (4, true);
    std::string encoded;
    ASSERT_TRUE (calc::Encode (request, {}, false, 0, encoded, error)) << error;
    const auto parsed = evp::nodegraph::json::Parse (encoded);
    ASSERT_TRUE (parsed.ok);
    EXPECT_TRUE (parsed.value.Find ("request")->Find ("terrain")->IsNull ());
}

TEST (MassingRules, ContourRightClickSavesCapturedGuidAndNeverWritesDuringLayout)
{
    RulesWidget widget;
    widget.draft.assignments[0].distance = 2;
    widget.draft.dirty = true;
    const auto center = widget.canvas.GetCenter ();
    widget.Open ({ center.x, widget.canvas.Max.y - 16 });
    EXPECT_EQ (widget.draft.targetEdge, 0);
    EXPECT_EQ (widget.draft.targetPoint, -1);
    const auto pointer = widget.Find ("Save assignments");
    ASSERT_GT (pointer.y, 0);
    EXPECT_TRUE (widget.Frame (pointer, true).empty ());
    const auto edits = widget.Frame (pointer);
    ASSERT_EQ (edits.size (), 1u);
    EXPECT_EQ (edits[0].before.guid, "parcel");
    EXPECT_TRUE (rules::SameGeometry (edits[0].before.edges, widget.page.edges));
    EXPECT_EQ (edits[0].before.stored, widget.page.stored);
    EXPECT_EQ (edits[0].assignments[0].distance, 2);
}

TEST (MassingRules, ContourBlankSpaceMenuDiscardsWithoutSavingOrLosingEndpointFlags)
{
    RulesWidget widget;
    widget.draft.assignments[0].distance = 2;
    widget.draft.endpoints[0] = false;
    widget.draft.dirty = true;
    widget.Open ({ widget.canvas.Min.x + 5, widget.canvas.Min.y + 5 });
    EXPECT_EQ (widget.draft.targetEdge, -1);
    EXPECT_EQ (widget.draft.targetPoint, -1);
    const auto pointer = widget.Find ("Discard edits");
    ASSERT_GT (pointer.y, 0);
    EXPECT_TRUE (widget.Frame (pointer, true).empty ());
    EXPECT_TRUE (widget.Frame (pointer).empty ());
    EXPECT_FALSE (widget.draft.dirty);
    EXPECT_EQ (widget.draft.assignments[0].distance, 1);
    EXPECT_FALSE (widget.draft.endpoints[0]);
}

TEST (MassingRules, ContourEndpointMenuTogglesRunLocalElevationMembershipOnly)
{
    RulesWidget widget;
    const auto center = widget.canvas.GetCenter ();
    widget.Open ({ center.x - 84, widget.canvas.Max.y - 16 });
    EXPECT_EQ (widget.draft.targetPoint, 0);
    EXPECT_EQ (widget.draft.targetEdge, -1);
    const auto pointer = widget.Find ("Use for average elevation");
    ASSERT_GT (pointer.y, 0);
    EXPECT_TRUE (widget.Frame (pointer, true).empty ());
    EXPECT_TRUE (widget.Frame (pointer).empty ());
    EXPECT_FALSE (widget.draft.endpoints[0]);
    EXPECT_FALSE (widget.draft.dirty);
}

TEST (MassingRules, ContourAutomaticallyCapturesChangesWithoutCalculateOrSave)
{
    RulesWidget widget;
    ASSERT_EQ (widget.draft.calculations.size (), 1u); // starts when the contour is first drawn
    widget.draft.calculations.clear ();
    widget.draft.assignments[0].distance = 2;
    widget.draft.endpoints[1] = false;
    widget.draft.regulated[2] = false;
    widget.draft.calculation.landscape = 1;
    EXPECT_TRUE (widget.Frame ({ 850, 850 }).empty ());
    EXPECT_TRUE (widget.Frame ({ 850, 850 }).empty ());
    ASSERT_EQ (widget.draft.calculations.size (), 1u);
    const auto request = widget.draft.calculations[0];
    EXPECT_EQ (request.before.guid, "parcel");
    EXPECT_EQ (request.assignments[0].distance, 2);
    EXPECT_FALSE (request.endpoints[1]);
    EXPECT_FALSE (request.regulated[2]);
    EXPECT_EQ (request.landscape, 1);
    EXPECT_FALSE (widget.draft.dirty); // calculating never saves metadata
}

TEST (MassingRules, CapKeyboardAnswerIsBoundedCapturedAndAutomaticallyRecalculated)
{
    RulesWidget widget;
    widgets::NumberEdit edit;
    edit.before = widget.draft.calculations.front ();
    edit.key = "Cap Project Z";
    edit.min = 5;
    edit.max = 50;
    EXPECT_FALSE (widgets::AnswerNumber (widget.draft, edit, 51));
    EXPECT_FALSE (widgets::AnswerNumber (widget.draft, edit, 4.9));
    ASSERT_TRUE (widgets::AnswerNumber (widget.draft, edit, 30));
    widget.Frame ({ 850, 850 });
    ASSERT_EQ (widget.draft.calculations.size (), 2u);
    EXPECT_EQ (widget.draft.calculations.back ().capZ, 30);
    EXPECT_FALSE (widgets::AnswerNumber (widget.draft, edit, 25)); // inputs changed during prompt
    EXPECT_FALSE (widget.draft.dirty);                             // run-local cap, no metadata saved
}

TEST (MassingRules, RoadNoneAssignmentTransportsVerticalFlag)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    calc::Request request;
    request.before = Page (Saved ());
    request.assignments = request.before.assignments;
    request.assignments[0].mode = rules::Mode::None;
    request.assignments[0].distance = 0;
    request.endpoints.assign (4, true);
    request.regulated.assign (4, true);
    std::string json, error;
    ASSERT_TRUE (calc::Encode (request, {}, false, 0, json, error)) << error;
    const auto parsed = evp::nodegraph::json::Parse (json);
    bool vertical = false;
    ASSERT_TRUE (
        parsed.value.Find ("request")->Find ("edges")->AsArray ()->at (0).Find ("vertical")->AsBool (vertical));
    EXPECT_TRUE (vertical);
}

TEST (MassingRules, HybridAdapterEncodesFiniteCapturedGeometryWithRunLocalFlags)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    namespace js = evp::nodegraph::json;
    calc::Request request;
    request.before = Page (Saved ());
    request.assignments = request.before.assignments;
    request.regulated = { true, false, true, true };
    request.endpoints = { true, true, false, true };
    geomsrv::Mesh terrain;
    terrain.vertices = { 0, 0, 0, 20, 0, 0, 0, 20, 0 };
    terrain.triangles = { 0, 1, 2 };
    std::string input, error;
    ASSERT_TRUE (calc::Encode (request, terrain, true, 123, input, error)) << error;
    const auto json = js::Parse (input);
    ASSERT_TRUE (json.ok);
    const auto* encoded = json.value.Find ("request");
    ASSERT_NE (encoded, nullptr);
    double altitude = 0;
    EXPECT_TRUE (encoded->Find ("originAltitude")->AsDouble (altitude));
    EXPECT_EQ (altitude, 123);
    const auto* edges = encoded->Find ("edges")->AsArray ();
    ASSERT_EQ (edges->size (), 4u);
    bool regulated = true;
    EXPECT_TRUE ((*edges)[1].Find ("regulated")->AsBool (regulated));
    EXPECT_FALSE (regulated);
    bool endpoint = true;
    EXPECT_TRUE ((*edges)[2].Find ("reference")->AsBool (endpoint));
    EXPECT_FALSE (endpoint);
    terrain.triangles[0] = 999;
    const auto before = input;
    EXPECT_FALSE (calc::Encode (request, terrain, true, 123, input, error));
    EXPECT_EQ (input, before);
}

TEST (MassingRules, HybridResultRejectsMalformedIndicesAndKeepsNativeStyles)
{
    namespace calc = geomsrv::archviz::massingcalculation;
    namespace js = evp::nodegraph::json;
    const std::string valid = R"({"ok":true,"outputs":{"payload":{"version":1,
      "hasEnvelope":true,"note":"","offsetXY":[0,0,10,0,0,10],"offsets":[[1,1,0,9,1,0]],
      "vertices":[0,0,1,10,0,1,0,10,1],"normals":[0,0,1,0,0,1,0,0,1],"triangles":[0,1,2],
      "boundary":[[0,0,0,10,0,0]],"wires":[[0,0,1,10,0,1]],"references":[0,0,0],
      "parcelArea":100,"allowedArea":50,"faces":1,"meanZ":0,"meanASL":123}}})";
    calc::Result result;
    std::string error;
    ASSERT_TRUE (calc::Decode (valid, result, error)) << error;
    EXPECT_EQ (result.layer.name, "tapioca.massing.envelope");
    ASSERT_EQ (result.layer.meshes.size (), 1u);
    EXPECT_EQ (result.layer.meshes[0].rgba, 0xDCF3FAFFu);
    EXPECT_FLOAT_EQ (result.layer.meshes[0].style.opacity, 0.25f);
    EXPECT_EQ (result.site.polylines[0].rgba, 0xAA4465FFu);
    EXPECT_EQ (result.site.polylines[1].rgba, 0xA66226FFu);
    EXPECT_EQ (result.layer.polylines[0].rgba, 0xA66226FFu);
    EXPECT_EQ (result.layer.occlusion, hudtest::layers::Behind::Dash);
    EXPECT_TRUE (result.hasMeanASL);
    EXPECT_EQ (result.meanASL, 123);
    for (double index : { -1.0, 0.5, 3.0 }) {
        auto bad = valid;
        bad.replace (bad.find ("[0,1,2]"), 7, "[" + std::to_string (index) + ",1,2]");
        EXPECT_FALSE (calc::Decode (bad, result, error));
        EXPECT_EQ (result.allowedArea, 50); // atomic reject preserves last accepted result
    }
    EXPECT_FALSE (calc::Decode (R"({"ok":false,"error":"terrain coverage missing"})", result, error));
    EXPECT_EQ (error, "terrain coverage missing");
}
