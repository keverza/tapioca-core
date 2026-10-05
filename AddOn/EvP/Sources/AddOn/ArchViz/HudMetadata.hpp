#ifndef EVP_ARCHVIZ_HUDMETADATA_HPP
#define EVP_ARCHVIZ_HUDMETADATA_HPP

// ArchViz/HudMetadata -- the Selection page's Tapioca metadata (Metadata/TapiocaMetadata.hpp):
// the fields the project's schema defines for what is selected, drawn as the HUD's own
// controls, and what the user changes said as edits for the owner to write.
//
// ⚠️ GENERATED FROM THE SCHEMA, NOT WRITTEN PER WORKFLOW (the user, 2026-10-03: generic
// primitives from which workflows define schemas). A definition the project adds gets its
// control with no code here: an enumeration or a classification system is a dropdown, a
// bounded number a slider (a drag without bounds), a flag or an offered tag a toggle. Free
// text is the one thing ImGui is not asked to edit: its button asks the owner for a native
// dialog (the user: "a small native dialog if needed, but most options ... dropdown pickers,
// number sliders, toggles").
//
// ⚠️ ONE VALUE FOR EVERY SELECTED ELEMENT: a field shows the value they share, or "mixed"; an
// edit is the value they all take. A slider's edit is said once, when it is let go -- one
// undo step per change, not one per frame of the drag.
//
// Pure: Dear ImGui and the metadata model, any thread that holds an ImGui context; nothing
// here reads or writes the project. tests/cpp builds it.

#include "ArchViz/OverlayLayers.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace hudmeta {
std::string NumberText (double number);

enum class FieldKind : uint8_t { Choice, Toggle, Number, Text, Fixed, Heights };

struct Option {
    std::string value;
    std::string label;
    uint32_t rgba = 0; // its colour, 0xRRGGBBAA; alpha 0: none
};

// What a field's id starts with when it is not a property's key.
constexpr char kClassPrefix[] = "class:";
constexpr char kTagPrefix[] = "tag:";

struct Field {
    std::string id; // the property's key, kClassPrefix + a system, kTagPrefix + a tag
    std::string label;
    std::string group; // the heading it is listed under
    FieldKind kind = FieldKind::Fixed;
    metadata::ValueType type = metadata::ValueType::String; // a property's
    std::vector<Option> options;                            // Choice
    // Number: the value and its bounds as shown -- `scale` times the stored one -- in `unit`.
    double number = 0.0;
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;
    double scale = 1.0;
    bool integer = false;
    std::string unit;
    std::string text; // Choice's value, Text's, Fixed's as it reads
    bool on = false;  // Toggle
    bool set = false; // a selected element holds a value
    bool mixed = false;
    std::string note; // the definition's description: its tip
    std::vector<double> numbers;
};

struct Page {
    bool known = false;    // read for the selection as it stands
    uint32_t elements = 0; // how many the fields were read from
    uint32_t selected = 0; // how many an edit is written to
    std::vector<Field> fields;
    std::string note;    // what could not be read
    std::string element; // captured target, for defined slabs even when unselected
};

struct Edit {
    enum class Action : uint8_t { Set, Clear, AskText, AskNumber };
    std::string id;
    Action action = Action::Set;
    FieldKind kind = FieldKind::Fixed;
    metadata::ValueType type = metadata::ValueType::String;
    std::string text; // a Choice's value, a Text's; AskText: what it holds now
    double number = 0.0;
    bool on = false;
    std::string label; // the field's, for the dialog and the undo step's name
    // ⚠️ OVER A PART OF THE ELEMENT when `domain` is given: positions `from` to `to` of it -- floors
    // 3 to 7 -- assigned the value (metadata::AssignRange) or cleared of it (ClearRange).
    std::string domain;
    double from = 0.0;
    double to = 0.0;
    // Only this element's, when given: a range clipped to one element's own floors.
    std::string element;
    int listIndex = -1; // one floorHeight array entry; -1 replaces the complete array
    double min = 0, max = 0;
};

// The fields of `entities` -- the selected elements read -- under `schema`: its definitions that
// apply to their classes, in its UI groups' order and then by group; one per classification
// system; one per tag it offers. `selected` is how many an edit goes to.
Page Fields (const metadata::ProjectSchema& schema, const std::vector<metadata::EntityMetadata>& entities,
             uint32_t selected);
// Selection's deliberately small slab editor; legacy massing.height is read-only
// fallback until the user authors floorHeight. Unrelated metadata is never erased.
Page BuildingSlabFields (const metadata::ProjectSchema& schema, std::vector<metadata::EntityMetadata> entities,
                         uint32_t selected);

// An option of a dropdown or a menu: its colour's swatch, its label. True when chosen.
bool OptionRow (const Option& option, bool chosen);

// The page drawn in `look`; what the user changed in this frame.
std::vector<Edit> Editor (const Page& page, const overlaylayers::Panel& look, float scale);

// `edit` laid on `entity` as the user's authored value at `nowMs`. False with `error` when the
// edit is not one the schema can take.
bool Apply (metadata::EntityMetadata& entity, const Edit& edit, const metadata::ProjectSchema& schema, int64_t nowMs,
            std::string& error);
bool ParseNumber (const std::string& text, double minimum, double maximum, double& value);
bool ParseHeights (const std::string& text, metadata::Value& value);

} // namespace hudmeta
} // namespace archviz
} // namespace geomsrv

#endif
