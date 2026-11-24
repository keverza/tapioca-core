#include "Workspace.hpp"
#include "ArchViz/ReportPaperPreview.hpp"
#include "WorkflowState.hpp"
#include "ArchViz/HudMassingStats.hpp"
#include "ArchViz/HudFloorProgramme.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayHudCriteriaTable.hpp"
#include "Geometry/SurfaceCriteria.hpp"
#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <map>
#include <string>
#include <vector>

// ImGuiFiles.cpp, in the shared ImGui: the preview height a screenshot is taken at, or 0.
float PlaygroundCaptureHeight ();

namespace tapioca::playground {
namespace {
void GalleryState (Workspace& w, bool write)
{
    auto state = Member (w.document, "gallery");
    auto& g = w.gallery;
    const auto number = [&] (const char* key, auto& field) {
        if (write)
            Put (state, key, Value::Double (double (field)));
    };
    // Use explicit casts below to avoid reference casts when reconstructing numeric samples.
    if (!write) {
        g.huge = Number (state, "huge", g.huge);
        g.tiny = Number (state, "tiny", g.tiny);
        g.negative = Number (state, "negative", g.negative);
        g.length = Number (state, "length", g.length);
        g.fraction = float (Number (state, "fraction", g.fraction));
        g.choice = int (Number (state, "choice", g.choice));
        g.radio = int (Number (state, "radio", g.radio));
        g.colour = uint32_t (Number (state, "colour", g.colour));
        g.activeIcon = int (Number (state, "activeIcon", g.activeIcon));
        g.flag = Flag (state, "flag", g.flag);
        g.mixed = Flag (state, "mixed", g.mixed);
        const auto text = Text (state, "typed", g.typed);
        strcpy_s (g.typed, text.substr (0, sizeof (g.typed) - 1).c_str ());
        if (const auto* entries = Member (state, "log").AsArray ())
            for (const auto& entry : *entries) {
                if (g.log.size () >= 200)
                    break;
                g.log.push_back ({ Text (entry, "control"),
                                   { Flag (entry, "changed"), Flag (entry, "committed"), Flag (entry, "canceled") },
                                   int (std::clamp (Number (entry, "frame"), 0.0, double (INT_MAX))) });
            }
    }
    else {
        number ("huge", g.huge);
        number ("tiny", g.tiny);
        number ("negative", g.negative);
        number ("length", g.length);
        number ("fraction", g.fraction);
        number ("choice", g.choice);
        number ("radio", g.radio);
        number ("colour", g.colour);
        number ("activeIcon", g.activeIcon);
        Put (state, "flag", Value::Bool (g.flag));
        Put (state, "mixed", Value::Bool (g.mixed));
        Put (state, "typed", Value::String (g.typed));
        json::JsonArray log;
        for (const auto& entry : g.log)
            log.push_back (Value::Object ({ { "control", Value::String (entry.control) },
                                            { "changed", Value::Bool (entry.result.changed) },
                                            { "committed", Value::Bool (entry.result.committed) },
                                            { "canceled", Value::Bool (entry.result.canceled) },
                                            { "frame", Value::Integer (entry.frame) } }));
        Put (state, "log", Value::Array (std::move (log)));
        Put (w.document, "gallery", state);
    }
}
void ProgrammeDialogs (Workspace& w, FixtureModel& model)
{
    if (w.tableOpen) {
        ImGui::OpenPopup ("Programme table");
        w.tableOpen = false;
    }
    ImGui::SetNextWindowSize ({ 640, 400 }, ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal ("Programme table", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTable ("edit", 4, ImGuiTableFlags_SizingFixedFit)) {
            for (const auto* heading : { "Rooms", "Min m2", "Max m2", "Share (fraction)" })
                ImGui::TableSetupColumn (heading);
            ImGui::TableHeadersRow ();
            for (size_t i = 0; i < w.tableDraft.types.size (); ++i) {
                auto& row = w.tableDraft.types[i];
                ImGui::PushID (int (i));
                ImGui::TableNextRow ();
                double* fields[] = { &row.rooms, &row.minM2, &row.maxM2, &row.share };
                for (int column = 0; column < 4; ++column) {
                    ImGui::TableSetColumnIndex (column);
                    ImGui::PushID (column);
                    ImGui::SetNextItemWidth (115);
                    ImGui::InputDouble ("##value", fields[column], 0, 0, "%.3f");
                    ImGui::PopID ();
                }
                ImGui::PopID ();
            }
            ImGui::EndTable ();
        }
        if (ImGui::Button ("Apply")) {
            av::floorprogramme::Programme answer;
            if (av::hudprogramme::PrepareTable (w.tableDraft, answer, w.editError) &&
                av::hudprogramme::Answer (model.programme, w.table, answer, w.editError)) {
                model.saved = model.programme;
                model.status = "completed";
                Event (w.document, "Define Programme: table applied and saved to mock owner");
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
            ImGui::CloseCurrentPopup ();
        ImGui::TextWrapped ("%s", w.editError.c_str ());
        ImGui::EndPopup ();
    }
    if (w.colourOpen) {
        ImGui::OpenPopup ("Programme colour");
        w.colourOpen = false;
    }
    if (ImGui::BeginPopupModal ("Programme colour", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorPicker3 ("Opaque RGB", w.colourDraft, ImGuiColorEditFlags_NoTooltip);
        if (ImGui::Button ("Apply")) {
            const auto packed = [] (float channel) {
                return uint32_t (std::lround (std::clamp (channel, 0.0f, 1.0f) * 255));
            };
            const uint32_t rgba = packed (w.colourDraft[0]) << 24 | packed (w.colourDraft[1]) << 16 |
                                  packed (w.colourDraft[2]) << 8 | 255u;
            if (av::hudprogramme::Answer (model.programme, w.colour, rgba, w.editError)) {
                model.saved = model.programme;
                Event (w.document, "Define Programme: colour applied and saved to mock owner");
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
            ImGui::CloseCurrentPopup ();
        ImGui::TextWrapped ("%s", w.editError.c_str ());
        ImGui::EndPopup ();
    }
    Value dialog;
    if (ImGui::IsPopupOpen ("Programme table"))
        dialog = Value::Object ({ { "kind", Value::String ("table") },
                                  { "before", EncodeProgramme (w.table.before) },
                                  { "draft", EncodeProgramme (w.tableDraft) } });
    if (ImGui::IsPopupOpen ("Programme colour"))
        dialog = Value::Object ({ { "kind", Value::String ("colour") },
                                  { "before", EncodeProgramme (w.colour.before) },
                                  { "type", Value::Integer (int64_t (w.colour.type)) },
                                  { "r", Value::Double (w.colourDraft[0]) },
                                  { "g", Value::Double (w.colourDraft[1]) },
                                  { "b", Value::Double (w.colourDraft[2]) } });
    Put (w.document, "dialog", std::move (dialog));
}
void DrawPanel (Workspace& w, FixtureModel& model, const av::overlaylayers::Panel& look, float scale)
{
    ImGui::TextWrapped ("%s: %s", model.status.c_str (), model.message.c_str ());
    if (model.panel == "hudeditor") {
        // The production editor panel on the session the fixture loaded (EditorFixture).
        w.native.mocks.document = &w.document;
        MockScope scope (w.native.mocks);
        av::edithud::Panel (w.native.mocks.editor);
        return;
    }
    if (model.panel == "hudmassingstats") {
        if (model.stats.rows.empty () && !model.stats.hasCoverage)
            ImGui::TextDisabled ("No floor area to distribute (owner empty state).");
        const auto hovered = av::hudmassingstats::Draw (model.stats, look, scale);
        if (hovered != Text (w.document, "hovered")) {
            Put (w.document, "hovered", Value::String (hovered));
            if (!hovered.empty ())
                Event (w.document, "Stats: hovered " + hovered);
        }
        std::vector<av::massingareas::NumberEdit> numbers;
        if (av::hudmassingstats::CoefficientInputs (model.coefficients, numbers))
            Event (w.document, "Stats: area coefficients changed (production validator accepted)");
        if (!numbers.empty ()) {
            w.number = numbers.back ();
            w.numberDraft = w.number.number;
            w.numberOpen = true;
            w.editError.clear ();
            Event (w.document, "Stats: exact number requested: " + w.number.key);
        }
        if (w.numberOpen) {
            ImGui::OpenPopup ("Area coefficient");
            w.numberOpen = false;
        }
        if (ImGui::BeginPopupModal ("Area coefficient", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted (w.number.key.c_str ());
            ImGui::InputDouble ("Value", &w.numberDraft);
            if (ImGui::Button ("Apply")) {
                if (av::massingareas::Same (model.coefficients, w.number.before) &&
                    av::massingareas::Assign (model.coefficients, w.number.key, w.numberDraft)) {
                    Event (w.document, "Stats: exact coefficient applied: " + w.number.key);
                    ImGui::CloseCurrentPopup ();
                }
                else
                    w.editError = "Value is outside the production coefficient range.";
            }
            ImGui::SameLine ();
            if (ImGui::Button ("Cancel"))
                ImGui::CloseCurrentPopup ();
            ImGui::TextWrapped ("%s", w.editError.c_str ());
            ImGui::EndPopup ();
        }
        Value dialog;
        if (ImGui::IsPopupOpen ("Area coefficient")) {
            auto before = model;
            before.coefficients = w.number.before;
            dialog = Value::Object ({ { "kind", Value::String ("number") },
                                      { "before", EncodeFixture (before) },
                                      { "key", Value::String (w.number.key) },
                                      { "draft", Value::Double (w.numberDraft) } });
        }
        Put (w.document, "dialog", std::move (dialog));
    }
    else {
        const auto problem = av::floorprogramme::Problem (model.programme);
        if (!problem.empty ())
            ImGui::TextWrapped ("Validation: %s", problem.c_str ());
        std::vector<av::hudprogramme::TableEdit> tables;
        std::vector<av::hudprogramme::ColourEdit> colours;
        ImGui::SetNextItemOpen (true, ImGuiCond_Once);
        const auto before = model.programme;
        const auto outcome = av::hudprogramme::Draw (model.programme, model.saved, tables, colours);
        if (outcome.changed) {
            std::string fields;
            if (before.types.size () != model.programme.types.size ())
                fields = " type count";
            for (size_t i = 0; i < std::min (before.types.size (), model.programme.types.size ()); ++i) {
                const auto& a = before.types[i];
                const auto& b = model.programme.types[i];
                if (a.rooms != b.rooms)
                    fields += " rooms";
                if (a.minM2 != b.minM2 || a.maxM2 != b.maxM2)
                    fields += " area range";
                if (a.share != b.share)
                    fields += " shares";
                if (a.rgba != b.rgba)
                    fields += " colour";
            }
            Event (w.document, "Define Programme: changed" + fields);
        }
        if (outcome.save) {
            model.saved = model.programme;
            model.status = "completed";
            Event (w.document, "Define Programme: Save -> mock owner (no project write)");
        }
        if (!tables.empty ()) {
            w.table = tables.back ();
            w.tableDraft = w.table.before;
            w.tableOpen = true;
            w.editError.clear ();
            Event (w.document, "Define Programme: table edit requested");
        }
        if (!colours.empty ()) {
            w.colour = colours.back ();
            w.colourOpen = true;
            w.editError.clear ();
            const auto rgba = av::floorprogramme::Colour (model.programme.types[w.colour.type]);
            for (int i = 0; i < 3; ++i)
                w.colourDraft[i] = float ((rgba >> (24 - 8 * i)) & 255) / 255;
            Event (w.document, "Define Programme: colour edit requested");
        }
        ProgrammeDialogs (w, model);
    }
}
// Exposed surfaces criteria grid (overlay HUD), drawn by the production table at the
// preview's width. A playground-only rule list stands in for the takeoff's draft rules.
constexpr int kCriteriaPreview = -2;
void CriteriaPreview (Workspace& w)
{
    struct Demo {
        std::vector<av::overlaylayers::CriteriaRow> rows;
        std::map<std::string, av::overlayhud::CriteriaText> texts;
        uint32_t next = 1;
    };
    static Demo demo;
    const auto& fields = geomsrv::surfacecriteria::Fields ();
    const auto options = [] (uint32_t field) -> std::vector<std::string> {
        switch (field) {
            case 0:
                return { "Wall", "Slab", "Column", "Beam", "Roof", "Curtain Wall" };
            case 1:
                return { "A-WALL-EXTR", "A-WALL-INTR", "S-SLAB" };
            case 2:
                return { "Ground floor", "1st floor", "2nd floor" };
            case 4:
                return { "Stucco red", "Stucco yellow", "Unknown surface (generated or pen colour)" };
            case 5:
                return { "Top", "Bottom", "Side" };
            default:
                return {};
        }
    };
    const auto add = [&] (uint32_t field, uint32_t condition, std::string value, uint32_t open = 0, uint32_t close = 0,
                          bool orNext = false) {
        av::overlaylayers::CriteriaRow row;
        row.id = "playground:rule:" + std::to_string (demo.next++);
        row.field = field;
        row.condition = condition;
        row.value = std::move (value);
        row.open = open;
        row.close = close;
        row.orNext = orNext;
        row.options = options (field);
        demo.rows.push_back (row);
    };
    if (demo.rows.empty () && demo.next == 1) {
        add (0, 0, "Wall", 1);
        add (4, 2, "Stucco", 0, 0, true);
        add (5, 1, "Bottom", 0, 1);
    }
    ImGui::PushTextWrapPos (0.0f);
    ImGui::TextUnformatted ("Select objects and materials with rules. For example: Surface contains Stucco. Use OR for "
                            "several finishes. Drafts change quantities/highlights only after Apply criteria.");
    ImGui::PopTextWrapPos ();
    av::overlaylayers::PanelItem grid;
    grid.kind = av::overlaylayers::ItemKind::CriteriaGrid;
    grid.id = "criteriaGrid";
    grid.labels = fields;
    grid.columns = geomsrv::surfacecriteria::Conditions ();
    grid.criteria = demo.rows;
    std::vector<av::overlayhud::CriteriaEdit> edits;
    bool textActive = false;
    av::overlayhud::DrawCriteriaTable (grid, demo.texts, ImGui::GetContentRegionAvail ().x, edits, textActive);
    for (const auto& edit : edits) {
        const auto at =
            std::find_if (demo.rows.begin (), demo.rows.end (), [&] (const auto& row) { return row.id == edit.rowId; });
        if (at == demo.rows.end ())
            continue;
        const std::string kind = edit.kind;
        const auto value = uint32_t (edit.value);
        if (kind == "criteriaOpen")
            at->open = value;
        else if (kind == "criteriaClose")
            at->close = value;
        else if (kind == "criteriaLink")
            at->orNext = value == 1;
        else if (kind == "criteriaCondition")
            at->condition = value;
        else if (kind == "criteriaField") {
            at->field = value;
            at->options = options (value);
            at->value = at->options.empty () ? "" : at->options.front ();
        }
        else if (kind == "criteriaValue")
            at->value = edit.text;
        else if (kind == "criteriaRemove") {
            demo.rows.erase (at);
            Event (w.document, "Criteria: " + kind);
            continue;
        }
        else if (kind == "criteriaMove") {
            const auto from = at - demo.rows.begin ();
            const auto to = from + int64_t (edit.value);
            if (to >= 0 && to < int64_t (demo.rows.size ()))
                std::swap (demo.rows[size_t (from)], demo.rows[size_t (to)]);
        }
        Event (w.document, "Criteria: " + kind + " " + edit.text);
    }
    if (ImGui::Button ("Add criterion"))
        add (0, 0, "Wall");
    ImGui::SameLine ();
    if (ImGui::Button ("Clear draft"))
        demo.rows.clear ();
    ImGui::TextDisabled ("Grid minimum %.0f px at this font; preview %.0f px.",
                         av::overlayhud::MeasureCriteria (grid).minimum, ImGui::GetContentRegionAvail ().x);
}
// A Virtual editor fixture chosen in the browser replaces the editor session once; the document
// remembers which one is loaded, so a UI reload or a redraw keeps the edits made since.
void EditorFixture (Workspace& w, const json::JsonArray& fixtures, int selected)
{
    if (selected < 0 || Text (fixtures[size_t (selected)], "panel") != "hudeditor" ||
        Number (w.document, "editorLoaded", -1) == selected)
        return;
    w.native.mocks.document = &w.document;
    MockScope scope (w.native.mocks);
    std::string error;
    const auto variant = Text (fixtures[size_t (selected)], "variant");
    if (LoadEditorFixture (w.native.mocks, Member (fixtures[size_t (selected)], "data"), error))
        Event (w.document, "Virtual editor: fixture " + variant + " loaded");
    else
        Event (w.document, "Virtual editor: fixture " + variant + " rejected: " + error);
    Put (w.document, "editorLoaded", Value::Integer (selected));
}
} // namespace
bool Restore (Workspace& w, const Value& document)
{
    const auto* fixtures = Member (document, "fixtures").AsArray ();
    if (Number (document, "version") != 1 || !fixtures || fixtures->empty () || fixtures->size () > 100)
        return false;
    for (const auto& fixture : *fixtures) {
        FixtureModel model;
        std::string error;
        if (!DecodeFixture (fixture, model, error))
            return false;
    }
    w.document = document;
    w.demo = Flag (document, "demo");
    w.styleEditor = Flag (document, "styleEditor");
    strcpy_s (w.search, Text (document, "search").substr (0, sizeof (w.search) - 1).c_str ());
    const auto& dialog = Member (document, "dialog");
    const auto kind = Text (dialog, "kind");
    if (kind == "table") {
        if (!DecodeProgramme (Member (dialog, "before"), w.table.before) ||
            !DecodeProgramme (Member (dialog, "draft"), w.tableDraft))
            return false;
        w.tableOpen = true;
    }
    else if (kind == "colour") {
        if (!DecodeProgramme (Member (dialog, "before"), w.colour.before))
            return false;
        const double type = Number (dialog, "type", -1);
        if (type < 0 || type >= w.colour.before.types.size ())
            return false;
        w.colour.type = size_t (type);
        w.colourDraft[0] = float (Number (dialog, "r"));
        w.colourDraft[1] = float (Number (dialog, "g"));
        w.colourDraft[2] = float (Number (dialog, "b"));
        w.colourOpen = true;
    }
    else if (kind == "number") {
        FixtureModel before;
        std::string error;
        if (!DecodeFixture (Member (dialog, "before"), before, error))
            return false;
        w.number.before = before.coefficients;
        w.number.key = Text (dialog, "key");
        w.numberDraft = Number (dialog, "draft");
        w.numberOpen = true;
    }
    GalleryState (w, false);
    if (!RestoreNative (w.native, document))
        return false;
    return true;
}
void DrawWorkspace (Workspace& w, const TapiocaUiFrame& frame, TapiocaUiResult& result)
{
    auto fixtures = *Member (w.document, "fixtures").AsArray ();
    int selected = std::clamp (int (Number (w.document, "selected")), kCriteriaPreview, int (fixtures.size ()) - 1);
    bool dark = Flag (w.document, "dark");
    int density = std::clamp (int (Number (w.document, "density", 1)), 0, 3);
    float dpi = float (std::clamp (Number (w.document, "dpi", 0), 0.0, 4.0));
    int fontStep = std::clamp (int (Number (w.document, "fontStep", av::hudshell::kFontStepDefault)), 0,
                               int (av::hudshell::kFontStepCount) - 1);
    bool compact = Flag (w.document, "compact");
    bool nativeMode = Flag (w.document, "nativeMode", true);
    float width = float (std::clamp (Number (w.document, "previewWidth", 320), 160.0, 2400.0));
    float height = float (std::clamp (Number (w.document, "previewHeight", 740), 120.0, 2400.0));
    // A screenshot draws the preview at its tallest for its frames only; the stored height stays.
    const float storedHeight = height;
    if (PlaygroundCaptureHeight () > 0.0f)
        height = PlaygroundCaptureHeight ();
    const float scale =
        (dpi > 0 ? dpi : frame.dpiScale) * (selected == -1 ? 1.0f : av::ui::tokens::kDensitySteps[density]);
    av::hudshell::BaseStyle (scale);
    ImGui::GetStyle ().FontScaleMain = nativeMode ? 1.0f : av::hudshell::FontScaleOfStep (uint32_t (fontStep));
    av::overlaylayers::Panel look = nativeMode ? av::hudshell::PlainLook () : av::overlaylayers::Panel {};
    av::overlaylayers::ApplyTheme (look,
                                   dark ? av::overlaylayers::PanelTheme::Dark : av::overlaylayers::PanelTheme::Light);
    av::ui::ScopedStyle style;
    style.Adopt (av::hudshell::PushLook (look, scale), av::hudshell::kLookVars);
    style.Font (nullptr, 13.0f * scale);
    if (compact)
        style.Var (ImGuiStyleVar_ItemSpacing, { 4 * scale, 2 * scale });
    if (w.native.mocks.hud.captureFrame.active) {
        auto& capture = w.native.mocks.hud.captureFrame;
        if (frame.renderDevice && capture.builder)
            av::viewerhud::UpdateReportPaper (static_cast<Diligent::IRenderDevice*> (frame.renderDevice), capture,
                                              false);
        DrawNativeCapture (w.native, w.document, { uint32_t (frame.width), uint32_t (frame.height) },
                           dpi > 0 ? dpi : frame.dpiScale);
        return;
    }
    ImGui::SetNextWindowPos ({ 0, 0 });
    ImGui::SetNextWindowSize ({ frame.width, frame.height });
    ImGui::Begin ("Tapioca UI Playground", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted ("Tapioca UI Playground - production panels / deterministic mock owner");
    if (ImGui::BeginTable ("workspace", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn ("Browser", ImGuiTableColumnFlags_WidthFixed, 210 * frame.dpiScale);
        ImGui::TableSetupColumn ("Preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Inspector", ImGuiTableColumnFlags_WidthFixed, 340 * frame.dpiScale);
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::BeginChild ("browser", { 0, 0 });
        ImGui::InputTextWithHint ("##search", "Search panels / fixtures", w.search, sizeof (w.search));
        if (ImGui::Selectable ("Component gallery", selected == -1))
            selected = -1;
        ImGui::SeparatorText ("Overlay HUD");
        if (ImGui::Selectable ("Exposed surfaces criteria", selected == kCriteriaPreview))
            selected = kCriteriaPreview;
        ImGui::SeparatorText ("Native workflows");
        for (const auto& entry : { std::pair { "Massing", "@massing" },
                                   { "Analysis (Sun / View)", "@analysis" },
                                   { "Virtual editor", "@editor" } })
            if (ImGui::Selectable (entry.first, w.native.held == entry.second)) {
                nativeMode = true;
                if (selected < 0)
                    selected = 0;
                Put (w.document, "nativeRequest", Value::String (entry.second));
            }
        for (const auto* panel : { "hudeditor", "hudmassingstats", "hudprogramme" }) {
            ImGui::SeparatorText (std::strcmp (panel, "hudprogramme") == 0 ? "Define Programme"
                                  : std::strcmp (panel, "hudeditor") == 0  ? "Virtual editor fixtures"
                                                                           : "Stats");
            for (size_t i = 0; i < fixtures.size (); ++i) {
                if (Text (fixtures[i], "panel") != panel)
                    continue;
                const auto variant = Text (fixtures[i], "variant");
                if (w.search[0] && (std::string (panel) + " " + variant).find (w.search) == std::string::npos)
                    continue;
                ImGui::PushID (int (i));
                if (ImGui::Selectable (variant.c_str (), selected == int (i))) {
                    selected = int (i);
                    Put (w.document, "nativeRequest",
                         Value::String (std::strcmp (panel, "hudprogramme") == 0 ? "@massing"
                                        : std::strcmp (panel, "hudeditor") == 0  ? "@editor"
                                                                                 : av::hudshell::kStatsKey));
                }
                ImGui::PopID ();
            }
        }
        ImGui::EndChild ();
        EditorFixture (w, fixtures, selected);
        ImGui::TableSetColumnIndex (1);
        const ImVec2 stageMin = ImGui::GetCursorScreenPos ();
        const ImVec2 stageMax = stageMin + ImGui::GetContentRegionAvail ();
        ImGui::BeginChild ("preview-scroll", { 0, 0 }, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        if (!nativeMode || selected < 0) {
            ImGui::BeginChild ("production-preview", { width * frame.dpiScale, height * frame.dpiScale },
                               ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX | ImGuiChildFlags_ResizeY);
            if (selected == -1) {
                w.gallery.dark = dark;
                // Gallery already applies its own density; don't multiply it a second time.
                w.gallery.density = uint32_t (density);
                const auto previous = json::Write (Member (w.document, "gallery"), 0);
                av::ui::DrawGallery (w.gallery);
                dark = w.gallery.dark;
                density = int (w.gallery.density);
                GalleryState (w, true);
                if (previous != json::Write (Member (w.document, "gallery"), 0))
                    for (const auto& entry : w.gallery.log)
                        if (entry.frame == ImGui::GetFrameCount ())
                            Event (w.document, "Gallery: " + entry.control + (entry.result.changed ? " changed" : "") +
                                                   (entry.result.committed ? " committed" : "") +
                                                   (entry.result.canceled ? " canceled" : ""));
            }
            else if (selected == kCriteriaPreview)
                CriteriaPreview (w);
            else if (!nativeMode) {
                FixtureModel model;
                std::string error;
                if (DecodeFixture (fixtures[size_t (selected)], model, error)) {
                    DrawPanel (w, model, look, scale);
                    fixtures[size_t (selected)] = EncodeFixture (model);
                }
                else
                    ImGui::TextWrapped ("%s", error.c_str ());
            }
            const auto previewSize = ImGui::GetWindowSize ();
            width = previewSize.x / frame.dpiScale;
            height = previewSize.y / frame.dpiScale;
            ImGui::EndChild ();
        }
        ImGui::EndChild ();
        ImGui::TableSetColumnIndex (2);
        ImGui::BeginChild ("inspector", { 0, 0 });
        ImGui::SeparatorText ("Preview controls");
        ImGui::Checkbox ("Native workflow shell", &nativeMode);
        if (nativeMode)
            ImGui::TextWrapped ("Production attached width: 320 logical px x DPI/text scale. Use the dock title for "
                                "workflows; its arrow collapses the panel.");
        ImGui::Checkbox ("Dark production look", &dark);
        const char* densities[] = { "80%", "100%", "140%", "200%" };
        ImGui::Combo ("Density", &density, densities, 4);
        ImGui::SliderFloat ("DPI override (0=window)", &dpi, 0, 4, "%.2f");
        ImGui::SliderInt ("Production font step", &fontStep, 0, int (av::hudshell::kFontStepCount) - 1);
        ImGui::Checkbox ("Compact spacing", &compact);
        if (!nativeMode)
            ImGui::SliderFloat ("Preview width", &width, 160, 2400, "%.0f");
        ImGui::SliderFloat ("Preview height", &height, 120, 2400, "%.0f");
        if (ImGui::Button ("Recompute (deterministic mock)")) {
            if (selected >= 0) {
                auto status = Value::Object (
                    { { "kind", Value::String ("completed") },
                      { "message",
                        Value::String (
                            "Mock recompute retained the deterministic fixture; no geometry engine ran.") } });
                Put (fixtures[size_t (selected)], "status", status);
            }
            Event (w.document, "Recompute: deterministic mock completed; no Archicad call");
        }
        ImGui::Checkbox ("ImGui Demo", &w.demo);
        ImGui::Checkbox ("Style Editor", &w.styleEditor);
        ImGui::SeparatorText ("Build / hot reload");
        if (ImGui::Button ("Build UI DLL now"))
            result.requestBuild = 1;
        ImGui::TextWrapped ("%s", frame.status ? frame.status : "");
        ImGui::SeparatorText ("Events (host-owned snapshot)");
        if (ImGui::Button ("Clear events"))
            Put (w.document, "events", Value::Array ({}));
        if (const auto* events = Member (w.document, "events").AsArray ())
            for (auto it = events->rbegin (); it != events->rend (); ++it) {
                std::string text;
                it->AsString (text);
                ImGui::TextWrapped ("%s", text.c_str ());
            }
        if (selected >= 0 && ImGui::CollapsingHeader ("Live fixture JSON"))
            ImGui::TextWrapped ("%s", json::Write (fixtures[size_t (selected)]).c_str ());
        ImGui::EndChild ();
        ImGui::EndTable ();
        if (nativeMode && selected >= 0) {
            w.native.fontStep = uint32_t (fontStep);
            const float dpiScale = dpi > 0 ? dpi : frame.dpiScale;
            const float ui =
                dpiScale * av::ui::tokens::kDensitySteps[density] * av::hudshell::FontScaleOfStep (uint32_t (fontStep));
            DrawNative (w.native, w.document, stageMin, stageMax, { frame.width, frame.height }, look, dpiScale, ui,
                        [&] (const std::string& key) {
                            const char* wanted = key == av::hudshell::kStatsKey ? "hudmassingstats" : "hudprogramme";
                            size_t index = size_t (selected);
                            if (Text (fixtures[index], "panel") != wanted)
                                for (size_t i = 0; i < fixtures.size (); ++i)
                                    if (Text (fixtures[i], "panel") == wanted &&
                                        Text (fixtures[i], "variant") == "valid-populated") {
                                        index = i;
                                        break;
                                    }
                            FixtureModel model;
                            std::string error;
                            if (DecodeFixture (fixtures[index], model, error)) {
                                selected = int (index);
                                DrawPanel (w, model, look, ui);
                                fixtures[index] = EncodeFixture (model);
                            }
                        });
            fontStep = int (w.native.fontStep);
        }
    }
    ImGui::End ();
    if (w.demo)
        ImGui::ShowDemoWindow (&w.demo);
    if (w.styleEditor) {
        ImGui::Begin ("Style Editor", &w.styleEditor);
        ImGui::TextWrapped ("Inspection sandbox: production style is reapplied next frame.");
        ImGui::ShowStyleEditor ();
        ImGui::End ();
    }
    if (selected != int (Number (w.document, "selected")))
        Event (w.document, "Browser: fixture changed without restarting host");
    Put (w.document, "fixtures", Value::Array (std::move (fixtures)));
    Put (w.document, "selected", Value::Integer (selected));
    Put (w.document, "dark", Value::Bool (dark));
    Put (w.document, "density", Value::Integer (density));
    Put (w.document, "dpi", Value::Double (dpi));
    Put (w.document, "fontStep", Value::Integer (fontStep));
    Put (w.document, "compact", Value::Bool (compact));
    Put (w.document, "nativeMode", Value::Bool (nativeMode));
    Put (w.document, "previewWidth", Value::Double (width));
    Put (w.document, "previewHeight", Value::Double (PlaygroundCaptureHeight () > 0.0f ? storedHeight : height));
    Put (w.document, "demo", Value::Bool (w.demo));
    Put (w.document, "styleEditor", Value::Bool (w.styleEditor));
    Put (w.document, "search", Value::String (w.search));
}
} // namespace tapioca::playground
