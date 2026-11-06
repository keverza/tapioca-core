#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "NativeCommands/SelectionSetStore.hpp"
#include "Python/MainThreadGate.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {

namespace {

// The day's sun, one SDK call per timestep, from the captured place settings.
// A failed call is skipped here (the day study's long-standing behaviour) and
// counted, so a caller that must not read a gap as darkness can refuse.
evp::sunstudy::SunSeries CaptureSeries (const API_PlaceInfo& place, const GS::ObjectState& params,
                                        size_t& failedSunCalls)
{
    using sunstudysupport::ReadInt;
    const GS::Int32 year = ReadInt (params, "year", place.year);
    const GS::Int32 month = ReadInt (params, "month", place.month);
    const GS::Int32 day = ReadInt (params, "day", place.day);
    const GS::Int32 timestep = std::max<GS::Int32> (1, ReadInt (params, "timestep", 60));
    const GS::Int32 hourFrom = ReadInt (params, "hourFrom", 0);
    const GS::Int32 hourTo = ReadInt (params, "hourTo", 24);
    const double minAltitude = sunstudysupport::ReadDouble (params, "minAltitudeDeg", 0.0);
    std::vector<evp::sunstudy::SunStep> raw;
    failedSunCalls = 0;
    for (const auto& moment : evp::sunstudy::EnumerateTimesteps (timestep, hourFrom, hourTo)) {
        API_PlaceInfo momentPlace = place;
        momentPlace.year = (unsigned short) year;
        momentPlace.month = (unsigned short) month;
        momentPlace.day = (unsigned short) day;
        momentPlace.hour = (unsigned short) moment.hour;
        momentPlace.minute = (unsigned short) moment.minute;
        momentPlace.second = 0;
        if (ACAPI_GeoLocation_CalcSunOnPlace (&momentPlace) != NoError) {
            ++failedSunCalls;
            continue;
        }
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
    return evp::sunstudy::SunSeries::FromSteps (raw, timestep, minAltitude);
}

} // namespace

NativeCommandResult CaptureSunStudyInputs (const GS::ObjectState& params,
                                           std::shared_ptr<const CapturedSunStudyInputs>& captured)
{
    if (!evp::MainThreadGate::Get ().IsMainThread ())
        return NativeCommandResult::Failure ("sun study input capture requires the host main thread");
    auto inputs = std::make_shared<CapturedSunStudyInputs> ();
    inputs->params = params;
    auto& binding = inputs->selectionBinding;
    binding.contextSet = sunstudysupport::ReadString (params, "contextSelectionSet", "");
    binding.ignoredSet = sunstudysupport::ReadString (params, "ignoredSelectionSet", "");
    binding.analysisSet = sunstudysupport::ReadString (params, "analysisSelectionSet", "");
    auto& selections = SelectionSetStore::Get ();
    const auto readSet = [&selections] (const std::string& name, std::vector<std::string>& guids) {
        const GS::UniString setName (name.c_str (), CC_UTF8);
        if (!selections.IsDeclared (setName))
            return false;
        for (const auto& guid : selections.Values (setName))
            guids.push_back (sunstudysupport::Utf8 (guid));
        return true;
    };
    if ((!binding.contextSet.empty () &&
         (params.Contains ("contextElements") || !readSet (binding.contextSet, inputs->contextElements))) ||
        (!binding.ignoredSet.empty () &&
         (params.Contains ("ignoredElements") || !readSet (binding.ignoredSet, inputs->ignoredElements))) ||
        (!binding.analysisSet.empty () &&
         (params.Contains ("analysisElements") || !readSet (binding.analysisSet, inputs->analysisElements))))
        return NativeCommandResult::Failure (
            "role selection sets must be declared and cannot also specify element lists");
    if (!binding.contextSet.empty () || !binding.ignoredSet.empty () || !binding.analysisSet.empty ()) {
        binding.generation = selections.Generation ();
        binding.revision = selections.Revision ();
    }
    inputs->snapshot = MeshStore::Get ().Current ();
    if (inputs->snapshot == nullptr)
        return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");
    const auto preset = sunstudysupport::ReadString (params, "preset", "");
    if (preset == "late")
        inputs->materialTransparency = inputs->snapshot->materialTransparency;
    const GSErrCode err = ACAPI_GeoLocation_GetPlaceSets (&inputs->place);
    if (err != NoError)
        return NativeCommandResult::Failure (
            EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", err, "reading the project's geo location"));

    size_t failedSunCalls = 0;
    inputs->series = CaptureSeries (inputs->place, params, failedSunCalls);
    captured = std::move (inputs);
    return GS::ObjectState ();
}

NativeCommandResult RecaptureSunStudyDay (const CapturedSunStudyInputs& base, const GS::ObjectState& params,
                                          std::shared_ptr<const CapturedSunStudyInputs>& captured)
{
    if (!evp::MainThreadGate::Get ().IsMainThread ())
        return NativeCommandResult::Failure ("sun study input capture requires the host main thread");
    auto inputs = std::make_shared<CapturedSunStudyInputs> (base);
    inputs->params = params;
    size_t failedSunCalls = 0;
    inputs->series = CaptureSeries (inputs->place, params, failedSunCalls);
    if (failedSunCalls != 0)
        return NativeCommandResult::Failure (GS::UniString::Printf (
            "Archicad could not place the sun at %u timestep(s) of %d-%02d-%02d; the period "
            "was not measured with a gap read as shade",
            unsigned (failedSunCalls), int (sunstudysupport::ReadInt (params, "year", 0)),
            int (sunstudysupport::ReadInt (params, "month", 0)), int (sunstudysupport::ReadInt (params, "day", 0))));
    captured = std::move (inputs);
    return GS::ObjectState ();
}

} // namespace geomsrv
