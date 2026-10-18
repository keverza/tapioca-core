#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <imgui.h>
#include <algorithm>
#include <charconv>
#include <cmath>

// Labels opened for typing: a click on a label of the view -- a path's height, grade,
// crossfall, width or radius, a kerb fillet's radius, a live dimension -- opens it on a pill
// with its value selected, so the first key typed replaces it; a caret stands after the
// text. Enter, or a click anywhere else, sets the value; Esc leaves it as it was. What cannot
// be set -- no number, or one the edit refuses -- keeps the label open with the reason.
namespace geomsrv::archviz::edithud::detail {

namespace {

// The number a label says: its first run of sign, digits and point ("R 3.00 m" is 3.00).
std::string Number (const std::string& text)
{
    const auto digit = [] (char c) { return (c >= '0' && c <= '9') || c == '.' || c == ','; };
    for (size_t i = 0; i < text.size (); ++i) {
        const bool sign = (text[i] == '-' || text[i] == '+') && i + 1 < text.size () && digit (text[i + 1]);
        if (!sign && !(text[i] >= '0' && text[i] <= '9'))
            continue;
        size_t j = i + 1;
        while (j < text.size () && digit (text[j]))
            ++j;
        return text.substr (i, j - i);
    }
    return {};
}

bool Inside (const editlayers::Box& b, ImVec2 p, float slack)
{
    const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
    const float x = (p.x - cx) * b.ux + (p.y - cy) * b.uy, y = -(p.x - cx) * b.uy + (p.y - cy) * b.ux;
    return std::abs (x) <= (b.x1 - b.x0) * 0.5f + slack && std::abs (y) <= (b.y1 - b.y0) * 0.5f + slack;
}

// Sets what was typed and closes the label: true. Text that is no number, or a value the edit
// refuses (a negative radius, a width the path cannot take), changes nothing: the label stays
// open on it, selected so the next key replaces it, and the message says why -- false. A label
// the document has moved past since it opened is stale: it closes unset -- false too.
bool Enter (Session& s)
{
    const auto refuse = [&s] (const std::string& why) {
        s.message = why;
        s.labelSelected = true;
        return false;
    };
    // The document moved on since the label opened (the panel, the menu): what it reads may
    // be another value now, or gone. It is not set, and it closes -- click it again.
    if (Doc (s).revision != s.labelRevision) {
        s.labelOpen = false;
        s.message = "The value changed while it was typed; click its label again.";
        return false;
    }
    double value = 0.0;
    if (!edit::ParseNumber (s.labelText, value))
        return refuse ("Type a plain number; Enter applies, Esc cancels.");
    const editlayers::MarkLabel t = s.labelTarget;
    if (t.kerbPath != edit::kNoId && value == t.kerb.radius) {
        s.labelOpen = false;
        s.message.clear ();
        return true;
    }
    edit::Operation op;
    if (t.kerbPath != edit::kNoId)
        op = [t, value] (edit::Document& d, std::string& e) {
            return edit::SetKerbRadius (d, t.kerbPath, t.kerb, value, e);
        };
    else if (t.value.ref.part == edit::Part::Range && t.value.quantity == edit::Quantity::Slope && value < 0.0) {
        // A crossfall typed negative falls to the other side (a crown has none: it is kept).
        const edit::Path* path = edit::FindPath (Doc (s), t.value.ref.entity);
        edit::Fall fall = edit::Fall::Crown;
        if (path != nullptr)
            for (const auto& r : path->sections)
                if (r.id == t.value.ref.item)
                    fall = edit::Flipped (r.fall);
        op = [t, value, fall] (edit::Document& d, std::string& e) {
            return edit::SetPathCrossfall (d, t.value.ref.entity, t.value.ref.item, -value, fall, e);
        };
    }
    else {
        // A grade is shown unsigned: what is typed keeps the way it falls, a minus turns it.
        double now = 0.0;
        if (t.value.ref.part == edit::Part::Break && t.value.quantity == edit::Quantity::Slope &&
            edit::ReadDimension (Doc (s), t.value, now) && now < 0.0)
            value = -value;
        op = edit::WriteDimension (t.value, value);
    }
    // Apply is false for a value that changes nothing too; only a refusal carries a reason.
    if (!s.edits.Apply (op) && !s.edits.Message ().empty ())
        return refuse (s.edits.Message ());
    s.labelOpen = false;
    s.message.clear ();
    return true;
}

} // namespace

bool LabelContains (const editlayers::Box& box, ImVec2 mouse)
{
    return Inside (box, mouse, 2.0f);
}

bool SameLabel (const editlayers::MarkLabel& a, const editlayers::MarkLabel& b)
{
    if (a.kerbPath != b.kerbPath)
        return false;
    if (a.kerbPath != edit::kNoId)
        return a.kerb.Same (b.kerb);
    return a.value.ref == b.value.ref && a.value.quantity == b.value.quantity;
}

bool LabelClick (Session& s, ImVec2 mouse, bool hovered)
{
    if (!hovered || !ImGui::IsMouseClicked (0) || s.drag != Drag::None)
        return false;
    for (auto it = s.placed.rbegin (); it != s.placed.rend (); ++it) {
        if (!Inside (it->box, mouse, 2.0f))
            continue;
        if (s.labelOpen && SameLabel (it->target, s.labelTarget)) {
            s.labelSelected = false; // a second click: the caret after the text, to add to it
            return true;
        }
        if (s.labelOpen && !Enter (s))
            return true; // what was typed is refused: its label stays open, and the click is spent
        s.labelOpen = true;
        s.labelSelected = true;
        s.labelTarget = it->target;
        s.labelRevision = Doc (s).revision;
        if (it->target.kerbPath != edit::kNoId) {
            // A fitted display is not the user's stored request. Edit that request with
            // round-trip plain-number precision, so opening/confirming it loses no intent.
            char text[2048];
            const auto result =
                std::to_chars (text, text + sizeof text, it->target.kerb.radius, std::chars_format::fixed);
            if (result.ec != std::errc {}) {
                s.labelOpen = false;
                s.message = "The requested radius could not be formatted for editing.";
                return true;
            }
            s.labelText.assign (text, result.ptr);
            const auto dot = s.labelText.find ('.');
            if (dot == std::string::npos)
                s.labelText += ".00";
            else if (s.labelText.size () - dot == 2)
                s.labelText += '0';
        }
        else
            s.labelText = Number (it->target.text);
        return true;
    }
    if (!s.labelOpen)
        return false;
    Enter (s); // a click elsewhere sets it, or is refused with it, and does nothing else
    return true;
}

bool LabelKeys (Session& s, const overlayhud::Input& v)
{
    if (!s.labelOpen)
        return false;
    for (const auto action : v.editActions)
        if (action == overlayhud::Input::EditAction::Cancel) {
            s.labelOpen = false;
            return true;
        }
    for (const auto& key : v.keys) {
        if (!key.down)
            continue;
        if (key.code == 0x1B) {
            s.labelOpen = false;
            return true;
        }
        if (key.code == 0x0D) {
            Enter (s);
            return true;
        }
        if (key.code == 0x08 || key.code == 0x2E) {
            if (s.labelSelected || key.code == 0x2E)
                s.labelText.clear ();
            else if (!s.labelText.empty ())
                s.labelText.pop_back ();
            s.labelSelected = false;
        }
    }
    for (const char16_t c : v.characters)
        if ((c >= u'0' && c <= u'9') || c == u'-' || c == u'+' || c == u'.' || c == u',') {
            if (s.labelSelected)
                s.labelText.clear (), s.labelSelected = false; // the first key replaces it
            s.labelText += char (c);
        }
    return true;
}

} // namespace geomsrv::archviz::edithud::detail
