#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "Python/MainThreadGate.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {

NativeCommandResult CaptureSunStudyInputs (const GS::ObjectState& params,
                                           std::shared_ptr<const CapturedSunStudyInputs>& captured)
{
    if (!evp::MainThreadGate::Get ().IsMainThread ())
        return NativeCommandResult::Failure ("sun study input capture requires the host main thread");
    auto inputs = std::make_shared<CapturedSunStudyInputs> ();
    inputs->params = params;
    inputs->snapshot = MeshStore::Get ().Current ();
    if (inputs->snapshot == nullptr)
        return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");
    const GSErrCode err = ACAPI_GeoLocation_GetPlaceSets (&inputs->place);
    if (err != NoError)
        return NativeCommandResult::Failure (
            EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", err, "reading the project's geo location"));

    using sunstudysupport::ReadInt;
    const API_PlaceInfo& place = inputs->place;
    const GS::Int32 year = ReadInt (params, "year", place.year);
    const GS::Int32 month = ReadInt (params, "month", place.month);
    const GS::Int32 day = ReadInt (params, "day", place.day);
    const GS::Int32 timestep = std::max<GS::Int32> (1, ReadInt (params, "timestep", 60));
    const GS::Int32 hourFrom = ReadInt (params, "hourFrom", 0);
    const GS::Int32 hourTo = ReadInt (params, "hourTo", 24);
    const double minAltitude = sunstudysupport::ReadDouble (params, "minAltitudeDeg", 0.0);
    std::vector<evp::sunstudy::SunStep> raw;
    for (const auto& moment : evp::sunstudy::EnumerateTimesteps (timestep, hourFrom, hourTo)) {
        API_PlaceInfo momentPlace = place;
        momentPlace.year = (unsigned short) year;
        momentPlace.month = (unsigned short) month;
        momentPlace.day = (unsigned short) day;
        momentPlace.hour = (unsigned short) moment.hour;
        momentPlace.minute = (unsigned short) moment.minute;
        momentPlace.second = 0;
        if (ACAPI_GeoLocation_CalcSunOnPlace (&momentPlace) != NoError)
            continue;
        evp::sunstudy::SunStep step;
        step.time = moment;
        step.altitudeDegrees = momentPlace.sunAngZ * 180.0 / 3.14159265358979323846;
        // Archicad's angle is already in model coordinates; no extra north term.
        const double horizontal = std::cos (momentPlace.sunAngZ);
        step.direction[0] = horizontal * std::cos (momentPlace.sunAngXY);
        step.direction[1] = horizontal * std::sin (momentPlace.sunAngXY);
        step.direction[2] = std::sin (momentPlace.sunAngZ);
        raw.push_back (step);
    }
    inputs->series = evp::sunstudy::SunSeries::FromSteps (raw, timestep, minAltitude);
    captured = std::move (inputs);
    return GS::ObjectState ();
}

} // namespace geomsrv
