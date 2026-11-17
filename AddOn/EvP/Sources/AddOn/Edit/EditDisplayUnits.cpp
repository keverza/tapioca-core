#include "Edit/EditDisplayUnits.hpp"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace geomsrv::edit {

namespace {
std::mutex s_lock;
DisplayUnits s_units;

double LengthFactor (DisplayUnits::Length length) // shown units per metre
{
    switch (length) {
        case DisplayUnits::Length::Decimetre:
            return 10.0;
        case DisplayUnits::Length::Centimetre:
            return 100.0;
        case DisplayUnits::Length::Millimetre:
            return 1000.0;
        case DisplayUnits::Length::Kilometre:
            return 0.001;
        case DisplayUnits::Length::Metre:
            break;
    }
    return 1.0;
}

std::string Format (double value, int decimals, bool sign, const char* symbol)
{
    char text[64];
    const int d = std::clamp (decimals, 0, 4);
    if (sign)
        std::snprintf (text, sizeof text, "%+.*f", d, value);
    else
        std::snprintf (text, sizeof text, "%.*f", d, value);
    std::string out = text;
    if (symbol != nullptr)
        out += std::string (" ") + symbol;
    return out;
}
} // namespace

DisplayUnits CurrentDisplayUnits ()
{
    std::lock_guard<std::mutex> guard (s_lock);
    return s_units;
}

void SetDisplayUnits (const DisplayUnits& units)
{
    std::lock_guard<std::mutex> guard (s_lock);
    s_units = units;
}

bool IsLength (Quantity quantity)
{
    switch (quantity) {
        case Quantity::Angle:
        case Quantity::Slope:
        case Quantity::Factor:
            return false;
        default:
            return true;
    }
}

double MetresToLength (const DisplayUnits& units, double metres)
{
    return metres * LengthFactor (units.length);
}

double LengthToMetres (const DisplayUnits& units, double shown)
{
    return shown / LengthFactor (units.length);
}

const char* LengthSymbol (const DisplayUnits& units)
{
    switch (units.length) {
        case DisplayUnits::Length::Decimetre:
            return "dm";
        case DisplayUnits::Length::Centimetre:
            return "cm";
        case DisplayUnits::Length::Millimetre:
            return "mm";
        case DisplayUnits::Length::Kilometre:
            return "km";
        case DisplayUnits::Length::Metre:
            break;
    }
    return "m";
}

const char* AreaSymbol (const DisplayUnits& units)
{
    switch (units.area) {
        case DisplayUnits::Area::SquareDecimetre:
            return "dm2";
        case DisplayUnits::Area::SquareCentimetre:
            return "cm2";
        case DisplayUnits::Area::SquareMillimetre:
            return "mm2";
        case DisplayUnits::Area::SquareKilometre:
            return "km2";
        case DisplayUnits::Area::SquareMetre:
            break;
    }
    return "m2";
}

const char* VolumeSymbol (const DisplayUnits& units)
{
    switch (units.volume) {
        case DisplayUnits::Volume::Litre:
            return "L";
        case DisplayUnits::Volume::CubicCentimetre:
            return "cm3";
        case DisplayUnits::Volume::CubicMillimetre:
            return "mm3";
        case DisplayUnits::Volume::CubicKilometre:
            return "km3";
        case DisplayUnits::Volume::CubicMetre:
            break;
    }
    return "m3";
}

double SquareMetresToArea (const DisplayUnits& units, double squareMetres)
{
    switch (units.area) {
        case DisplayUnits::Area::SquareDecimetre:
            return squareMetres * 1.0e2;
        case DisplayUnits::Area::SquareCentimetre:
            return squareMetres * 1.0e4;
        case DisplayUnits::Area::SquareMillimetre:
            return squareMetres * 1.0e6;
        case DisplayUnits::Area::SquareKilometre:
            return squareMetres * 1.0e-6;
        case DisplayUnits::Area::SquareMetre:
            break;
    }
    return squareMetres;
}

double CubicMetresToVolume (const DisplayUnits& units, double cubicMetres)
{
    switch (units.volume) {
        case DisplayUnits::Volume::Litre:
            return cubicMetres * 1.0e3;
        case DisplayUnits::Volume::CubicCentimetre:
            return cubicMetres * 1.0e6;
        case DisplayUnits::Volume::CubicMillimetre:
            return cubicMetres * 1.0e9;
        case DisplayUnits::Volume::CubicKilometre:
            return cubicMetres * 1.0e-9;
        case DisplayUnits::Volume::CubicMetre:
            break;
    }
    return cubicMetres;
}

std::string FormatLength (const DisplayUnits& units, double metres, bool sign, bool symbol)
{
    return Format (MetresToLength (units, metres), units.lengthDecimals, sign, symbol ? LengthSymbol (units) : nullptr);
}

std::string FormatArea (const DisplayUnits& units, double squareMetres, bool symbol)
{
    return Format (SquareMetresToArea (units, squareMetres), units.areaDecimals, false,
                   symbol ? AreaSymbol (units) : nullptr);
}

std::string FormatVolume (const DisplayUnits& units, double cubicMetres, bool symbol)
{
    return Format (CubicMetresToVolume (units, cubicMetres), units.volumeDecimals, false,
                   symbol ? VolumeSymbol (units) : nullptr);
}

double TypedToDocument (const DisplayUnits& units, Quantity quantity, double typed)
{
    return IsLength (quantity) ? LengthToMetres (units, typed) : typed;
}

} // namespace geomsrv::edit
