// ArchViz/HudSection -- see the header.

#include "ArchViz/HudSection.hpp"

#include "ArchViz/HudShell.hpp"

#include <imgui.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>

namespace geomsrv {
namespace archviz {
namespace hudsection {

namespace meta = metadata;
namespace layers = overlaylayers;

namespace {

constexpr char kMenu[] = "##tapioca.floors";
// The narrowest a floor's bar is drawn, of the widest's: a small floor stays a target.
constexpr float kNarrowest = 0.25f;

std::string StoreyLabel (const ProjectStoreys& storeys, int storey)
{
    for (size_t i = 0; i < storeys.indices.size () && i < storeys.names.size (); ++i)
        if (storeys.indices[i] == storey && !storeys.names[i].empty ())
            return storeys.names[i];
    return std::to_string (storey);
}

std::string Area (double areaM2)
{
    char text[32] = {};
    std::snprintf (text, sizeof (text), "%.0f m\xC2\xB2", areaM2);
    return text;
}

// "Floors 3-7", "Floor 3".
std::string RunText (const Run& run)
{
    return run.first == run.last ? "Floor " + std::to_string (run.first)
                                 : "Floors " + std::to_string (run.first) + "-" + std::to_string (run.last);
}

const hudmeta::Option* OptionOf (const Section& section, const std::string& value)
{
    for (const hudmeta::Option& option : section.options)
        if (option.value == value)
            return &option;
    return nullptr;
}

// What a floor says: its parts' value when they agree, "mixed" when they do not.
std::string FloorValue (const Section& section, const Floor& floor)
{
    std::string value;
    for (size_t i = 0; i < floor.parts.size (); ++i) {
        if (i > 0 && floor.parts[i].value != value)
            return "mixed";
        value = floor.parts[i].value;
    }
    const hudmeta::Option* option = OptionOf (section, value);
    return option != nullptr ? option->label : value;
}

} // namespace

bool operator== (const Run& a, const Run& b)
{
    return (a.Empty () && b.Empty ()) || (a.first == b.first && a.last == b.last);
}

Section Build (const std::vector<Slab>& slabs, const ProjectStoreys& storeys, const meta::ProjectSchema& schema,
               const std::string& property)
{
    Section section;
    section.known = true;
    // The first enumeration that may be assigned over floors: what a floor is coloured by.
    const meta::PropertyDefinition* key = nullptr;
    for (const meta::PropertyDefinition& definition : schema.properties)
        if ((property.empty () ? definition.type == meta::ValueType::Enum : definition.key == property) &&
            std::find (definition.domains.begin (), definition.domains.end (), meta::kFloorDomain) !=
                definition.domains.end ()) {
            key = &definition;
            break;
        }
    if (key != nullptr) {
        section.key = key->key;
        section.keyLabel = key->label.empty () ? key->key : key->label;
        section.valueType = key->type;
        if (const meta::Enumeration* options = schema.FindEnumeration (key->enumId))
            for (const meta::EnumOption& option : options->options)
                section.options.push_back (
                    { option.value, option.label.empty () ? option.value : option.label, option.rgba });
    }
    std::map<int, Floor> rows;
    for (const Slab& slab : slabs) {
        Span span { slab.guid, INT_MAX, INT_MIN };
        for (size_t k = 0; k < slab.floors.size () && k < slab.storeys.size (); ++k) {
            const int storey = slab.storeys[k];
            Floor& row = rows[storey];
            if (row.parts.empty ()) {
                row.storey = storey;
                row.base = slab.floors[k].base;
            }
            row.base = (std::min) (row.base, slab.floors[k].base);
            Part part;
            part.guid = slab.guid;
            part.areaM2 = slab.floors[k].areaM2;
            if (key != nullptr) {
                // The floor's own value, or the element's where no range says one.
                const meta::Property* value = meta::RangeValue (slab.meta, meta::kFloorDomain, storey, key->key);
                if (value == nullptr)
                    value = meta::FindProperty (slab.meta, key->key);
                if (value != nullptr) {
                    part.value = value->value.s;
                    if (const hudmeta::Option* option = OptionOf (section, part.value))
                        part.rgba = option->rgba;
                }
            }
            row.areaM2 += part.areaM2;
            row.parts.push_back (std::move (part));
            span.low = (std::min) (span.low, storey);
            span.high = (std::max) (span.high, storey);
        }
        if (span.low <= span.high)
            section.spans.push_back (span);
    }
    for (auto& entry : rows) {
        entry.second.label = StoreyLabel (storeys, entry.first);
        section.widestM2 = (std::max) (section.widestM2, entry.second.areaM2);
        section.floors.push_back (std::move (entry.second));
    }
    return section;
}

std::vector<hudmeta::Edit> RunEdits (const Section& section, const Run& run, const std::string& value, bool clear)
{
    std::vector<hudmeta::Edit> edits;
    if (run.Empty () || section.key.empty ())
        return edits;
    for (const Span& span : section.spans) {
        const int from = (std::max) (run.first, span.low), to = (std::min) (run.last, span.high);
        if (from > to)
            continue;
        hudmeta::Edit edit;
        edit.id = section.key;
        edit.action = clear ? hudmeta::Edit::Action::Clear : hudmeta::Edit::Action::Set;
        edit.kind = hudmeta::FieldKind::Choice;
        edit.type = section.valueType;
        edit.text = value;
        edit.label = section.keyLabel + ", " + RunText (run);
        edit.domain = meta::kFloorDomain;
        edit.from = double (from);
        edit.to = double (to);
        edit.element = span.guid;
        edits.push_back (std::move (edit));
    }
    return edits;
}

std::vector<hudmeta::Edit> Diagram (const Section& section, Run& run, const layers::Panel& look, float scale)
{
    std::vector<hudmeta::Edit> edits;
    if (!section.known || section.floors.empty ())
        return edits;
    ImGui::SeparatorText ("Building section");
    const ImGuiIO& io = ImGui::GetIO ();
    const float em = ImGui::GetFontSize ();
    const float row = std::floor (em * 1.2f), gap = (std::max) (1.0f, std::floor (scale));
    const size_t count = section.floors.size ();
    float labels = 0.0f;
    for (const Floor& floor : section.floors)
        labels = (std::max) (labels, ImGui::CalcTextSize (floor.label.c_str ()).x);
    labels = std::ceil (labels + em * 0.5f);
    const float width = (std::max) (ImGui::GetContentRegionAvail ().x, labels + 4.0f * em);
    const ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##section", ImVec2 (width, float (count) * (row + gap)),
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered (), active = ImGui::IsItemActive ();
    // The floor under the pointer: the rows go up from the bottom, the highest floor first.
    const auto under = [&] () -> int {
        const int down = int (std::floor ((io.MousePos.y - origin.y) / (row + gap)));
        return int (count) - 1 - (std::min) ((std::max) (down, 0), int (count) - 1);
    };
    const int pointed = hovered || active ? under () : -1;

    // ---- picking: a press, a shift-press, a drag ----------------------------------------------
    ImGuiStorage* const storage = ImGui::GetStateStorage ();
    const ImGuiID anchorId = ImGui::GetID ("##anchor");
    if (pointed >= 0) {
        const int storey = section.floors[size_t (pointed)].storey;
        if (ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
            if (!io.KeyShift || run.Empty ())
                storage->SetInt (anchorId, storey);
            const int anchor = storage->GetInt (anchorId, storey);
            run = { (std::min) (anchor, storey), (std::max) (anchor, storey) };
        }
        else if (active && ImGui::IsMouseDragging (ImGuiMouseButton_Left, 0.0f)) {
            const int anchor = storage->GetInt (anchorId, storey);
            run = { (std::min) (anchor, storey), (std::max) (anchor, storey) };
        }
        // ⚠️ THE RIGHT CLICK IS THE SECTION'S, NOT THE HUD'S MENU (hudshell::ClaimRightClick).
        if (hovered &&
            (ImGui::IsMouseClicked (ImGuiMouseButton_Right) || ImGui::IsMouseReleased (ImGuiMouseButton_Right))) {
            hudshell::ClaimRightClick ();
            if (ImGui::IsMouseReleased (ImGuiMouseButton_Right)) {
                if (!run.Has (storey)) {
                    run = { storey, storey };
                    storage->SetInt (anchorId, storey);
                }
                ImGui::OpenPopup (kMenu);
            }
        }
    }

    // ---- the floors ---------------------------------------------------------------------------
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    const float bars = width - labels;
    const uint32_t empty = hudshell::WithAlpha (look.textRgba, 0.10f);
    const ImU32 muted = hudshell::Packed (hudshell::WithAlpha (look.textRgba, 0.65f));
    const float rounding = std::floor (2.0f * scale);
    for (size_t i = 0; i < count; ++i) {
        const Floor& floor = section.floors[i];
        const float y0 = std::floor (origin.y + float (count - 1 - i) * (row + gap)), y1 = y0 + row;
        draw->AddText (ImVec2 (origin.x, std::floor (y0 + (row - em) * 0.5f)), muted, floor.label.c_str ());
        const float share = section.widestM2 > 0.0 ? float (floor.areaM2 / section.widestM2) : 1.0f;
        const float w = std::floor (bars * (std::max) (kNarrowest, (std::min) (share, 1.0f)));
        const float x0 = std::floor (origin.x + labels + (bars - w) * 0.5f);
        float x = x0;
        uint32_t ink = look.textRgba;
        for (size_t p = 0; p < floor.parts.size (); ++p) {
            const Part& part = floor.parts[p];
            const float pw =
                floor.areaM2 > 0.0 ? w * float (part.areaM2 / floor.areaM2) : w / float (floor.parts.size ());
            const float right = p + 1 == floor.parts.size () ? x0 + w : std::floor (x + pw) - 1.0f;
            const uint32_t fill = (part.rgba & 0xFFu) != 0 ? part.rgba : empty;
            draw->AddRectFilled (ImVec2 (x, y0), ImVec2 (right, y1), hudshell::Packed (fill), rounding);
            if ((part.rgba & 0xFFu) != 0)
                ink = hudshell::Contrast (part.rgba);
            x = right + 1.0f;
        }
        if (int (i) == pointed)
            draw->AddRectFilled (ImVec2 (x0, y0), ImVec2 (x0 + w, y1),
                                 hudshell::Packed (hudshell::WithAlpha (look.accentRgba, 0.22f)), rounding);
        if (run.Has (floor.storey))
            draw->AddRect (ImVec2 (x0 - 1.0f, y0 - 1.0f), ImVec2 (x0 + w + 1.0f, y1 + 1.0f),
                           hudshell::Packed (look.accentRgba), rounding, 0, (std::max) (1.5f, 2.0f * scale));
        // What the floor is, inside it where it fits.
        const std::string value = FloorValue (section, floor);
        const ImVec2 size = ImGui::CalcTextSize (value.c_str ());
        if (!value.empty () && size.x + em * 0.5f < w)
            draw->AddText (ImVec2 (std::floor (x0 + (w - size.x) * 0.5f), std::floor (y0 + (row - em) * 0.5f)),
                           hudshell::Packed (ink), value.c_str ());
    }
    // The floor pointed at, said beside it.
    if (pointed >= 0 && !ImGui::IsPopupOpen (kMenu)) {
        const Floor& floor = section.floors[size_t (pointed)];
        const float y0 = std::floor (origin.y + float (count - 1 - size_t (pointed)) * (row + gap));
        std::string tip = floor.label + "  " + Area (floor.areaM2);
        const std::string value = FloorValue (section, floor);
        if (!value.empty ())
            tip += "\n" + section.keyLabel + ": " + value;
        hudshell::TipBeside (ImVec2 (origin.x, y0), ImVec2 (origin.x + width, y0 + row), hudshell::TipSide::Left, tip);
    }

    // ---- what is picked, and the values to give it --------------------------------------------
    double picked = 0.0;
    for (const Floor& floor : section.floors)
        if (run.Has (floor.storey))
            picked += floor.areaM2;
    ImGui::PushStyleColor (ImGuiCol_Text, hudshell::Colour (hudshell::WithAlpha (look.textRgba, 0.65f)));
    if (run.Empty ())
        ImGui::TextWrapped ("Press a floor; shift-press or drag for several; right-click to assign %s",
                            section.keyLabel.empty () ? "a value" : section.keyLabel.c_str ());
    else
        ImGui::TextWrapped ("%s picked, %s", RunText (run).c_str (), Area (picked).c_str ());
    ImGui::PopStyleColor ();
    if (ImGui::BeginPopup (kMenu)) {
        ImGui::TextDisabled ("%s: %s", RunText (run).c_str (), section.keyLabel.c_str ());
        ImGui::Separator ();
        for (const hudmeta::Option& option : section.options)
            if (hudmeta::OptionRow (option, false)) {
                for (hudmeta::Edit& edit : RunEdits (section, run, option.value, false))
                    edits.push_back (std::move (edit));
                ImGui::CloseCurrentPopup ();
            }
        ImGui::Separator ();
        if (ImGui::Selectable ("Clear")) {
            for (hudmeta::Edit& edit : RunEdits (section, run, std::string (), true))
                edits.push_back (std::move (edit));
            ImGui::CloseCurrentPopup ();
        }
        if (ImGui::Selectable ("Pick every floor"))
            run = { section.floors.front ().storey, section.floors.back ().storey };
        ImGui::EndPopup ();
    }
    if (!section.note.empty ()) {
        ImGui::PushStyleColor (ImGuiCol_Text, hudshell::Colour (0xD64545FFu));
        ImGui::TextWrapped ("%s", section.note.c_str ());
        ImGui::PopStyleColor ();
    }
    return edits;
}

} // namespace hudsection
} // namespace archviz
} // namespace geomsrv
