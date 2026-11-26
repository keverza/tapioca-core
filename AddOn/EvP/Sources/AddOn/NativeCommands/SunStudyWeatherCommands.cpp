#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "NativeCommands/SunStudyWeatherCommands.hpp"
#include "NativeCommands/SunStudyWeatherSun.hpp"
#include "SunStudy/SunStudyWeather.hpp"

#include <filesystem>
#include <vector>

namespace geomsrv {

namespace {

using sunstudysupport::Text;

// ---------------------------------------------------------------------------
// Tapioca.CheckWeatherSun
// ---------------------------------------------------------------------------
class CheckWeatherSunCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CheckWeatherSun";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string path = sunstudysupport::ReadString (params, "weatherFile", "");
        if (path.empty ())
            return NativeCommandResult::Failure ("weatherFile is required: an .epw file, or a .zip holding one");
        evp::sunstudy::WeatherData weather;
        std::string error;
        if (!evp::sunstudy::ReadWeatherFile (std::filesystem::u8path (path), weather, error))
            return NativeCommandResult::Failure (Text (error));

        WeatherSunRequest request;
        request.year = sunstudysupport::ReadInt (params, "year", 2026);
        request.recordsPerHour = static_cast<int> (weather.identity.recordsPerHour);
        request.stationOffsetHours = weather.identity.timeZoneHours;
        request.stamps.reserve (weather.records.size ());
        for (const auto& r : weather.records)
            request.stamps.push_back ({ r.month, r.day, r.hour, r.minute });
        // A leap-day record cannot be placed in a common year: drop it, say so.
        size_t leapDays = 0;
        const bool leapYear = request.year % 4 == 0 && (request.year % 100 != 0 || request.year % 400 == 0);
        if (!leapYear) {
            const auto before = request.stamps.size ();
            std::erase_if (request.stamps, [] (const auto& s) { return s.month == 2 && s.day == 29; });
            leapDays = before - request.stamps.size ();
        }

        WeatherSunCapture capture;
        if (!CaptureWeatherSun (request, capture, error))
            return NativeCommandResult::Failure (Text (error));

        GS::ObjectState os;
        os.Add ("station", Text (weather.identity.city + ", " + weather.identity.country));
        os.Add ("sha256", Text (weather.identity.sha256));
        os.Add ("records", (GS::Int32) request.stamps.size ());
        os.Add ("leapDayRecordsDropped", (GS::Int32) leapDays);
        os.Add ("captured", (GS::Int32) (request.stamps.size () - capture.failedCalls));
        os.Add ("failedSunCalls", (GS::Int32) capture.failedCalls);
        os.Add ("compared", (GS::Int32) capture.check.compared);
        os.Add ("meanDeg", capture.check.meanDegrees);
        os.Add ("maxDeg", capture.check.maxDegrees);
        os.Add ("accepted", capture.check.accepted && capture.failedCalls == 0);
        os.Add ("bestTurnDeg", capture.bestTurnDegrees);
        os.Add ("turnSpreadDeg", capture.turnSpreadDegrees);
        os.Add ("latitude", capture.latitude);
        os.Add ("longitude", capture.longitude);
        os.Add ("altitude", capture.altitude);
        os.Add ("northRad", capture.north);
        os.Add ("timeZoneInMinutes", (GS::Int32) capture.timeZoneInMinutes);
        os.Add ("projectSumTime", capture.projectSumTime);
        os.Add ("stationOffsetHours", weather.identity.timeZoneHours);
        os.Add ("assumedRotationRad", capture.rotation);
        return os;
    }
};

const NativeCommandRegistration kSunStudyWeatherRegistrations[] = {
    { "CheckWeatherSun", &MakeRegisteredNativeCommand<CheckWeatherSunCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "weatherFile":{"type":"string"},
                "year":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["weatherFile"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "station":{"type":"string"},
                "sha256":{"type":"string"},
                "records":{"type":"integer"},
                "leapDayRecordsDropped":{"type":"integer"},
                "captured":{"type":"integer"},
                "failedSunCalls":{"type":"integer"},
                "compared":{"type":"integer"},
                "meanDeg":{"type":"number"},
                "maxDeg":{"type":"number"},
                "accepted":{"type":"boolean"},
                "bestTurnDeg":{"type":"number"},
                "turnSpreadDeg":{"type":"number"},
                "latitude":{"type":"number"},
                "longitude":{"type":"number"},
                "altitude":{"type":"number"},
                "northRad":{"type":"number"},
                "timeZoneInMinutes":{"type":"integer"},
                "projectSumTime":{"type":"boolean"},
                "stationOffsetHours":{"type":"number"},
                "assumedRotationRad":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["records","compared","meanDeg","maxDeg","accepted"]
        })json" },
};

} // namespace

NativeCommandRegistrations GetSunStudyWeatherCommandRegistrations ()
{
    return MakeRegistrationView (kSunStudyWeatherRegistrations);
}

} // namespace geomsrv
