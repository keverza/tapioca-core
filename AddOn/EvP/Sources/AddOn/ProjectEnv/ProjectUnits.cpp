#include "ProjectEnv/ProjectUnits.hpp"
#include "APIEnvir.h"
#include "ACAPinc.h"
#include <algorithm>

namespace geomsrv::projectenv {

namespace {
using Units = edit::DisplayUnits;

int Decimals (short value)
{
    return std::clamp (int (value), 0, 4);
}
} // namespace

bool RefreshProjectUnits ()
{
    API_WorkingUnitPrefs prefs = {};
    if (ACAPI_ProjectSetting_GetPreferences (&prefs, APIPrefs_WorkingUnitsID) != NoError) {
        ForgetProjectUnits ();
        return false;
    }
    Units units;
    units.fromProject = true;
    switch (prefs.lengthUnit) {
        case API_LengthTypeID::Meter:
            units.length = Units::Length::Metre;
            break;
        case API_LengthTypeID::Decimeter:
            units.length = Units::Length::Decimetre;
            break;
        case API_LengthTypeID::Centimeter:
            units.length = Units::Length::Centimetre;
            break;
        case API_LengthTypeID::Millimeter:
            units.length = Units::Length::Millimetre;
            break;
        case API_LengthTypeID::KiloMeter:
            units.length = Units::Length::Kilometre;
            break;
        default: // feet, inches, yards: deferred (#109), shown in metres
            units.imperial = true;
            break;
    }
    switch (prefs.areaUnit) {
        case API_AreaTypeID::SquareMeter:
            units.area = Units::Area::SquareMetre;
            break;
        case API_AreaTypeID::SquareDeciMeter:
            units.area = Units::Area::SquareDecimetre;
            break;
        case API_AreaTypeID::SquareCentimeter:
            units.area = Units::Area::SquareCentimetre;
            break;
        case API_AreaTypeID::SquareMillimeter:
            units.area = Units::Area::SquareMillimetre;
            break;
        case API_AreaTypeID::SquareKiloMeter:
            units.area = Units::Area::SquareKilometre;
            break;
        default:
            units.imperial = true;
            break;
    }
    switch (prefs.volumeUnit) {
        case API_VolumeTypeID::CubicMeter:
            units.volume = Units::Volume::CubicMetre;
            break;
        case API_VolumeTypeID::Liter:
            units.volume = Units::Volume::Litre;
            break;
        case API_VolumeTypeID::CubicCentimeter:
            units.volume = Units::Volume::CubicCentimetre;
            break;
        case API_VolumeTypeID::CubicMillimeter:
            units.volume = Units::Volume::CubicMillimetre;
            break;
        case API_VolumeTypeID::CubicKiloMeter:
            units.volume = Units::Volume::CubicKilometre;
            break;
        default:
            units.imperial = true;
            break;
    }
    if (units.imperial) {
        // Nothing is approximated: every unit and its decimals stay metric until imperial lands.
        units = Units {};
        units.imperial = true;
        units.fromProject = true;
    }
    else {
        units.lengthDecimals = Decimals (prefs.lenDecimals);
        units.areaDecimals = Decimals (prefs.areaDecimals);
        units.volumeDecimals = Decimals (prefs.volumeDecimals);
    }
    edit::SetDisplayUnits (units);
    return true;
}

void ForgetProjectUnits ()
{
    edit::SetDisplayUnits ({});
}

} // namespace geomsrv::projectenv
