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
                            ImGuiSliderFlags_AlwaysClamp);
    else
        ImGui::DragFloat ("##value", held, field.step > 0.0 ? float (field.step) : 0.1f, 0.0f, 0.0f, format.c_str ());
    storage->SetBool (activeId, ImGui::IsItemActive ());
    *held = float (Snapped (field, *held));
    if (!ImGui::IsItemDeactivatedAfterEdit ())
        return;
    Edit edit;
    edit.id = field.id;
    edit.kind = field.kind;
    edit.type = field.type;
    edit.number = double (*held) / (field.scale != 0.0 ? field.scale : 1.0);
    edit.label = field.label;
    edits.push_back (std::move (edit));
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
                NumberControl (field, edits);
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
    return edits;
}

bool Apply (meta::EntityMetadata& entity, const Edit& edit, const meta::ProjectSchema& schema, int64_t nowMs,
            std::string& error)
{
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
        if (edit.domain.empty ())
            meta::RemoveProperty (entity, edit.id);
        else
            meta::ClearRange (entity, edit.domain, edit.from, edit.to, edit.id);
        return true;
    }
    if (edit.action == Edit::Action::AskText) {
        error = edit.id + ": the text is the dialog's to answer first";
        return false;
    }
    const meta::PropertyDefinition* definition = schema.Find (edit.id);
    const meta::ValueType type = definition != nullptr ? definition->type : edit.type;
    meta::Property property;
    property.key = edit.id;
    if (type == meta::ValueType::Bool)
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

} // namespace hudmeta
} // namespace archviz
} // namespace geomsrv
