#ifndef EVP_NATIVECOMMANDS_SUNSTUDYWEATHERSUN_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYWEATHERSUN_HPP

// NativeCommands/SunStudyWeatherSun -- Archicad's sun for every interval of a
// weather file (#155 P2), and the check that it is the sun we think it is.
//
// ⚠️ DAYLIGHT SAVING IS OFF. A weather file runs on standard time; every host
// call is made with `sumTime = false` at the interval's midpoint on the
// project's standard-time clock. The project's own flag is recorded, never
// inherited -- inherited, it would shift every sun by an hour with no symptom.
//
// ⚠️ THE HOST'S SUN IS THE AUTHORITY, THE FORMULA ONLY CHECKS IT. The
// independent (NOAA) sun is turned into the model frame with an ASSUMED north
// convention (`rotation = north - pi/2`: `north` the angle of true north
// counter-clockwise from model +X, pi/2 when north is up) and an ASSUMED time
// zone sign (`timeZoneInMinutes` east of Greenwich positive). The DevKit does
// not state either. The capture records the raw values and, beside the
// verdict, the constant azimuth turn that would best align the two suns: a
// pure frame mismatch shows as a turn with a small spread, a clock error does
// not. Neither is applied; a refused check is refused.
//
// Main thread only (ACAPI).

#include "SunStudy/SunStudyRustClimate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {

struct WeatherSunRequest {
    std::vector<evp::sunstudy::rust::WeatherStamp> stamps; // one per record
    int year = 2026;                                       // the calendar the weather is placed in
    int recordsPerHour = 1;
    double stationOffsetHours = 0.0; // EPW header time zone
};

struct WeatherSunCapture {
    // Per stamp, in order.
    std::vector<evp::sunstudy::rust::WeatherInstant> instants;
    std::vector<std::array<double, 3>> host; // unit, model frame; zero where not captured
    std::vector<uint8_t> captured;           // 1 when the host answered
    size_t failedCalls = 0;

    // The project's place as Archicad reported it.
    double latitude = 0.0, longitude = 0.0, altitude = 0.0;
    double north = 0.0;          // radians, raw
    int timeZoneInMinutes = 0;   // raw
    bool projectSumTime = false; // the project's own flag: recorded, not used
    double projectOffsetHours = 0.0;

    double rotation = 0.0; // the assumed model turn the independent sun used
    evp::sunstudy::rust::SunCrossCheck check;
    // Diagnostic only: the constant azimuth turn (degrees, counter-clockwise)
    // that best aligns host and independent suns, and the spread around it.
    double bestTurnDegrees = 0.0, turnSpreadDegrees = 0.0;
};

// False with `error` when the place cannot be read or a stamp cannot be
// placed. A failed host call for one interval is counted, not fatal; the
// caller decides (a weather run refuses any gap rather than read it as night).
bool CaptureWeatherSun (const WeatherSunRequest& request, WeatherSunCapture& out, std::string& error);

} // namespace geomsrv

#endif
