#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyWeatherSun.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "Python/MainThreadGate.hpp"

#include <cmath>

namespace geomsrv {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The constant azimuth turn (degrees, counter-clockwise) that best maps the
// independent suns onto the host's, as a circular mean over pairs where both
// are up, and the circular spread around it.
void BestTurn (const std::vector<std::array<double, 3>>& host, const std::vector<std::array<double, 3>>& independent,
               const std::vector<uint8_t>& captured, double& turnDegrees, double& spreadDegrees)
{
    double c = 0.0, s = 0.0;
    size_t n = 0;
    for (size_t k = 0; k < host.size () && k < independent.size (); ++k) {
        if (captured[k] == 0 || host[k][2] <= 0.0 || independent[k][2] <= 0.0)
            continue;
        const double d = std::atan2 (host[k][1], host[k][0]) - std::atan2 (independent[k][1], independent[k][0]);
        c += std::cos (d);
        s += std::sin (d);
        ++n;
    }
    if (n == 0) {
        turnDegrees = spreadDegrees = 0.0;
        return;
    }
    turnDegrees = std::atan2 (s, c) * 180.0 / kPi;
    const double r = std::min (1.0, std::hypot (c, s) / static_cast<double> (n));
    spreadDegrees = std::sqrt (std::max (0.0, -2.0 * std::log (std::max (r, 1e-12)))) * 180.0 / kPi;
}

} // namespace

bool CaptureWeatherSun (const WeatherSunRequest& request, WeatherSunCapture& out, std::string& error)
{
    using namespace evp::sunstudy::rust;
    if (!evp::MainThreadGate::Get ().IsMainThread ()) {
        error = "weather sun capture requires the host main thread";
        return false;
    }
    API_PlaceInfo place = {};
    const GSErrCode err = ACAPI_GeoLocation_GetPlaceSets (&place);
    if (err != NoError) {
        error = EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", err, "reading the project's geo location")
                    .ToCStr (0, MaxUSize, CC_UTF8)
                    .Get ();
        return false;
    }
    WeatherSunCapture capture;
    capture.latitude = place.latitude;
    capture.longitude = place.longitude;
    capture.altitude = place.altitude;
    capture.north = place.north;
    capture.timeZoneInMinutes = place.timeZoneInMinutes;
    capture.projectSumTime = place.sumTime;
    capture.projectOffsetHours =
        place.timeZoneInMinutes / 60.0;         // east of Greenwich positive (measured, see the header)
    capture.rotation = place.north - kPi / 2.0; // north CCW from +X (measured, see the header)

    WeatherPlacement placement;
    placement.year = request.year;
    placement.recordsPerHour = request.recordsPerHour;
    placement.stationOffsetHours = request.stationOffsetHours;
    placement.projectOffsetHours = capture.projectOffsetHours;
    placement.latitude = place.latitude;
    placement.longitude = place.longitude;
    placement.rotation = capture.rotation;
    if (!WeatherInstants (request.stamps, placement, capture.instants, error))
        return false;

    const size_t count = capture.instants.size ();
    capture.host.assign (count, { 0.0, 0.0, 0.0 });
    capture.captured.assign (count, 0);
    std::vector<double> host, independent;
    for (size_t k = 0; k < count; ++k) {
        const WeatherInstant& w = capture.instants[k];
        if (!w.valid) {
            error = "weather record " + std::to_string (k) + " has an impossible date or hour";
            return false;
        }
        const auto seconds = static_cast<long> (std::lround (w.projectSeconds));
        API_PlaceInfo moment = place;
        moment.sumTime = false; // standard time: never the project's flag
        moment.year = static_cast<unsigned short> (w.projectYear);
        moment.month = static_cast<unsigned short> (w.projectMonth);
        moment.day = static_cast<unsigned short> (w.projectDay);
        moment.hour = static_cast<unsigned short> (seconds / 3600);
        moment.minute = static_cast<unsigned short> ((seconds / 60) % 60);
        moment.second = static_cast<unsigned short> (seconds % 60);
        if (ACAPI_GeoLocation_CalcSunOnPlace (&moment) != NoError) {
            ++capture.failedCalls;
            continue;
        }
        // Archicad's angle is already in model coordinates (as CaptureSeries).
        const double horizontal = std::cos (moment.sunAngZ);
        capture.host[k] = { horizontal * std::cos (moment.sunAngXY), horizontal * std::sin (moment.sunAngXY),
                            std::sin (moment.sunAngZ) };
        capture.captured[k] = 1;
        host.insert (host.end (), capture.host[k].begin (), capture.host[k].end ());
        independent.insert (independent.end (), w.independentSun.begin (), w.independentSun.end ());
    }
    if (!CrossCheckSun (host, independent, capture.check, error))
        return false;
    std::vector<std::array<double, 3>> independentSuns;
    independentSuns.reserve (count);
    for (const WeatherInstant& w : capture.instants)
        independentSuns.push_back (w.independentSun);
    BestTurn (capture.host, independentSuns, capture.captured, capture.bestTurnDegrees, capture.turnSpreadDegrees);
    out = std::move (capture);
    error.clear ();
    return true;
}

} // namespace geomsrv
