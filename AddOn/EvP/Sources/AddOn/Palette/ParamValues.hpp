#ifndef GEOMETRYSERVER_PALETTE_PARAMVALUES_HPP
#define GEOMETRYSERVER_PALETTE_PARAMVALUES_HPP

// Every conversion between a generated parameter control's VALUE and TEXT.
//
// One concern, lifted out of ParamPanel when the action selector (F3) pushed that
// file past its cap: reading a project-info default in, writing a JSON value out,
// spelling a field's accepted range, and the UTF-8 boundary into the DevKit-free
// helpers. ParamPanel builds and places controls; this decides what a value LOOKS
// like on either side of one.
//
// Nothing here touches DG. Two functions call ACAPI (through geomsrv), which is why
// this is not in the offline test suite the way Palette/ParamVisibility is.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <string>

namespace evp {

// UTF-8, for the boundary into the std::string helpers (Palette/ParamVisibility,
// Palette/CommandFilter).
std::string Utf8 (const GS::UniString& text);

// A field's accepted range, compact enough to sit beside it: "1-255", ">= 0",
// "<= 90". Built at runtime so the glyphs can be UTF-8 — the .grc is codepage 1252
// and cannot carry an en dash or a >=.
GS::UniString FormatDomain (bool haveMin, bool haveMax,
                            const GS::UniString& low, const GS::UniString& high);

// A Float's range in the same project unit its DG edit displays. Composite formats
// that cannot be represented by one multiplier omit the hint rather than lie.
GS::UniString FormatNumericDomain (const GS::UniString& apiUnit,
                                   bool haveMin, bool haveMax,
                                   double minimum, double maximum);

// The open project's Working Unit suffix for a dimensional annotation. Reads
// API_WorkingUnitPrefs, the same preference family DG's specialised edits use.
GS::UniString ProjectUnitLabel (const GS::UniString& unit);

// The project's WORKING LENGTH UNIT: the factor that turns a value in metres into
// that unit, and the unit's short name ("mm", "cm", "m").
//
// WHY THIS EXISTS — a reported defect, not a nicety. A Float carrying unit="m"
// becomes a DG::LengthEdit, which RENDERS in the project's working units while its
// SetValue/GetValue/SetMin/SetMax stay in metres (LengthEdit inherits all four from
// RealEdit unchanged and only differs in formatting). So a script always sees
// metres — correct — but a range hint printed from the raw numbers said "0.05-0.5"
// beside a field showing "500" in a millimetre project. The value was right and the
// label was a lie, which is the worst combination: the user retunes a number that
// was never wrong.
//
// Returns false for the foot-and-inch composites (FootFracInch, FootDecInch,
// FracInch, DecInch) and Yard: those have no single multiplier that produces the
// string Archicad actually shows, and a wrong hint is worse than none — the caller
// falls back to omitting the unit rather than inventing one.
//
// Length only by design; FormatNumericDomain handles area, volume, and angle from
// their separate API_WorkingUnitPrefs fields.
bool WorkingLengthUnit (double& metresToUnit, GS::UniString& shortName);

// Parse a possibly localised number ("2 500,00 m2", "1,234.5") to a double —
// mirrors MassingFeasibility's parse_number so a prefilled project field reads the
// same value the command would. Grabs the first numeric token, then resolves the
// comma: both separators present -> comma is thousands; comma only -> decimal.
bool ParseLocalizedNumber (const GS::UniString& text, double& out);

// Resolve default_from="project:<field name>" to a number at dialog-build time. The
// "project:" prefix namespaces the source so other sources can be added later; an
// unknown prefix or unmatched field returns false and leaves the literal default
// untouched.
bool ResolveDefaultFromNumber (const GS::UniString& spec, double& out);

// A Float input must reach Python as a JSON REAL, never an integer literal.
//
// "%.10g" prints 4.0 as "4", so `json.loads` handed run() the Python int 4 for a
// parameter declared evp.Float. A command then computes `origin_x + size` = 0 + 4 and
// puts an INT into a coordinate list, and an Int element does not survive
// GS::ObjectState::Get<GS::Array<double>> on the native side — the coordinate arrives
// as 0.0 and the polygon collapses to coincident points.
//
// That is the whole APIERR_IRREGULARPOLY investigation (see
// Commands/CreateRoofProbe/ROOF-CREATION-STATE.md): a roof over a 4 m square failed
// while the identical roof over a 4.5 m square created, because only the second one
// produced floats. It silently mis-set scalars too — an integral `base` became 0.0, so
// elements landed at the wrong height with no error at all.
//
// Fixed where the wrong type is born, once, for every command and every Float input.
GS::UniString JsonReal (double value);

// JSON string escaping: `"`, `\`, AND every control character (U+0000..U+001F),
// which RFC 8259 forbids raw inside a string.
//
// ⚠️ The control characters are not theoretical, and this function used to omit
// them on the assumption that "a generated control can only hold flat,
// control-character-free strings". An ARCHICAD LAYER NAME disproved it: a real
// layer in a real project is named with a single control character, the attribute
// picker handed that name to CollectJson verbatim, and the params JSON reached
// Python as `{"layer":"<0x..>",...}` — which json.loads rejects outright, so the
// command died before its first line ran, with a decoder error that named a
// column number and nothing else.
//
// Any Text input containing a newline had the same bug. Escape here, once, for
// every control that serialises a string.
GS::UniString EscapeJson (const GS::UniString& text);

}   // namespace evp

#endif
