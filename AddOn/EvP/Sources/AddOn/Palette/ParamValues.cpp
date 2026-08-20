#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/ParamValues.hpp"
#include "AddOnCommands.hpp"   // geomsrv::ProjectInfoField — default_from="project:..."

#include <cstdio>    // std::snprintf — JsonReal
#include <cstdlib>   // std::strtod   — ParseLocalizedNumber

namespace evp {

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

GS::UniString FormatDomain (bool haveMin, bool haveMax,
                            const GS::UniString& low, const GS::UniString& high)
{
    if (haveMin && haveMax)
        return low + GS::UniString ("\xE2\x80\x93", CC_UTF8) + high;   // en dash
    if (haveMin)
        return GS::UniString ("\xE2\x89\xA5 ", CC_UTF8) + low;         // >=
    if (haveMax)
        return GS::UniString ("\xE2\x89\xA4 ", CC_UTF8) + high;        // <=
    return GS::UniString ();
}

GS::UniString FormatNumericDomain (const GS::UniString& apiUnit,
                                   bool haveMin, bool haveMax,
                                   double minimum, double maximum)
{
    double        toUnit = 1.0;
    GS::UniString unitName;
    bool          convert = false;
    API_WorkingUnitPrefs prefs = {};
    const bool havePrefs = ACAPI_ProjectSetting_GetPreferences (
        &prefs, APIPrefs_WorkingUnitsID) == NoError;
    if (apiUnit == "m") {
        convert = WorkingLengthUnit (toUnit, unitName);
    } else if (havePrefs && apiUnit == "m2") {
        switch (prefs.areaUnit) {
            case API_AreaTypeID::SquareMeter: unitName = "m2"; convert = true; break;
            case API_AreaTypeID::SquareKiloMeter: toUnit = 1e-6; unitName = "km2"; convert = true; break;
            case API_AreaTypeID::SquareDeciMeter: toUnit = 100.0; unitName = "dm2"; convert = true; break;
            case API_AreaTypeID::SquareCentimeter: toUnit = 10000.0; unitName = "cm2"; convert = true; break;
            case API_AreaTypeID::SquareMillimeter: toUnit = 1e6; unitName = "mm2"; convert = true; break;
            case API_AreaTypeID::SquareFoot: toUnit = 10.7639104167097; unitName = "ft2"; convert = true; break;
            case API_AreaTypeID::SquareInch: toUnit = 1550.0031000062; unitName = "in2"; convert = true; break;
            case API_AreaTypeID::SquareYard: toUnit = 1.19599004630108; unitName = "yd2"; convert = true; break;
        }
    } else if (havePrefs && apiUnit == "m3") {
        switch (prefs.volumeUnit) {
            case API_VolumeTypeID::CubicMeter: unitName = "m3"; convert = true; break;
            case API_VolumeTypeID::CubicKiloMeter: toUnit = 1e-9; unitName = "km3"; convert = true; break;
            case API_VolumeTypeID::Liter: toUnit = 1000.0; unitName = "L"; convert = true; break;
            case API_VolumeTypeID::CubicCentimeter: toUnit = 1e6; unitName = "cm3"; convert = true; break;
            case API_VolumeTypeID::CubicMillimeter: toUnit = 1e9; unitName = "mm3"; convert = true; break;
            case API_VolumeTypeID::CubicFoot: toUnit = 35.3146667214886; unitName = "ft3"; convert = true; break;
            case API_VolumeTypeID::CubicInch: toUnit = 61023.7440947323; unitName = "in3"; convert = true; break;
            case API_VolumeTypeID::CubicYard: toUnit = 1.30795061931439; unitName = "yd3"; convert = true; break;
            case API_VolumeTypeID::Gallon: return GS::UniString ();
        }
    } else if (havePrefs && apiUnit == "rad") {
        switch (prefs.angleUnit) {
            case API_AngleTypeID::DecimalDegree:
                toUnit = 180.0 / 3.14159265358979323846; unitName = "degrees"; convert = true; break;
            case API_AngleTypeID::Grad:
                toUnit = 200.0 / 3.14159265358979323846; unitName = "grad"; convert = true; break;
            case API_AngleTypeID::Radian:
                unitName = "rad"; convert = true; break;
            default:
                return GS::UniString ();
        }
    }

    GS::UniString text = FormatDomain (haveMin, haveMax,
                                       GS::UniString::Printf ("%g", minimum * toUnit),
                                       GS::UniString::Printf ("%g", maximum * toUnit));
    if ((apiUnit == "m" || apiUnit == "m2" || apiUnit == "m3" || apiUnit == "rad") && !convert)
        return GS::UniString ();
    if (convert && !text.IsEmpty ())
        text += " " + unitName;
    return text;
}

GS::UniString ProjectUnitLabel (const GS::UniString& unit)
{
    API_WorkingUnitPrefs prefs = {};
    if (ACAPI_ProjectSetting_GetPreferences (&prefs, APIPrefs_WorkingUnitsID) != NoError)
        return GS::UniString ();

    if (unit == "m") {
        switch (prefs.lengthUnit) {
            case API_LengthTypeID::Meter: return "m";
            case API_LengthTypeID::Decimeter: return "dm";
            case API_LengthTypeID::Centimeter: return "cm";
            case API_LengthTypeID::Millimeter: return "mm";
            case API_LengthTypeID::FootFracInch:
            case API_LengthTypeID::FootDecInch: return "ft-in";
            case API_LengthTypeID::DecFoot: return "ft";
            case API_LengthTypeID::FracInch:
            case API_LengthTypeID::DecInch: return "in";
            case API_LengthTypeID::KiloMeter: return "km";
            case API_LengthTypeID::Yard: return "yd";
        }
    }
    if (unit == "m2") {
        switch (prefs.areaUnit) {
            case API_AreaTypeID::SquareMeter: return "m2";
            case API_AreaTypeID::SquareKiloMeter: return "km2";
            case API_AreaTypeID::SquareDeciMeter: return "dm2";
            case API_AreaTypeID::SquareCentimeter: return "cm2";
            case API_AreaTypeID::SquareMillimeter: return "mm2";
            case API_AreaTypeID::SquareFoot: return "ft2";
            case API_AreaTypeID::SquareInch: return "in2";
            case API_AreaTypeID::SquareYard: return "yd2";
        }
    }
    if (unit == "m3") {
        switch (prefs.volumeUnit) {
            case API_VolumeTypeID::CubicMeter: return "m3";
            case API_VolumeTypeID::CubicKiloMeter: return "km3";
            case API_VolumeTypeID::Liter: return "L";
            case API_VolumeTypeID::CubicCentimeter: return "cm3";
            case API_VolumeTypeID::CubicMillimeter: return "mm3";
            case API_VolumeTypeID::CubicFoot: return "ft3";
            case API_VolumeTypeID::CubicInch: return "in3";
            case API_VolumeTypeID::CubicYard: return "yd3";
            case API_VolumeTypeID::Gallon: return "gal";
        }
    }
    if (unit == "rad") {
        switch (prefs.angleUnit) {
            case API_AngleTypeID::DecimalDegree: return "degrees";
            case API_AngleTypeID::DegreeMinSec: return "deg-min-sec";
            case API_AngleTypeID::Grad: return "grad";
            case API_AngleTypeID::Radian: return "rad";
            case API_AngleTypeID::Surveyors: return "surveyor";
        }
    }
    return GS::UniString ();
}

bool WorkingLengthUnit (double& metresToUnit, GS::UniString& shortName)
{
    API_WorkingUnitPrefs prefs = {};
    if (ACAPI_ProjectSetting_GetPreferences (&prefs, APIPrefs_WorkingUnitsID) != NoError)
        return false;

    switch (prefs.lengthUnit) {
        case API_LengthTypeID::Meter:      metresToUnit = 1.0;          shortName = "m";  return true;
        case API_LengthTypeID::Decimeter:  metresToUnit = 10.0;         shortName = "dm"; return true;
        case API_LengthTypeID::Centimeter: metresToUnit = 100.0;        shortName = "cm"; return true;
        case API_LengthTypeID::Millimeter: metresToUnit = 1000.0;       shortName = "mm"; return true;
        case API_LengthTypeID::KiloMeter:  metresToUnit = 0.001;        shortName = "km"; return true;
        case API_LengthTypeID::DecFoot:    metresToUnit = 1.0 / 0.3048; shortName = "ft"; return true;
        default:
            // Foot-and-inch composites and Yard — see the header note on why these
            // are refused rather than approximated.
            return false;
    }
}

bool ParseLocalizedNumber (const GS::UniString& text, double& out)
{
    const USize len = text.GetLength ();

    // Find the token start: the first sign or digit.
    UIndex start = 0;
    bool   found = false;
    for (UIndex i = 0; i < len; ++i) {
        const GS::uchar_t c = text[i];
        if ((c >= '0' && c <= '9') || c == '+' || c == '-') { start = i; found = true; break; }
    }
    if (!found)
        return false;

    GS::UniString token;
    bool          seenDigit = false;
    for (UIndex i = start; i < len; ++i) {
        const GS::uchar_t c = text[i];
        if (c >= '0' && c <= '9') {
            token += c;
            seenDigit = true;
        } else if (c == '.' || c == ',') {
            token += c;                         // separator — kept, resolved below
        } else if (c == '+' || c == '-') {
            if (token.IsEmpty ()) token += c;   // a sign leads the token, nowhere else
            else break;
        } else if (c == ' ') {
            // A space between digits is a thousands separator (drop it); a space
            // after the number is where the unit begins (stop).
            UIndex j = i + 1;
            while (j < len && text[j] == ' ') ++j;
            const GS::uchar_t next = (j < len) ? (GS::uchar_t) text[j] : 0;
            if (!(seenDigit && next >= '0' && next <= '9'))
                break;
        } else {
            break;                              // unit / other suffix after the number
        }
    }
    if (!seenDigit)
        return false;

    const bool hasComma = token.Contains (',');
    const bool hasDot   = token.Contains ('.');
    if (hasComma && hasDot)
        token.ReplaceAll (GS::UniString (","), GS::UniString (""));   // 2,500.00 -> 2500.00 (thousands)
    else if (hasComma)
        token.ReplaceAll (',', '.');                                  // 2500,00  -> 2500.00 (decimal)

    const auto  cstr = token.ToCStr ();   // keep the buffer alive for strtod
    char*       endp = nullptr;
    const double v = std::strtod (cstr.Get (), &endp);
    if (endp == cstr.Get ())
        return false;
    out = v;
    return true;
}

bool ResolveDefaultFromNumber (const GS::UniString& spec, double& out)
{
    const GS::UniString prefix ("project:");
    if (!spec.BeginsWith (prefix))
        return false;
    GS::UniString field = spec.GetSubstring (prefix.GetLength (), spec.GetLength () - prefix.GetLength ());
    field.Trim ();
    GS::UniString value;
    if (!geomsrv::ProjectInfoField (field, value))
        return false;
    return ParseLocalizedNumber (value, out);
}

GS::UniString JsonReal (double value)
{
    // Append ".0" whenever the formatted number carries no fraction, exponent, or
    // non-finite marker. See the header for what this one line is paying for.
    char buffer[64];
    std::snprintf (buffer, sizeof buffer, "%.10g", value);
    for (const char* p = buffer; *p != '\0'; ++p) {
        if (*p == '.' || *p == 'e' || *p == 'E' ||
            *p == 'n' || *p == 'N' || *p == 'i' || *p == 'I')
            return GS::UniString (buffer);
    }
    return GS::UniString (buffer) + ".0";
}

GS::UniString EscapeJson (const GS::UniString& text)
{
    GS::UniString out;
    for (UIndex i = 0; i < text.GetLength (); ++i) {
        const GS::uchar_t c = text[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c >= 0x20) {
            out += c;
        } else {
            // A control character. RFC 8259 forbids these raw inside a string, and
            // a raw one makes json.loads reject the WHOLE params object — see the
            // layer-name story in the header. The five with short forms get them;
            // everything else goes out as \u00XX, which is always legal.
            switch (c) {
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += GS::UniString::Printf ("\\u%04X", (int) c); break;
            }
        }
    }
    return out;
}

}   // namespace evp
