// ArchViz/HudMetadata -- see the header.

#include "ArchViz/HudMetadata.hpp"

#include "ArchViz/HudShell.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue, the combo's own preview

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace geomsrv {
namespace archviz {
namespace hudmeta {
std::string NumberText (double number)
{
    char text[64] = {};
    std::snprintf (text, sizeof (text), "%.2f", number);
    return text;
}

namespace meta = metadata;
namespace layers = overlaylayers;

namespace {

constexpr uint32_t kRed = 0xD64545FFu;
constexpr char kNone[] = "-";
constexpr char kMixed[] = "mixed";

bool StartsWith (const std::string& text, const char* prefix)
{
    return text.compare (0, std::strlen (prefix), prefix) == 0;
}

// How a definition's unit is shown, and what a stored value is multiplied by to show it.
void UnitOf (const meta::ProjectSchema& schema, const meta::PropertyDefinition& definition, std::string& symbol,
             double& scale)
{
    symbol = definition.unit;
    scale = 1.0;
    for (const meta::UnitDefinition& unit : schema.units)
        if (unit.id == definition.unit) {
            symbol = unit.symbol;
            if (unit.toCanonical > 0.0)
                scale = 1.0 / unit.toCanonical;
            return;
        }
}

FieldKind KindOf (meta::ValueType type)
{
    if (type == meta::ValueType::Enum)
        return FieldKind::Choice;
    if (type == meta::ValueType::Bool)
        return FieldKind::Toggle;
    if (type == meta::ValueType::String)
        return FieldKind::Text;
    return meta::IsNumber (type) ? FieldKind::Number : FieldKind::Fixed;
}

// What the entities say of one thing: the first value, whether any has it, whether they differ.
template <typename T> struct Agreement {
    T first {};
    size_t holding = 0;
    bool differ = false;
    void Note (const T* value)
    {
        if (value == nullptr)
            return;
        if (holding++ == 0)
            first = *value;
        else if (!(*value == first))
            differ = true;
    }
    // Set and mixed for `count` entities.
    void Into (Field& field, size_t count) const
    {
        field.set = holding > 0;
        field.mixed = holding > 0 && (differ || holding < count);
    }
};

Field PropertyField (const meta::ProjectSchema& schema, const meta::PropertyDefinition& definition,
                     const std::string& group, const std::vector<meta::EntityMetadata>& entities)
{
    Field field;
    field.id = definition.key;
    field.label = definition.label.empty () ? definition.key : definition.label;
    field.group = group;
    field.type = definition.type;
    field.kind = KindOf (definition.type);
    field.note = definition.description;
    Agreement<meta::Value> agree;
    for (const meta::EntityMetadata& entity : entities) {
        const meta::Property* property = meta::FindProperty (entity, definition.key);
        agree.Note (property != nullptr ? &property->value : nullptr);
    }
    agree.Into (field, entities.size ());
    const meta::Value& value = agree.first;
    switch (field.kind) {
        case FieldKind::Choice:
            if (const meta::Enumeration* options = schema.FindEnumeration (definition.enumId))
                for (const meta::EnumOption& option : options->options)
                    field.options.push_back (
                        { option.value, option.label.empty () ? option.value : option.label, option.rgba });
            field.text = value.s;
            break;
        case FieldKind::Toggle:
            field.on = field.set && !field.mixed && value.b;
            break;
        case FieldKind::Number:
            UnitOf (schema, definition, field.unit, field.scale);
            field.integer = definition.type == meta::ValueType::Int;
            field.number = value.AsNumber () * field.scale;
            field.min = definition.min * field.scale;
            field.max = definition.max * field.scale;
            field.step = definition.step * field.scale;
            break;
        case FieldKind::Text:
            field.text = value.s;
            break;
        case FieldKind::Fixed:
            field.text = field.set ? meta::ToText (value) : std::string ();
            break;
    }
    return field;
}

// A line in the look's muted colour -- or `rgba` -- wrapped inside the panel.
void Note (const std::string& text, const layers::Panel& look, uint32_t rgba = 0)
{
    ImGui::PushStyleColor (ImGuiCol_Text,
                           hudshell::Colour ((rgba & 0xFFu) != 0 ? rgba : hudshell::WithAlpha (look.textRgba, 0.6f)));
    ImGui::TextWrapped ("%s", text.c_str ());
    ImGui::PopStyleColor ();
}

// A small square of an option's colour at `at`, a line `h` tall.
void Swatch (ImDrawList* draw, ImVec2 at, float h, uint32_t rgba)
{
    const float s = std::floor (h * 0.7f), y = std::floor (at.y + (h - s) * 0.5f);
    draw->AddRectFilled (ImVec2 (at.x, y), ImVec2 (at.x + s, y + s), hudshell::Packed (rgba), 2.0f);
}

} // namespace

bool OptionRow (const Option& option, bool chosen)
{
    const float h = ImGui::GetTextLineHeight ();
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const bool swatched = (option.rgba & 0xFFu) != 0;
    ImGui::PushID (option.value.c_str ());
    const bool pressed = ImGui::Selectable ("##option", chosen);
    ImGui::PopID ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float lead = swatched ? std::floor (h * 0.7f) + ImGui::GetStyle ().ItemInnerSpacing.x : 0.0f;
    if (swatched)
        Swatch (draw, at, h, option.rgba);
    draw->AddText (ImVec2 (at.x + lead, at.y), ImGui::GetColorU32 (ImGuiCol_Text), option.label.c_str ());
    return pressed;
}

namespace {

void ChoiceControl (const Field& field, std::vector<Edit>& edits)
{
    const Option* shown = nullptr;
    for (const Option& option : field.options)
        if (field.set && !field.mixed && option.value == field.text)
            shown = &option;
    const bool open = ImGui::BeginCombo ("##value", nullptr, ImGuiComboFlags_CustomPreview);
    if (ImGui::BeginComboPreview ()) {
        const float h = ImGui::GetTextLineHeight ();
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        float lead = 0.0f;
        if (shown != nullptr && (shown->rgba & 0xFFu) != 0) {
            Swatch (ImGui::GetWindowDrawList (), at, h, shown->rgba);
            lead = std::floor (h * 0.7f) + ImGui::GetStyle ().ItemInnerSpacing.x;
        }
        ImGui::SetCursorScreenPos (ImVec2 (at.x + lead, at.y));
        const std::string text = field.mixed        ? std::string (kMixed)
                                 : shown != nullptr ? shown->label
                                 : field.set        ? field.text // a value its enumeration no longer has
                                                    : std::string (kNone);
        ImGui::TextUnformatted (text.c_str ());
        ImGui::EndComboPreview ();
    }
    if (!open)
        return;
    for (const Option& option : field.options)
        if (OptionRow (option, &option == shown) && &option != shown) {
            Edit edit;
            edit.id = field.id;
            edit.kind = field.kind;
            edit.type = field.type;
            edit.text = option.value;
            edit.label = field.label;
            edits.push_back (std::move (edit));
        }
    if (field.set) {
        ImGui::Separator ();
        if (ImGui::Selectable ("Clear")) {
            Edit edit;
            edit.id = field.id;
            edit.action = Edit::Action::Clear;
            edit.kind = field.kind;
            edit.type = field.type;
            edit.label = field.label;
            edits.push_back (std::move (edit));
        }
    }
    ImGui::EndCombo ();
}

void ToggleControl (const Field& field, std::vector<Edit>& edits)
{
    bool on = field.on;
    if (field.mixed)
        ImGui::PushItemFlag (ImGuiItemFlags_MixedValue, true);
    const bool pressed = ImGui::Checkbox ("##value", &on);
    if (field.mixed)
        ImGui::PopItemFlag ();
    if (!pressed)
        return;
    Edit edit;
    edit.id = field.id;
    edit.kind = field.kind;
    edit.type = field.type;
    edit.on = on;
    edit.label = field.label;
    edits.push_back (std::move (edit));
}

// A number's text: its decimals by its step, its unit after it.
std::string NumberFormat (const Field& field)
{
    const int decimals = field.integer || field.step >= 1.0 ? 0 : field.step >= 0.1 ? 1 : 2;
    std::string unit;
    for (const char c : field.unit)
        unit += c == '%' ? std::string ("%%") : std::string (1, c);
    return "%." + std::to_string (decimals) + "f" + (unit.empty () ? std::string () : " " + unit);
}

double Snapped (const Field& field, double value)
{
    if (field.integer)
        value = std::round (value);
    else if (field.step > 0.0)
        value = std::round (value / field.step) * field.step;
    if (field.max > field.min)
        value = (std::min) ((std::max) (value, field.min), field.max);
    return value;
}

void NumberControl (const Field& field, std::vector<Edit>& edits)
{
    // ⚠️ THE DRAG HOLDS ITS OWN VALUE UNTIL IT IS LET GO: the page is the selection as last
    // read, and taken every frame it would put the slider back under the pointer.
    ImGuiStorage* const storage = ImGui::GetStateStorage ();
    const ImGuiID heldId = ImGui::GetID ("##held"), activeId = ImGui::GetID ("##active");
    float* const held = storage->GetFloatRef (heldId, float (field.number));
    const bool active = storage->GetBool (activeId, false);
    if (!active)
        *held = float (field.number);
    const std::string format = field.mixed && !active ? std::string (kMixed) : NumberFormat (field);
    if (field.max > field.min)
        ImGui::SliderFloat ("##value", held, float (field.min), float (field.max), format.c_str (),
                            ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
    else
        ImGui::DragFloat ("##value", held, field.step > 0.0 ? float (field.step) : 0.1f, 0.0f, 0.0f, format.c_str (),
                          ImGuiSliderFlags_NoInput);
    storage->SetBool (activeId, ImGui::IsItemActive ());
    *held = float (Snapped (field, *held));
    const bool changed = ImGui::IsItemDeactivatedAfterEdit ();
    ImGui::SameLine ();
    const bool ask = ImGui::SmallButton ("Set");
    if (changed || ask) {
        Edit edit;
        edit.id = field.id;
        edit.kind = field.kind;
        edit.type = field.type;
        const double scale = field.scale != 0.0 ? field.scale : 1.0;
        edit.number = double (*held) / scale;
        edit.label = field.label;
        edit.min = field.min / scale;
        edit.max = field.max / scale;
        if (ask) {
            edit.action = Edit::Action::AskNumber;
            edit.text = NumberText (edit.number);
        }
        edits.push_back (std::move (edit));
    }
}

void HeightsControl (const Field& field, std::vector<Edit>& edits)
{
    for (size_t i = 0; i < field.numbers.size (); ++i) {
        ImGui::PushID (int (i));
        ImGui::Text ("Floor %d", int (i));
        Field entry = field;
        entry.kind = FieldKind::Number;
        entry.number = field.numbers[i];
        entry.min = 2.2;
        entry.max = 6;
        entry.step = 0.01;
        entry.unit = "m";
        entry.label = "Floor " + std::to_string (i) + " height (2.2 - 6.0 m)";
        ImGui::SetNextItemWidth ((std::max) (40.0f, ImGui::GetContentRegionAvail ().x - 45));
        const auto first = edits.size ();
        NumberControl (entry, edits);
        for (size_t j = first; j < edits.size (); ++j)
            edits[j].listIndex = int (i);
        ImGui::PopID ();
    }
    ImGui::TextDisabled ("Last height repeats for higher floors.");
    auto numbers = field.numbers;
    bool changed = false;
    if (numbers.size () < 512 && ImGui::SmallButton ("Add height")) {
        numbers.push_back (numbers.empty () ? 3 : numbers.back ());
        changed = true;
    }
    ImGui::SameLine ();
    if (numbers.size () > 1 && ImGui::SmallButton ("Remove last")) {
        numbers.pop_back ();
        changed = true;
    }
    ImGui::SameLine ();
    const bool ask = ImGui::SmallButton ("Set array");
    if (changed || ask) {
        Edit edit;
        edit.id = field.id;
        edit.type = meta::ValueType::List;
        edit.kind = FieldKind::Heights;
        edit.label = "Floor heights, comma separated (2.2 - 6.0 m)";
        for (double value : numbers) {
            if (!edit.text.empty ())
                edit.text += ", ";
            if (ask)
                edit.text += NumberText (value);
            else {
                // Adding/removing a height must not round untouched stored entries.
                char text[64] = {};
                std::snprintf (text, sizeof (text), "%.17g", value);
                edit.text += text;
            }
        }
        edit.action = ask ? Edit::Action::AskText : Edit::Action::Set;
        edits.push_back (std::move (edit));
    }
}

void TextControl (const Field& field, std::vector<Edit>& edits)
{
    ImGui::TextUnformatted (field.mixed ? kMixed : field.set ? field.text.c_str () : kNone);
    if (!ImGui::SmallButton ("Edit..."))
        return;
    Edit edit;
    edit.id = field.id;
    edit.action = Edit::Action::AskText;
    edit.kind = field.kind;
    edit.type = field.type;
    edit.text = field.mixed ? std::string () : field.text;
    edit.label = field.label;
    edits.push_back (std::move (edit));
}

} // namespace

Page Fields (const meta::ProjectSchema& schema, const std::vector<meta::EntityMetadata>& entities, uint32_t selected)
{
    Page page;
    page.known = true;
    page.elements = uint32_t (entities.size ());
    page.selected = selected;
    if (entities.empty ())
        return page;

    // The definitions that apply to any of their classes, under the schema's UI groups first,
    // then each under its own group -- every group once, in the order it was first met.
    std::vector<std::string> classes;
    for (const meta::EntityMetadata& entity : entities)
        for (const meta::Classification& classification : entity.classifications)
            classes.push_back (classification.value);
    const std::vector<const meta::PropertyDefinition*> applicable = schema.ApplicableTo (classes);
    std::vector<std::string> order;
    std::map<std::string, std::vector<Field>> groups;
    std::set<std::string> placed;
    const auto add = [&] (const std::string& group, const meta::PropertyDefinition& definition) {
        if (!placed.insert (definition.key).second)
            return;
        if (groups.find (group) == groups.end ())
            order.push_back (group);
        groups[group].push_back (PropertyField (schema, definition, group, entities));
    };
    for (const meta::UiSchema& ui : schema.ui)
        for (const std::string& key : ui.keys)
            for (const meta::PropertyDefinition* definition : applicable)
                if (definition->key == key)
                    add (ui.title.empty () ? ui.id : ui.title, *definition);
    for (const meta::PropertyDefinition* definition : applicable)
        add (definition->group.empty () ? std::string ("Other") : definition->group, *definition);
    for (const std::string& group : order)
        for (Field& field : groups[group])
            page.fields.push_back (std::move (field));

    // What each element is, per system.
    for (const meta::ClassificationSystem& system : schema.classifications) {
        Field field;
        field.id = kClassPrefix + system.id;
        field.label = system.label.empty () ? system.id : system.label;
        field.group = "Classification";
        field.kind = FieldKind::Choice;
        for (const meta::EnumOption& value : system.values)
            field.options.push_back ({ value.value, value.label.empty () ? value.value : value.label, value.rgba });
        Agreement<std::string> agree;
        for (const meta::EntityMetadata& entity : entities) {
            const std::string value = meta::ClassificationIn (entity, system.id);
            agree.Note (value.empty () ? nullptr : &value);
        }
        agree.Into (field, entities.size ());
        field.text = agree.first;
        page.fields.push_back (std::move (field));
    }

    // The tags the schema offers: on when every element has it.
    for (const std::string& tag : schema.tags) {
        Field field;
        field.id = kTagPrefix + tag;
        field.label = tag;
        field.group = "Tags";
        field.kind = FieldKind::Toggle;
        size_t holding = 0;
        for (const meta::EntityMetadata& entity : entities)
            holding += meta::HasTag (entity, tag) ? 1u : 0u;
        field.set = holding > 0;
        field.on = holding == entities.size ();
        field.mixed = holding > 0 && holding < entities.size ();
        page.fields.push_back (std::move (field));
    }
    return page;
}

std::vector<Edit> Editor (const Page& page, const layers::Panel& look, float scale)
{
    (void) scale;
    std::vector<Edit> edits;
    if (!page.known) {
        Note ("The Tapioca metadata has not been read yet", look);
        return edits;
    }
    if (!page.note.empty ())
        Note (page.note, look, kRed);
    if (page.elements == 0)
        return edits;
    std::string group;
    bool table = false;
    for (const Field& field : page.fields) {
        if (!table || field.group != group) {
            if (table)
                ImGui::EndTable ();
            group = field.group;
            ImGui::SeparatorText (group.c_str ());
            table = ImGui::BeginTable (("##meta." + group).c_str (), 2,
                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
            if (table) {
                ImGui::TableSetupColumn ("##label", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthStretch);
            }
        }
        if (!table)
            continue;
        ImGui::PushID (field.id.c_str ());
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted (field.label.c_str ());
        hudshell::Tip (field.note, hudshell::TipSide::Left);
        ImGui::TableSetColumnIndex (1);
        ImGui::SetNextItemWidth (-FLT_MIN);
        switch (field.kind) {
            case FieldKind::Choice:
                ChoiceControl (field, edits);
                break;
            case FieldKind::Toggle:
                ToggleControl (field, edits);
                break;
            case FieldKind::Number:
                ImGui::SetNextItemWidth ((std::max) (40.0f, ImGui::GetContentRegionAvail ().x - 45));
                NumberControl (field, edits);
                break;
            case FieldKind::Heights:
                HeightsControl (field, edits);
                break;
            case FieldKind::Text:
                TextControl (field, edits);
                break;
            case FieldKind::Fixed:
                Note (field.set ? field.text : std::string (kNone), look);
                break;
        }
        ImGui::PopID ();
    }
    if (table)
        ImGui::EndTable ();
    if (page.selected > page.elements)
        Note ("Read from the first " + std::to_string (page.elements) + "; a change is written to all " +
                  std::to_string (page.selected) + " selected",
              look);
    for (auto& edit : edits)
        edit.element = page.element;
    return edits;
}

Page BuildingSlabFields (const meta::ProjectSchema& schema, std::vector<meta::EntityMetadata> entities,
                         uint32_t selected)
{
    for (auto& entity : entities)
        if (!meta::FindProperty (entity, "massing.floorHeight"))
            if (const auto* legacy = meta::FindProperty (entity, "massing.height")) {
                auto property = *legacy;
                property.key = "massing.floorHeight";
                entity.properties.push_back (std::move (property));
            }
    const auto fields = Fields (schema, entities, selected);
    Page page = fields;
    page.fields.clear ();
    for (const char* key : { "massing.buildingId", "massing.story", "massing.function", "massing.floorHeight" })
        for (const auto& candidate : fields.fields)
            if (candidate.id == key) {
                Field field = candidate;
                field.group = "BUILDING SOURCE";
                field.label = key;
                if (field.id == "massing.floorHeight" && !field.set)
                    field.number = 3;
                if (field.id == "massing.floorHeight") {
                    field.kind = FieldKind::Heights;
                    field.type = meta::ValueType::List;
                    field.numbers = { field.number > 0 ? field.number : 3 };
                    if (!entities.empty ())
                        if (const auto* value = meta::FindProperty (entities.front (), field.id.c_str ()); value) {
                            if (value->value.type == meta::ValueType::List) {
                                field.numbers.clear ();
                                for (const auto& height : value->value.list)
                                    field.numbers.push_back (height.AsNumber ());
                            }
                            else if (meta::IsNumber (value->value.type)) {
                                field.number = value->value.AsNumber ();
                                field.numbers = { field.number };
                            }
                        }
                    if (field.mixed) {
                        field.kind = FieldKind::Text;
                        field.text.clear ();
                    }
                    const bool archicad = std::all_of (entities.begin (), entities.end (), [] (const auto& entity) {
                        const auto* mode = meta::FindProperty (entity, "massing.heightMode");
                        return mode && mode->value.s == "archicad";
                    });
                    if (archicad) {
                        field.kind = FieldKind::Fixed;
                        field.set = true;
                        field.text = "Archicad stories";
                    }
                    else if (!field.mixed) {
                        field.text.clear ();
                        for (double height : field.numbers) {
                            if (!field.text.empty ())
                                field.text += ", ";
                            field.text += NumberText (height);
                        }
                        field.text += " m";
                    }
                    else
                        field.text = "mixed";
                    field.kind = FieldKind::Fixed;
                    field.set = true;
                    field.note = "Preview only. Edit heights in Story slice editor.";
                }
                if (field.id == "massing.story") {
                    field.kind = FieldKind::Fixed;
                    field.note = "Automatically counted from story slices.";
                }
                if (field.id == "massing.function") {
                    field.kind = FieldKind::Fixed;
                    field.set = true;
                    field.text = "Per story (Story slice editor)";
                }
                if (field.id == "massing.buildingId")
                    field.note = "One building ID identifies a building, which may have several stairwells. Edits "
                                 "apply only to viewport-selected slabs.";
                page.fields.push_back (std::move (field));
                break;
            }
    return page;
}

namespace {
// The building's floor designs travel with its stairwells: written, or taken off when none.
bool SetDesigns (meta::EntityMetadata& entity, const std::string& text, const meta::Provenance& provenance,
                 std::string& error)
{
    if (text.empty ()) {
        meta::RemoveProperty (entity, "massing.floorDesigns");
        return true;
    }
    if (text.size () > 256 * 1024) {
        error = "Floor designs exceed their size budget.";
        return false;
    }
    meta::Property designs;
    designs.key = "massing.floorDesigns";
    designs.value = meta::Value::Text (text);
    designs.state = meta::State::Authored;
    designs.provenance = provenance;
    meta::SetProperty (entity, std::move (designs));
    return true;
}
} // namespace

bool Apply (meta::EntityMetadata& entity, const Edit& edit, const meta::ProjectSchema& schema, int64_t nowMs,
            std::string& error)
{
    if (edit.id == "massing.story" || (edit.id == "massing.function" && edit.domain.empty ())) {
        error = edit.id == "massing.story" ? "Story count is calculated from slices."
                                           : "Assign function to picked stories in Story slice editor.";
        return false;
    }
    meta::Provenance provenance;
    provenance.source = meta::Source::User;
    provenance.sourceId = "hud";
    provenance.timestampMs = nowMs;
    if (StartsWith (edit.id, kClassPrefix)) {
        const std::string system = edit.id.substr (std::strlen (kClassPrefix));
        if (edit.action == Edit::Action::Clear) {
            meta::Unclassify (entity, system);
            return true;
        }
        if (edit.text.empty ()) {
            error = "no value for " + system;
            return false;
        }
        meta::Classify (entity, system, edit.text, provenance);
        return true;
    }
    if (StartsWith (edit.id, kTagPrefix)) {
        const std::string tag = edit.id.substr (std::strlen (kTagPrefix));
        if (edit.action == Edit::Action::Set && edit.on)
            meta::AddTag (entity, tag);
        else
            meta::RemoveTag (entity, tag);
        return true;
    }
    if (edit.action == Edit::Action::Clear) {
        if (edit.domain.empty ()) {
            meta::RemoveProperty (entity, edit.id);
            if (edit.id == "massing.stairwellLocations") {
                meta::RemoveProperty (entity, "massing.stairwellShapes");
                return SetDesigns (entity, edit.text, provenance, error);
            }
        }
        else
            meta::ClearRange (entity, edit.domain, edit.from, edit.to, edit.id);
        return true;
    }
    if (edit.action == Edit::Action::AskText || edit.action == Edit::Action::AskNumber) {
        error = edit.id + ": the text is the dialog's to answer first";
        return false;
    }
    const meta::PropertyDefinition* definition = schema.Find (edit.id);
    const meta::ValueType type = definition != nullptr ? definition->type : edit.type;
    meta::Property property;
    property.key = edit.id;
    if (edit.id == "massing.floorHeight") {
        if (edit.listIndex >= 0) {
            const auto* held = meta::FindProperty (entity, edit.id);
            property.value.type = meta::ValueType::List;
            property.value.elementType = meta::ValueType::Length;
            if (held && held->value.type == meta::ValueType::List)
                property.value = held->value;
            else {
                const auto* legacy = held ? held : meta::FindProperty (entity, "massing.height");
                property.value.list = { meta::Value::Number (legacy ? legacy->value.AsNumber () : 3,
                                                             meta::ValueType::Length) };
            }
            if (size_t (edit.listIndex) >= property.value.list.size () || !std::isfinite (edit.number) ||
                edit.number < 2.2 || edit.number > 6) {
                error = "Floor height entry changed or is outside 2.2 - 6.0 m.";
                return false;
            }
            property.value.list[size_t (edit.listIndex)] = meta::Value::Number (edit.number, meta::ValueType::Length);
        }
        else if (!ParseHeights (edit.text, property.value)) {
            error = "Use comma-separated floor heights from 2.2 to 6.0 m.";
            return false;
        }
    }
    else if (edit.id == "massing.stairwellLocations") {
        if (type != meta::ValueType::List) {
            error = "The project's stairwell definition is not a location list.";
            return false;
        }
        if (edit.numbers.empty () || edit.numbers.size () % 2 || edit.numbers.size () > 64 ||
            std::any_of (edit.numbers.begin (), edit.numbers.end (),
                         [] (double value) { return !std::isfinite (value) || std::abs (value) > 1e9; })) {
            error = "Stairwell locations need 1..32 finite XY pairs in project metres.";
            return false;
        }
        if (!edit.shapes.empty () && (edit.shapes.size () != edit.numbers.size () ||
                                      std::any_of (edit.shapes.begin (), edit.shapes.end (), [] (double value) {
                                          return !std::isfinite (value) || value < 2.0 || value > 12.0;
                                      }))) {
            error = "Stairwell sizes need one 2-12 m width/depth pair per location.";
            return false;
        }
        property.value.type = meta::ValueType::List;
        property.value.elementType = meta::ValueType::Length;
        for (double value : edit.numbers)
            property.value.list.push_back (meta::Value::Number (value, meta::ValueType::Length));
        // Sizes travel with the locations; a write without them restores the 4.5 x 4.2 m default.
        if (edit.shapes.empty ())
            meta::RemoveProperty (entity, "massing.stairwellShapes");
        else {
            meta::Property shapes;
            shapes.key = "massing.stairwellShapes";
            shapes.value.type = meta::ValueType::List;
            shapes.value.elementType = meta::ValueType::Length;
            for (double value : edit.shapes)
                shapes.value.list.push_back (meta::Value::Number (value, meta::ValueType::Length));
            shapes.state = meta::State::Authored;
            shapes.provenance = provenance;
            meta::SetProperty (entity, std::move (shapes));
        }
        if (!SetDesigns (entity, edit.text, provenance, error))
            return false;
    }
    else if (type == meta::ValueType::Bool)
        property.value = meta::Value::Boolean (edit.on);
    else if (type == meta::ValueType::Int)
        property.value = meta::Value::Integer (int64_t (std::llround (edit.number)));
    else if (type == meta::ValueType::Enum)
        property.value = meta::Value::Option (edit.text);
    else if (type == meta::ValueType::String)
        property.value = meta::Value::Text (edit.text);
    else if (meta::IsNumber (type))
        property.value = meta::Value::Number (edit.number, type);
    else {
        error = edit.id + " is not edited on the HUD";
        return false;
    }
    property.state = meta::State::Authored;
    property.provenance = provenance;
    if (!edit.domain.empty ()) {
        if (edit.to < edit.from) {
            error = edit.id + ": a range ending before it starts";
            return false;
        }
        meta::RangeAssignment range;
        range.domain = edit.domain;
        range.from = edit.from;
        range.to = edit.to;
        range.provenance = provenance;
        range.properties.push_back (std::move (property));
        meta::AssignRange (entity, range);
        return true;
    }
    meta::SetProperty (entity, std::move (property));
    return true;
}

bool ParseNumber (const std::string& text, double minimum, double maximum, double& value)
{
    try {
        size_t end = 0;
        const double number = std::stod (text, &end);
        if (text.find_first_not_of (" \t\r\n", end) != std::string::npos || !std::isfinite (number) ||
            (maximum > minimum && (number < minimum || number > maximum)))
            return false;
        value = number;
        return true;
    }
    catch (...) {
        return false;
    }
}

bool ParseHeights (const std::string& text, meta::Value& value)
{
    meta::Value parsed;
    parsed.type = meta::ValueType::List;
    parsed.elementType = meta::ValueType::Length;
    size_t start = 0;
    for (;;) {
        const size_t end = text.find (',', start);
        double height = 0;
        if (parsed.list.size () >= 512 || !ParseNumber (text.substr (start, end - start), 2.2, 6, height))
            return false;
        parsed.list.push_back (meta::Value::Number (height, meta::ValueType::Length));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    value = std::move (parsed);
    return true;
}

} // namespace hudmeta
} // namespace archviz
} // namespace geomsrv
