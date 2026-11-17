#ifndef EVP_EDIT_EDITDISPLAYUNITS_HPP
#define EVP_EDIT_EDITDISPLAYUNITS_HPP

// The units the editor shows and reads lengths, areas and volumes in (#109). The document is
// always metres; only what is written on screen and what is typed is in these units. They are the
// project's working units, taken from Archicad on the main thread (ProjectUnits.cpp) into one
// process-wide snapshot that any thread may read. An imperial project is shown in metres for now
// (imperial display is deferred): `imperial` says so, and nothing is approximated.
#include "Edit/EditTracker.hpp"
#include <cstdint>
#include <string>

namespace geomsrv::edit {

struct DisplayUnits {
    enum class Length : uint8_t { Metre, Decimetre, Centimetre, Millimetre, Kilometre };
    enum class Area : uint8_t { SquareMetre, SquareDecimetre, SquareCentimetre, SquareMillimetre, SquareKilometre };
    enum class Volume : uint8_t { CubicMetre, Litre, CubicCentimetre, CubicMillimetre, CubicKilometre };
    Length length = Length::Metre;
    Area area = Area::SquareMetre;
    Volume volume = Volume::CubicMetre;
    int lengthDecimals = 2, areaDecimals = 2, volumeDecimals = 2; // 0..4, as Archicad keeps them
    bool imperial = false;    // the project's units are imperial: shown in metres until that lands
    bool fromProject = false; // read from Archicad; false: the defaults above
    bool operator== (const DisplayUnits&) const = default;
};

// The snapshot. Set on the main thread when a project opens or its units change; read anywhere.
DisplayUnits CurrentDisplayUnits ();
void SetDisplayUnits (const DisplayUnits& units);

// Whether a tracker quantity is a length (X, Y, Z, Length, Elevation, Width, Distance, Height,
// Radius): those are shown and typed in the length unit. Angles, slopes and factors are not.
bool IsLength (Quantity quantity);
double MetresToLength (const DisplayUnits& units, double metres);
double LengthToMetres (const DisplayUnits& units, double shown);
const char* LengthSymbol (const DisplayUnits& units);
const char* AreaSymbol (const DisplayUnits& units);   // "m2", "cm2", ...
const char* VolumeSymbol (const DisplayUnits& units); // "m3", "L", ...
double SquareMetresToArea (const DisplayUnits& units, double squareMetres);
double CubicMetresToVolume (const DisplayUnits& units, double cubicMetres);

// "12.50 m", "1250 cm" (`sign`: a + on positive values, for heights); without the symbol when
// `symbol` is false. Area and volume likewise, in their own decimals.
std::string FormatLength (const DisplayUnits& units, double metres, bool sign = false, bool symbol = true);
std::string FormatArea (const DisplayUnits& units, double squareMetres, bool symbol = true);
std::string FormatVolume (const DisplayUnits& units, double cubicMetres, bool symbol = true);

// What was typed for `quantity`, back in the document's metres (a length), or as typed (anything
// else).
double TypedToDocument (const DisplayUnits& units, Quantity quantity, double typed);

} // namespace geomsrv::edit
#endif
