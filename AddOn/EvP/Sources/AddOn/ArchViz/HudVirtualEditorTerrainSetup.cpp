#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditPathEdit.hpp"
#include "ArchViz/HudSlopeFields.hpp"
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue
#include <algorithm>
#include <set>

// OVERLAY-INVARIANTS.md §§8,12,14: setup authors through existing drawing/history;
// native capture is an explicit queued action, never a layout/Present side effect.
namespace geomsrv::archviz::edithud {
namespace {
bool Idle (const Session& s)
{
    return !s.close && !s.terrainCapture && !GetTerrainDesignStatus (s).pending && !s.edits.InGesture () &&
           !GizmoModeLocked (s) && s.drawing.empty () && !s.landscapeCommand;
}

// The section draft as one document edit: identity and range stations are the path tools'.
bool ApplyField (edit::Section& target, const edit::Section& value, SectionField field, std::string& error)
{
    switch (field) {
        case SectionField::WidthLeft:
            target.widthLeft = value.widthLeft;
            return true;
        case SectionField::WidthRight:
            target.widthRight = value.widthRight;
            return true;
        case SectionField::Taper:
            // The end widths are independent fields. A taper-only choice must
            // not copy the first selected range's values into every range.
            target.taper = value.taper;
            return true;
        case SectionField::EndLeft:
            target.endLeft = value.endLeft;
            return true;
        case SectionField::EndRight:
            target.endRight = value.endRight;
            return true;
        case SectionField::Crossfall:
            target.crossfall = value.crossfall;
            return true;
        case SectionField::CrossfallUnit:
            return tapioca::geometry::ConvertTerrainSlope (target.crossfall.slope, value.crossfall.slope.unit, 1,
                                                           target.crossfall.slope, error);
        case SectionField::Fall:
            target.fall = value.fall;
            return true;
        case SectionField::Shoulder:
            target.shoulder = value.shoulder;
            return true;
        case SectionField::Cut:
            target.cutSlope = value.cutSlope;
            return true;
        case SectionField::CutUnit:
            return tapioca::geometry::ConvertTerrainSlope (target.cutSlope, value.cutSlope.unit, 1, target.cutSlope,
                                                           error);
        case SectionField::Fill:
            target.fillSlope = value.fillSlope;
            return true;
        case SectionField::FillUnit:
            return tapioca::geometry::ConvertTerrainSlope (target.fillSlope, value.fillSlope.unit, 1, target.fillSlope,
                                                           error);
        case SectionField::Rounding:
            target.rounding = value.rounding;
            return true;
    }
    error = "TerrainSectionField: unknown mixed field";
    return false;
}

edit::Operation SectionOperation (const TerrainSectionDraft& request)
{
    return [request] (edit::Document& doc, std::string& error) {
        if (request.sections.empty ())
            return false;
        const auto selected = request.sections;
        const auto ordered = selected;
        if (!std::is_sorted (ordered.begin (), ordered.end ()) ||
            (request.value.fall != edit::Fall::Left && request.value.fall != edit::Fall::Right &&
             request.value.fall != edit::Fall::Crown)) {
            error = "TerrainSectionIdentity: invalid range selection or fall hand.";
            return false;
        }
        return edit::EditPath (
            doc, request.path,
            [&] (edit::Path& path, std::string& error) {
                size_t matched = 0;
                for (const auto id : selected) {
                    const auto found = std::find_if (path.sections.begin (), path.sections.end (),
                                                     [id] (const edit::Section& section) { return section.id == id; });
                    if (found == path.sections.end ()) {
                        error = "TerrainSectionStale: reload the selected ranges before applying.";
                        return false;
                    }
                    ++matched;
                    if (found->fall != edit::Fall::Left && found->fall != edit::Fall::Right &&
                        found->fall != edit::Fall::Crown) {
                        error = "TerrainSectionIdentity: invalid fall hand.";
                        return false;
                    }
                    for (uint32_t bit = 1; bit <= uint32_t (SectionField::Rounding); bit <<= 1) {
                        // Clear bits are mixed/preserved; single-range applies all.
                        if (selected.size () != 1 && !(request.mixed & bit))
                            continue;
                        if (!ApplyField (*found, request.value, static_cast<SectionField> (bit), error))
                            return false;
                    }
                }
                return matched == selected.size ();
            },
            error);
    };
}

bool Same (const edit::PathCrossfall& a, const edit::PathCrossfall& b)
{
    return a.negative == b.negative && a.slope.a == b.slope.a && a.slope.b == b.slope.b;
}

bool Ready (Session& s)
{
    if (s.edits.Doc ().landscape && Idle (s))
        return true;
    s.message = "Finish the current input or terrain build before changing terrain setup.";
    return false;
}

bool BeginDrawing (Session& s, bool property)
{
    if (!Ready (s))
        return false;
    s.tool = property ? Tool::Polyline : Tool::Path;
    s.landscapeMode = LandscapeMode::DrawSource;
    s.landscapeRole = property ? tapioca::geometry::SourceRole::PropertyLine : tapioca::geometry::SourceRole::Guide;
    s.message = property ? "Draw the property boundary in the view; close it on the first point."
                         : "Draw the design path in the view; press Enter to finish.";
    return true;
}
} // namespace

uint32_t SectionFieldBit (SectionField field)
{
    return static_cast<uint32_t> (field);
}

std::vector<edit::Id> TerrainDesignPaths (const Session& s)
{
    std::vector<edit::Id> result;
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return result;
    std::set<edit::Id> authored;
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.role == tapioca::geometry::SourceRole::PathCenterline && source.pathId)
            authored.insert (source.pathId);
    for (const auto& path : doc.paths)
        if (authored.count (path.id))
            result.push_back (path.id);
    return result;
}

edit::Id ResolveTerrainDesignPath (Session& s)
{
    const auto paths = TerrainDesignPaths (s);
    const auto available = [&] (edit::Id path) {
        return std::find (paths.begin (), paths.end (), path) != paths.end ();
    };
    if (s.terrainPath)
        return available (s.terrainPath) ? s.terrainPath : edit::kNoId;
    std::set<edit::Id> selected;
    for (const auto& ref : s.selection.Items ())
        if (edit::Exists (s.edits.Doc (), ref) && available (ref.entity))
            selected.insert (ref.entity);
    if (selected.size () == 1)
        s.terrainPath = *selected.begin ();
    // Outline terrain grades the whole network; this is its initial inspector/
    // band owner, not permission to draw. Existing paths must be usable without
    // manufacturing another one. Selection and explicit choices still win.
    else if (!paths.empty ())
        s.terrainPath = paths.front ();
    return s.terrainPath;
}

bool ChooseTerrainDesignPath (Session& s, edit::Id path)
{
    if (!Ready (s))
        return false;
    const auto paths = TerrainDesignPaths (s);
    if (std::find (paths.begin (), paths.end (), path) == paths.end ()) {
        s.message = "Choose a live authored landscape path, not a native element or unbound curve.";
        return false;
    }
    s.terrainPath = path;
    if (s.terrainSectionDraft && s.terrainSectionDraft->path != path)
        s.terrainSectionDraft.reset ();
    s.message.clear ();
    return true;
}

bool EditTerrainDesignPath (Session& s)
{
    if (!Ready (s))
        return false;
    const auto path = ResolveTerrainDesignPath (s);
    if (!path) {
        s.message = "Draw or select an authored design path first.";
        return false;
    }
    s.selection.Set ({ { path, edit::Part::Entity, 0 } });
    s.profilePath = path;
    s.trimPreview.reset ();
    s.tool = Tool::Select;
    s.landscapeMode = LandscapeMode::Select;
    s.publishedRevision = UINT64_MAX;
    s.message = "Edit this path's profile and cross-section in the view or the path panel.";
    return true;
}

bool DrawTerrainDesignPath (Session& s)
{
    return BeginDrawing (s, false);
}
bool DrawTerrainPropertyLine (Session& s)
{
    return BeginDrawing (s, true);
}

bool BeginTerrainSectionEdit (Session& s, const std::vector<edit::Id>& sections)
{
    if (!Ready (s))
        return false;
    const auto path = ResolveTerrainDesignPath (s);
    const auto* authored = edit::FindPath (s.edits.Doc (), path);
    if (authored && !sections.empty ()) {
        TerrainSectionDraft candidate;
        candidate.path = path;
        candidate.section = sections.front ();
        candidate.revision = s.edits.Doc ().revision;
        // Set bits are fields to apply. Start common, then preserve actual
        // differences; a one-range draft therefore keeps every field editable.
        candidate.mixed = (SectionFieldBit (SectionField::Rounding) << 1) - 1;
        for (const auto id : sections) {
            const auto found = std::find_if (authored->sections.begin (), authored->sections.end (),
                                             [id] (const edit::Section& value) { return value.id == id; });
            if (found == authored->sections.end ())
                break;
            candidate.sections.push_back (id);
            if (candidate.sections.size () == 1)
                candidate.value = *found;
            const auto& first = candidate.value;
            if (candidate.sections.size () == 1)
                continue; // A single range has no differing fields to mark.
            const auto note = [&] (SectionField field, bool differs) {
                if (differs)
                    candidate.mixed &= ~SectionFieldBit (field);
            };
            note (SectionField::WidthLeft, first.widthLeft != found->widthLeft);
            note (SectionField::WidthRight, first.widthRight != found->widthRight);
            note (SectionField::Taper, first.taper != found->taper);
            note (SectionField::EndLeft, first.endLeft != found->endLeft);
            note (SectionField::EndRight, first.endRight != found->endRight);
            note (SectionField::Crossfall, !Same (first.crossfall, found->crossfall));
            note (SectionField::CrossfallUnit, first.crossfall.slope.unit != found->crossfall.slope.unit);
            note (SectionField::Fall, first.fall != found->fall);
            note (SectionField::Shoulder, first.shoulder != found->shoulder);
            note (SectionField::Cut, first.cutSlope.a != found->cutSlope.a || first.cutSlope.b != found->cutSlope.b);
            note (SectionField::CutUnit, first.cutSlope.unit != found->cutSlope.unit);
            note (SectionField::Fill,
                  first.fillSlope.a != found->fillSlope.a || first.fillSlope.b != found->fillSlope.b);
            note (SectionField::FillUnit, first.fillSlope.unit != found->fillSlope.unit);
            note (SectionField::Rounding, first.rounding != found->rounding);
        }
        if (candidate.sections.size () == sections.size () &&
            std::is_sorted (candidate.sections.begin (), candidate.sections.end ()) &&
            std::adjacent_find (candidate.sections.begin (), candidate.sections.end ()) == candidate.sections.end ()) {
            if (candidate.sections.size () == 1)
                candidate.mixed = (SectionFieldBit (SectionField::Rounding) << 1) - 1;
            s.terrainSectionDraft = std::make_shared<TerrainSectionDraft> (std::move (candidate));
            s.message.clear ();
            return true;
        }
    }
    s.message = "TerrainSectionMissing: the chosen path range no longer exists.";
    return false;
}

bool BeginTerrainSectionEdit (Session& s, edit::Id section)
{
    return BeginTerrainSectionEdit (s, std::vector<edit::Id> { section });
}

bool ApplyTerrainSectionEdit (Session& s)
{
    if (!Ready (s))
        return false;
    const auto draft = s.terrainSectionDraft;
    if (!draft || draft->revision != s.edits.Doc ().revision || draft->path != ResolveTerrainDesignPath (s)) {
        s.message = "TerrainSectionStale: reload section values before applying over a changed document.";
        return false;
    }
    const auto request = *draft; // The operation owns its typed values, never a mutable UI borrow.
    const bool changed = s.edits.Apply (SectionOperation (request));
    if (!changed) {
        s.message = s.edits.Message ().empty () ? "Cross-section unchanged." : s.edits.Message ();
        return false;
    }
    s.terrainSectionDraft = std::make_shared<TerrainSectionDraft> (request);
    s.terrainSectionDraft->revision = s.edits.Doc ().revision;
    s.terrainSectionDraft->section = request.section;
    s.publishedRevision = UINT64_MAX;
    s.message = "Cross-section applied. Terrain preview follows the document revision.";
    return true;
}

bool BeginTerrainSectionDrag (Session& s)
{
    const auto draft = s.terrainSectionDraft;
    if (s.terrainSectionDrag || !Ready (s) || !draft || draft->revision != s.edits.Doc ().revision ||
        !s.edits.Begin ({ draft->path, edit::Part::Entity, 0 })) {
        if (s.message.empty ())
            s.message = "TerrainSectionStale: reload section values before changing them.";
        return false;
    }
    s.terrainSectionDrag = true;
    s.terrainSectionSliderId = 0;
    s.terrainSectionCancelled = false;
    s.terrainSectionBefore = std::make_shared<TerrainSectionDraft> (*draft);
    return true;
}

bool CancelTerrainSectionDrag (Session& s)
{
    if (!s.terrainSectionDrag)
        return false;
    s.terrainSectionDrag = false;
    s.terrainSectionCancelled = true;
    if (s.edits.InGesture ())
        s.edits.Cancel ();
    // The typed values go back too, in place: the panel's widgets hold references into the draft.
    if (s.terrainSectionDraft && s.terrainSectionBefore) {
        *s.terrainSectionDraft = *s.terrainSectionBefore;
        s.terrainSectionDraft->revision = s.edits.Doc ().revision;
    }
    s.publishedRevision = UINT64_MAX;
    s.message.clear ();
    return true;
}

namespace {
// After Esc, until the release: whatever the held slider wrote goes back. True when it did.
bool KeepCancelledSection (Session& s)
{
    if (!s.terrainSectionCancelled)
        return false;
    if (s.terrainSectionDraft && s.terrainSectionBefore) {
        *s.terrainSectionDraft = *s.terrainSectionBefore;
        s.terrainSectionDraft->revision = s.edits.Doc ().revision;
    }
    return true;
}
} // namespace

bool MoveTerrainSectionDrag (Session& s)
{
    const auto draft = s.terrainSectionDraft;
    if (!s.terrainSectionDrag || !draft)
        return false;
    // Each move from the gesture's start: the draft holds every slider's value, not a delta.
    const bool moved = s.edits.Move (SectionOperation (*draft));
    draft->revision = s.edits.Doc ().revision;
    s.publishedRevision = UINT64_MAX;
    s.message = moved ? "" : s.edits.Message ();
    return moved;
}

void EndTerrainSectionDrag (Session& s)
{
    if (!s.terrainSectionDrag) {
        // The release of a slider Esc already gave up: nothing to record.
        s.terrainSectionCancelled = false;
        s.terrainSectionBefore.reset ();
        return;
    }
    s.terrainSectionBefore.reset ();
    s.terrainSectionDrag = false;
    s.terrainSectionSliderId = 0;
    s.edits.End ();
    if (s.terrainSectionDraft)
        s.terrainSectionDraft->revision = s.edits.Doc ().revision;
    s.publishedRevision = UINT64_MAX;
}

namespace {
constexpr SlopeFieldLabels s_pathCutLabels {
    "Cut slope unit",
    "Cut slope",
    "Cut slope (degrees)",
    "Cut rise",
    "Cut run",
    "Cut height (m)",
    "Cut plan run (m)",
    "Path Cut: positive finite grade; vertical is unsupported. Height/run uses model metres.",
    false,
    5,
    200,
    "%.0f %%"
};
constexpr SlopeFieldLabels s_pathCrossfallLabels {
    "Crossfall unit",
    "Crossfall",
    "Crossfall (degrees)",
    "Crossfall rise",
    "Crossfall run",
    "Crossfall height (m)",
    "Crossfall plan run (m)",
    "Path crossfall: finite magnitude below 100 percent; vertical is unsupported. Sign and Falls to stay separate.",
    true,
    0.01,
    100,
    "%.1f %%"
};
constexpr SlopeFieldLabels s_pathFillLabels {
    "Fill slope unit",
    "Fill slope",
    "Fill slope (degrees)",
    "Fill rise",
    "Fill run",
    "Fill height (m)",
    "Fill plan run (m)",
    "Path Fill: positive finite grade; vertical is unsupported. Height/run uses model metres.",
    false,
    5,
    200,
    "%.0f %%"
};

bool SectionSlopeFields (Session& s, const SlopeFieldLabels& labels, tapioca::geometry::TerrainSlope& slope,
                         SectionField valueField, SectionField unitField, bool mixedValue, bool mixedUnit)
{
    bool edited = false, released = false;
    const auto previousUnit = slope.unit;
    SlopeFields (
        labels, slope, s.message,
        [&] (const char* label, double& field, double lo, double hi, const char* format) {
            const auto id = ImGui::GetID (label);
            if (s.terrainSectionDrag && s.terrainSectionSliderId == id) {
                lo = s.terrainSectionSliderMin;
                hi = s.terrainSectionSliderMax;
            }
            const char* shown = mixedValue ? "mixed" : format;
            ImGui::SliderScalar (label, ImGuiDataType_Double, &field, &lo, &hi, shown);
            if (ImGui::IsItemActivated () && BeginTerrainSectionDrag (s)) {
                s.terrainSectionSliderId = id;
                s.terrainSectionSliderMin = lo;
                s.terrainSectionSliderMax = hi;
            }
            edited |= ImGui::IsItemEdited ();
            released |= ImGui::IsItemDeactivated ();
            return ImGui::IsItemEdited ();
        },
        mixedUnit);
    if (edited && KeepCancelledSection (s)) {
        if (released)
            EndTerrainSectionDrag (s);
        return false;
    }
    const bool unitEdited = previousUnit != slope.unit;
    if (unitEdited)
        s.terrainSectionDraft->mixed |= SectionFieldBit (unitField);
    else if (edited)
        s.terrainSectionDraft->mixed |= SectionFieldBit (valueField);
    // The shared control first converts displayed degrees back to the authored
    // radians. Only then may the live section operation capture the draft.
    if (edited)
        MoveTerrainSectionDrag (s);
    if (released)
        EndTerrainSectionDrag (s);
    return unitEdited;
}

void SectionPanel (Session& s, edit::Id path)
{
    if (!s.edits.InGesture ())
        s.terrainSectionSliderId = 0;
    const auto* authored = edit::FindPath (s.edits.Doc (), path);
    if (!authored || authored->sections.empty () || !ImGui::TreeNode ("Terrain cross-section"))
        return;
    const auto sections = authored->sections;
    std::vector<edit::Id> chosen;
    for (const auto& ref : s.selection.Items ())
        if (ref.entity == path && ref.part == edit::Part::Range)
            chosen.push_back (ref.item);
    if (chosen.empty () && s.terrainSectionDraft && s.terrainSectionDraft->path == path)
        chosen = s.terrainSectionDraft->sections;
    if (chosen.empty ())
        chosen.push_back (sections.front ().id);
    std::sort (chosen.begin (), chosen.end ());
    chosen.erase (std::unique (chosen.begin (), chosen.end ()), chosen.end ());
    if ((!s.terrainSectionDraft || s.terrainSectionDraft->path != path || s.terrainSectionDraft->sections != chosen) &&
        Idle (s))
        BeginTerrainSectionEdit (s, chosen);
    if (!s.terrainSectionDraft || s.terrainSectionDraft->path != path) {
        ImGui::TextDisabled ("Finish the current input to edit section values.");
        ImGui::TreePop ();
        return;
    }
    const auto range = chosen.size () == 1                  ? "Range " + std::to_string (chosen.front ())
                       : chosen.size () == sections.size () ? "All ranges"
                                                            : std::to_string (chosen.size ()) + " selected";
    if (ImGui::BeginCombo ("Section ranges", range.c_str ())) {
        if (ImGui::Selectable ("All ranges")) {
            std::vector<edit::Id> all;
            for (const auto& section : sections)
                all.push_back (section.id);
            BeginTerrainSectionEdit (s, all);
        }
        for (const auto& section : sections) {
            const auto label =
                "Range " + std::to_string (section.id) + " from " + std::to_string (section.station) + " m";
            if (ImGui::Selectable (label.c_str (), chosen.size () == 1 && section.id == chosen.front ()))
                BeginTerrainSectionEdit (s, section.id);
        }
        ImGui::EndCombo ();
    }
    const auto draft = s.terrainSectionDraft;
    const auto mixed = [&] (SectionField field) { return (draft->mixed & SectionFieldBit (field)) == 0; };
    const auto common = [&] (SectionField field) { draft->mixed |= SectionFieldBit (field); };
    const bool stale = draft->revision != s.edits.Doc ().revision && !s.terrainSectionDrag;
    if (stale)
        ImGui::TextWrapped (
            "Section values are stale. Reload before applying; newer document edits will not be overwritten.");
    ImGui::BeginDisabled (stale);
    auto& value = draft->value;
    // Sliders (the user, 2026-10-24): "apply while dragging and only release becomes an undo
    // step". Holding one opens an editor gesture, every change moves it (the preview rebuilds
    // live) and the release ends it as one undo entry. Ctrl+click types a value, the same way.
    bool release = false;
    const auto slide = [&] (SectionField field, const char* label, double& number, double lo, double hi,
                            const char* format) {
        const char* shown = mixed (field) ? "mixed" : format;
        ImGui::SliderScalar (label, ImGuiDataType_Double, &number, &lo, &hi, shown);
        if (ImGui::IsItemActivated ())
            BeginTerrainSectionDrag (s);
        if (ImGui::IsItemEdited () && !KeepCancelledSection (s)) {
            common (field);
            MoveTerrainSectionDrag (s);
        }
        if (ImGui::IsItemDeactivated ())
            EndTerrainSectionDrag (s);
    };
    slide (SectionField::WidthLeft, "Left width", value.widthLeft, 0.0, 10.0, "%.2f m");
    slide (SectionField::WidthRight, "Right width", value.widthRight, 0.0, 10.0, "%.2f m");
    bool taper = value.taper;
    if (mixed (SectionField::Taper))
        ImGui::PushItemFlag (ImGuiItemFlags_MixedValue, true);
    const bool taperChanged = ImGui::Checkbox ("Taper widths", &taper);
    if (mixed (SectionField::Taper))
        ImGui::PopItemFlag ();
    if (taperChanged) {
        bool accepted = true;
        if (draft->sections.size () == 1) {
            std::vector<edit::Section> changed { value };
            accepted = edit::SetTaper (changed, value.id, taper, -1.0, -1.0, s.message);
            if (accepted)
                value = changed.front ();
        }
        else
            value.taper = taper;
        if (accepted) {
            common (SectionField::Taper);
            release = true;
        }
    }
    if (value.taper) {
        slide (SectionField::EndLeft, "Left end width", value.endLeft, 0.0, 10.0, "%.2f m");
        slide (SectionField::EndRight, "Right end width", value.endRight, 0.0, 10.0, "%.2f m");
        ImGui::TextWrapped ("End widths apply at the next range's start, or at the path end for the last range.");
    }
    const bool crossfallParts = mixed (SectionField::Crossfall);
    const bool crossfallUnit = mixed (SectionField::CrossfallUnit);
    release |= SectionSlopeFields (s, s_pathCrossfallLabels, value.crossfall.slope, SectionField::Crossfall,
                                   SectionField::CrossfallUnit, crossfallParts, crossfallUnit);
    const char* fall = mixed (SectionField::Fall)        ? "mixed"
                       : value.fall == edit::Fall::Left  ? "Left"
                       : value.fall == edit::Fall::Right ? "Right"
                                                         : "Crown";
    if (ImGui::BeginCombo ("Falls to", fall)) {
        for (auto hand : { edit::Fall::Left, edit::Fall::Right, edit::Fall::Crown }) {
            const char* name = hand == edit::Fall::Left ? "Left" : hand == edit::Fall::Right ? "Right" : "Crown";
            if (ImGui::Selectable (name, !mixed (SectionField::Fall) && value.fall == hand) &&
                (mixed (SectionField::Fall) || value.fall != hand)) {
                value.fall = hand;
                common (SectionField::Fall);
                release = true;
            }
        }
        ImGui::EndCombo ();
    }
    slide (SectionField::Shoulder, "Shoulder", value.shoulder, 0.0, 5.0, "%.2f m");
    release |= SectionSlopeFields (s, s_pathCutLabels, value.cutSlope, SectionField::Cut, SectionField::CutUnit,
                                   mixed (SectionField::Cut), mixed (SectionField::CutUnit));
    release |= SectionSlopeFields (s, s_pathFillLabels, value.fillSlope, SectionField::Fill, SectionField::FillUnit,
                                   mixed (SectionField::Fill), mixed (SectionField::FillUnit));
    slide (SectionField::Rounding, "Rounding", value.rounding, 0.0, 10.0, "%.2f m");
    if (release)
        ApplyTerrainSectionEdit (s);
    ImGui::EndDisabled ();
    if (ImGui::Button ("Reload section values"))
        BeginTerrainSectionEdit (s, draft->sections);
    ImGui::TextWrapped (
        "Sliders update while dragging; release records one undo step. Ctrl+click a slider to type a value. "
        "Profile elevations stay in the path profile.");
    ImGui::TreePop ();
}
} // namespace

void TerrainSetupPanel (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape || !ImGui::CollapsingHeader ("Topography preview", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::TextWrapped ("Region and path terrain preview. Enable a region target or select an authored path, then "
                        "capture one native Mesh. "
                        "No native terrain is written.");
    const auto paths = TerrainDesignPaths (s);
    ResolveTerrainDesignPath (s);
    ImGui::BeginDisabled (!Idle (s) && !s.terrainSectionDrag);
    if (paths.empty ())
        ImGui::TextWrapped ("No authored design paths yet. Regions can grade without a path. "
                            "Draw a path only when you also want path-driven grading.");
    else {
        const auto available = std::find (paths.begin (), paths.end (), s.terrainPath) != paths.end ();
        const auto label = available ? "Path " + std::to_string (s.terrainPath) : "Select an authored path";
        if (ImGui::BeginCombo ("Design path", label.c_str ())) {
            for (auto path : paths) {
                const auto name = "Path " + std::to_string (path);
                if (ImGui::Selectable (name.c_str (), s.terrainPath == path))
                    ChooseTerrainDesignPath (s, path);
            }
            ImGui::EndCombo ();
        }
    }
    if (ImGui::Button ("Draw design path"))
        DrawTerrainDesignPath (s);
    // Re-resolve after the combo: button readiness must use this frame's choice.
    const auto chosen = ResolveTerrainDesignPath (s);
    if (chosen) {
        ImGui::SameLine ();
        if (ImGui::Button ("Edit design path"))
            EditTerrainDesignPath (s);
    }
    if (chosen)
        SectionPanel (s, chosen);
    if (!doc.landscape->propertyLineSourceId) {
        ImGui::TextWrapped ("Without a property line, the topography boundary limits the design area. "
                            "Draw a property line to override it.");
        if (ImGui::Button ("Draw property line"))
            DrawTerrainPropertyLine (s);
    }
#if defined(TAPIOCA_GEOMETRY_RUST)
    // The terrain first, in any order with the path (the user, 2026-10-24).
    ImGui::TextWrapped ("Select the terrain Mesh in Archicad before locking the view, then set it here. The "
                        "selection is cleared once it is read.");
    if (ImGui::Button ("Set terrain from selection"))
        RequestTerrainCapture (s, true);
    if (!TerrainGuid (s).empty ()) {
        ImGui::SameLine ();
        if (ImGui::Button ("Recapture terrain"))
            RequestTerrainCapture (s, false);
    }
#else
    ImGui::TextDisabled ("Terrain preview requires the Rust geometry build.");
#endif
    ImGui::EndDisabled ();
    if (s.terrainCapture)
        ImGui::TextUnformatted ("Capturing topography...");
    if (const auto guid = TerrainGuid (s); !guid.empty ())
        ImGui::TextWrapped ("Terrain Mesh: %s%s", guid.c_str (),
                            s.terrainBaseline ? " (waiting for a design path)" : "");
}
} // namespace geomsrv::archviz::edithud
