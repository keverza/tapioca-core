#include "InputModel.hpp"

#include "NodeGraph/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace evp {

namespace {

namespace json = evp::nodegraph::json;

// Reads a member as a string, leaving `out` alone when it is absent or of
// another kind. Absence is how the schema expresses an optional value, so it is
// not an error here.
void ReadString (const json::JsonValue& object, const char* key, std::string& out)
{
    const json::JsonValue* member = object.Find (key);
    if (member == nullptr)
        return;
    std::string value;
    if (member->AsString (value))
        out = std::move (value);
}

void ReadBool (const json::JsonValue& object, const char* key, bool& out)
{
    const json::JsonValue* member = object.Find (key);
    if (member == nullptr)
        return;
    bool value = false;
    if (member->AsBool (value))
        out = value;
}

void ReadInt (const json::JsonValue& object, const char* key, int& out)
{
    const json::JsonValue* member = object.Find (key);
    if (member == nullptr)
        return;
    int64_t value = 0;
    if (member->AsInteger (value)) {
        out = (int) value;
        return;
    }
    // The schema writes order with no fraction, but a producer that wrote 3.0
    // meant 3 and refusing it would drop a control over a formatting choice.
    double asDouble = 0.0;
    if (member->AsDouble (asDouble))
        out = (int) asDouble;
}

// A bound is present only when it parsed as a number. `has` is set alongside so
// that "absent" and "zero" stay different things -- a minimum of 0 is a real
// constraint and the commonest one there is.
void ReadBound (const json::JsonValue& object, const char* key, bool& has, double& out)
{
    const json::JsonValue* member = object.Find (key);
    if (member == nullptr)
        return;
    double value = 0.0;
    if (member->AsDouble (value)) {
        has = true;
        out = value;
    }
}

bool HasExponent (const std::string& text)
{
    return text.find ('e') != std::string::npos || text.find ('E') != std::string::npos;
}

// Parses a number the way the panel's text field produces one, and REFUSES
// anything with a tail. strtod happily reads "3abc" as 3 and stops; a control
// that accepted that would send 3 for a value the user can plainly see is wrong.
bool ParseNumber (const std::string& text, double& out)
{
    if (text.empty ())
        return false;

    const char* begin = text.c_str ();
    char* end = nullptr;
    const double value = std::strtod (begin, &end);
    if (end == begin)
        return false;

    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;

    // NaN and infinity parse but cannot be solved with, and a definition that
    // received one would fail somewhere far from here.
    if (!std::isfinite (value))
        return false;

    out = value;
    return true;
}

std::string Trim (const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size ();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return text.substr (begin, end - begin);
}

char LowerAscii (char value)
{
    return (value >= 'A' && value <= 'Z') ? (char) (value + 32) : value;
}

// Ordinal, ignoring ASCII case: the same ordering .NET's
// StringComparison.OrdinalIgnoreCase gives for the group names a schema carries.
int CompareIgnoringCase (const std::string& left, const std::string& right)
{
    const size_t shared = left.size () < right.size () ? left.size () : right.size ();
    for (size_t index = 0; index < shared; ++index) {
        const char a = LowerAscii (left[index]);
        const char b = LowerAscii (right[index]);
        if (a != b)
            return a < b ? -1 : 1;
    }
    if (left.size () == right.size ())
        return 0;
    return left.size () < right.size () ? -1 : 1;
}

bool EqualsIgnoringCase (const std::string& left, const std::string& right)
{
    if (left.size () != right.size ())
        return false;
    for (size_t index = 0; index < left.size (); ++index) {
        const char a = (left[index] >= 'A' && left[index] <= 'Z') ? (char) (left[index] + 32) : left[index];
        const char b = (right[index] >= 'A' && right[index] <= 'Z') ? (char) (right[index] + 32) : right[index];
        if (a != b)
            return false;
    }
    return true;
}

CoerceResult Refuse (const InputControl& control, const std::string& reason)
{
    CoerceResult result;
    result.ok = false;
    // The LABEL, never the id: the id is a wire detail and the label is the word
    // beside the control the user is looking at.
    result.error = "'" + (control.label.empty () ? control.id : control.label) + "': " + reason;
    return result;
}

CoerceResult Accept (std::string value)
{
    CoerceResult result;
    result.ok = true;
    result.value = std::move (value);
    return result;
}

} // namespace

// ⚠️ A SHORTER SPELLING THAT ROUND-TRIPS IS NOT AUTOMATICALLY A BETTER ONE, AND
// THIS IS WHERE THAT WAS GOT WRONG. Taking the first precision whose output
// reads back identically renders 40 as "4e+01": one significant digit, exactly
// equal on the way back, and unrecognisable as the number the author typed. It
// would have gone into the domain hint the user reads, into the value sent over
// the pipe, and into the input-snapshot hash -- so two panels showing "40" could
// hash differently depending on which one formatted it.
//
// So an exponent form is only accepted when the full-precision rendering needs
// one too, which is exactly the case where the number really is too large or too
// small to write out. Caught by WorkflowRows.TheDomainIsWrittenOutForNumericRowsOnly.
std::string FormatInputNumber (double value)
{
    char full[64];
    std::snprintf (full, sizeof (full), "%.17g", value);
    const std::string longest (full);
    const bool exponentAllowed = HasExponent (longest);

    for (int precision = 1; precision <= 17; ++precision) {
        char candidate[64];
        std::snprintf (candidate, sizeof (candidate), "%.*g", precision, value);
        const std::string text (candidate);
        if (!exponentAllowed && HasExponent (text))
            continue;
        if (std::strtod (candidate, nullptr) == value)
            return text;
    }
    return longest;
}

const char* DescribeInputKind (InputKind kind)
{
    switch (kind) {
        case InputKind::Number:
            return "number";
        case InputKind::Integer:
            return "integer";
        case InputKind::Boolean:
            return "boolean";
        case InputKind::Text:
            return "text";
        case InputKind::Enum:
            return "enum";
        case InputKind::Unsupported:
            return "unsupported";
    }
    return "unsupported";
}

InputKind InputKindFromName (const std::string& name)
{
    if (name == "number")
        return InputKind::Number;
    if (name == "integer")
        return InputKind::Integer;
    if (name == "boolean")
        return InputKind::Boolean;
    // The package writes "string"; the panel and this model call it text. Both
    // spellings are accepted because only one of them is ours to change.
    if (name == "string" || name == "text")
        return InputKind::Text;
    if (name == "enum")
        return InputKind::Enum;
    return InputKind::Unsupported;
}

InputModel ParseInputModel (const std::string& schemaJson)
{
    InputModel model;

    const json::ParseResult parsed = json::Parse (schemaJson);
    if (!parsed.ok) {
        model.errors.push_back ("The workflow schema could not be read: " + parsed.error + ".");
        return model;
    }

    const json::JsonObject* root = parsed.value.AsObject ();
    if (root == nullptr) {
        model.errors.push_back ("The workflow schema was not a JSON object.");
        return model;
    }

    ReadString (parsed.value, "workflowId", model.workflowId);
    ReadString (parsed.value, "name", model.name);
    ReadString (parsed.value, "description", model.description);

    // The schema's own errors come across first, so that a model which also
    // fails to yield controls still shows the reason the author can act on.
    const json::JsonValue* errors = parsed.value.Find ("errors");
    if (errors != nullptr) {
        if (const json::JsonArray* list = errors->AsArray ()) {
            for (const json::JsonValue& entry : *list) {
                std::string text;
                if (entry.AsString (text))
                    model.errors.push_back (std::move (text));
            }
        }
    }

    const json::JsonValue* inputs = parsed.value.Find ("inputs");
    if (inputs == nullptr) {
        model.errors.push_back ("The workflow schema declared no inputs array.");
        return model;
    }

    const json::JsonArray* list = inputs->AsArray ();
    if (list == nullptr) {
        model.errors.push_back ("The workflow schema's inputs were not an array.");
        return model;
    }

    int fallbackOrder = 0;
    for (const json::JsonValue& entry : *list) {
        if (entry.AsObject () == nullptr) {
            model.errors.push_back ("An entry in the workflow schema's inputs was not an object.");
            continue;
        }

        InputControl control;
        ReadString (entry, "id", control.id);
        ReadString (entry, "label", control.label);
        ReadString (entry, "group", control.group);
        ReadString (entry, "type", control.declaredType);
        ReadString (entry, "default", control.defaultValue);
        ReadBool (entry, "required", control.required);
        control.order = fallbackOrder;
        ReadInt (entry, "order", control.order);
        ReadBound (entry, "min", control.hasMinimum, control.minimum);
        ReadBound (entry, "max", control.hasMaximum, control.maximum);
        control.kind = InputKindFromName (control.declaredType);

        if (const json::JsonValue* choices = entry.Find ("choices")) {
            if (const json::JsonArray* rows = choices->AsArray ()) {
                for (const json::JsonValue& row : *rows) {
                    std::string text;
                    if (row.AsString (text))
                        control.choices.push_back (std::move (text));
                }
            }
        }

        if (control.id.empty ()) {
            // Dropped, and SAID so. A control with no id cannot be sent -- the
            // wire refuses an id-less value -- but a row silently missing from
            // the panel is indistinguishable from a definition that never had
            // it.
            model.errors.push_back ("An input" +
                                    (control.label.empty () ? std::string () : " labelled '" + control.label + "'") +
                                    " has no id, so the panel cannot bind a control to it.");
            ++fallbackOrder;
            continue;
        }

        if (control.label.empty ())
            control.label = control.id;

        model.controls.push_back (std::move (control));
        ++fallbackOrder;
    }

    // Group, then order, then the position the schema listed them in -- the same
    // rule the worker's discovery applied, restated here because the panel sorts
    // its own rows and two sorts that disagree would reorder the user's controls
    // every time the schema was refetched. std::stable_sort keeps the schema's
    // order as the final tiebreak without a third key.
    //
    // ⚠️ THE GROUP COMPARISON IS CASE-INSENSITIVE BECAUSE THE WORKER'S IS.
    // TapiocaInputSchema.Discover sorts groups with OrdinalIgnoreCase; an
    // ordinal comparison here would put "massing" and "Massing" in different
    // places from the schema that declared them, and the disagreement would only
    // show on a definition whose author capitalised a group inconsistently --
    // which is exactly the definition nobody tests with.
    std::stable_sort (model.controls.begin (), model.controls.end (),
                      [] (const InputControl& a, const InputControl& b) {
                          const int byGroup = CompareIgnoringCase (a.group, b.group);
                          if (byGroup != 0)
                              return byGroup < 0;
                          return a.order < b.order;
                      });

    model.parsed = true;
    return model;
}

CoerceResult Coerce (const InputControl& control, const std::string& text)
{
    const std::string trimmed = Trim (text);

    switch (control.kind) {
        case InputKind::Boolean: {
            if (EqualsIgnoringCase (trimmed, "true") || trimmed == "1")
                return Accept ("true");
            if (EqualsIgnoringCase (trimmed, "false") || trimmed == "0")
                return Accept ("false");
            return Refuse (control, "'" + trimmed + "' is not true or false.");
        }

        case InputKind::Integer: {
            double value = 0.0;
            if (!ParseNumber (trimmed, value))
                return Refuse (control, "'" + trimmed + "' is not a whole number.");
            // Rejected rather than rounded. Rounding would let a user type 2.6,
            // see it accepted, and get a solution computed from 3 with nothing
            // on screen saying so.
            if (value != std::floor (value))
                return Refuse (control, "'" + trimmed + "' is not a whole number.");
            if (control.hasMinimum && value < control.minimum)
                return Refuse (control, "must be at least " + FormatInputNumber (control.minimum) + ".");
            if (control.hasMaximum && value > control.maximum)
                return Refuse (control, "must be at most " + FormatInputNumber (control.maximum) + ".");
            return Accept (FormatInputNumber (value));
        }

        case InputKind::Number: {
            double value = 0.0;
            if (!ParseNumber (trimmed, value))
                return Refuse (control, "'" + trimmed + "' is not a number.");
            if (control.hasMinimum && value < control.minimum)
                return Refuse (control, "must be at least " + FormatInputNumber (control.minimum) + ".");
            if (control.hasMaximum && value > control.maximum)
                return Refuse (control, "must be at most " + FormatInputNumber (control.maximum) + ".");
            return Accept (FormatInputNumber (value));
        }

        case InputKind::Enum: {
            if (control.choices.empty ())
                return Refuse (control, "offers no choices, so there is nothing to send.");
            for (const std::string& choice : control.choices) {
                // Exact first, so a definition offering both "Wall" and "wall"
                // resolves to the one the user picked rather than to whichever
                // came first.
                if (choice == trimmed)
                    return Accept (choice);
            }
            for (const std::string& choice : control.choices) {
                if (EqualsIgnoringCase (choice, trimmed))
                    return Accept (choice);
            }
            return Refuse (control, "'" + trimmed + "' is not one of its choices.");
        }

        case InputKind::Text: {
            // NOT trimmed on the way out. Leading and trailing spaces are
            // content in a text input -- a label, a prefix, a separator -- and
            // the trim above exists only so the numeric and boolean parsers see
            // a clean string.
            if (control.required && Trim (text).empty ())
                return Refuse (control, "is required.");
            return Accept (text);
        }

        case InputKind::Unsupported:
            return Refuse (control,
                           "is of type '" + control.declaredType + "', which this version of Tapioca cannot offer.");
    }

    return Refuse (control, "is of a type this version of Tapioca cannot offer.");
}

bool CoerceAll (const InputModel& model, const std::vector<std::string>& texts, std::vector<std::string>& values,
                std::vector<std::string>& errors)
{
    values.clear ();
    errors.clear ();

    if (texts.size () != model.controls.size ()) {
        errors.push_back ("The panel offered " + std::to_string (texts.size ()) + " values for " +
                          std::to_string (model.controls.size ()) + " inputs.");
        return false;
    }

    bool ok = true;
    for (size_t index = 0; index < model.controls.size (); ++index) {
        const CoerceResult result = Coerce (model.controls[index], texts[index]);
        // The normalised value is pushed even on refusal, so `values` stays
        // parallel to the controls and a caller can render every row against its
        // own control without tracking which ones were skipped.
        values.push_back (result.ok ? result.value : texts[index]);
        if (!result.ok) {
            errors.push_back (result.error);
            ok = false;
        }
    }

    return ok;
}

} // namespace evp
