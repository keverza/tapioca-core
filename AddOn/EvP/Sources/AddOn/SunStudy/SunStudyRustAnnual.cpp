#include "SunStudy/SunStudyRustAnnual.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"

#include <algorithm>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (void*) != 8 || sizeof (TapiocaAnnualDayRequest) == 16);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAnnualValuesRequest) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAnnualInfo) == 272);
static_assert (sizeof (TapiocaAnnualScheduleRequest) == 12);
static_assert (sizeof (TapiocaAnnualScheduledDay) == 16);

namespace {

bool Month (int month, bool allowWhole, uint8_t& out, std::string& error)
{
    if (month < (allowWhole ? 0 : 1) || month > 12) {
        error = "AnnualInvalidMonth";
        return false;
    }
    out = static_cast<uint8_t> (month);
    return true;
}

} // namespace

bool ScheduleAnnualDays (int year, int fromMonth, int fromDay, int toMonth, int toDay, AnnualSampling sampling,
                         std::vector<AnnualDay>& days, uint32_t& representedDays, std::string& error)
{
    days.clear ();
    representedDays = 0;
    const auto narrow = [] (int value) { return static_cast<uint8_t> (value < 0 || value > 255 ? 0 : value); };
    TapiocaAnnualScheduleRequest request {};
    request.year = year;
    request.from[0] = narrow (fromMonth);
    request.from[1] = narrow (fromDay);
    request.to[0] = narrow (toMonth);
    request.to[1] = narrow (toDay);
    request.sampling = static_cast<uint8_t> (sampling);
    std::vector<TapiocaAnnualScheduledDay> planned;
    size_t count = 0;
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_annual_schedule (&request, planned.data (), planned.size (), &count, &representedDays,
                                                 m, n);
    };
    if (!Invoke (read, error)) // shape
        return false;
    planned.assign (count, TapiocaAnnualScheduledDay {});
    if (!planned.empty () && !Invoke (read, error)) // fill
        return false;
    for (const auto& day : planned)
        days.push_back ({ day.year, day.month, day.day, day.weightDays });
    return true;
}

RustAnnualSun::RustAnnualSun (TapiocaSunStudyAnnual* handle) : handle_ (handle)
{
}

RustAnnualSun::~RustAnnualSun ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_annual_destroy (handle_, m, n); }, ignored);
}

std::unique_ptr<RustAnnualSun> RustAnnualSun::Create (size_t sampleCount, std::string& error)
{
    TapiocaSunStudyAnnual* handle = nullptr;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_annual_create (sampleCount, &handle, m, n); },
                 error))
        return nullptr;
    return std::unique_ptr<RustAnnualSun> (new RustAnnualSun (handle));
}

bool RustAnnualSun::Add (const RustStudyCore& day, int month, double weightDays, std::string& error)
{
    TapiocaAnnualDayRequest request {};
    request.weightDays = weightDays;
    if (!Month (month, false, request.month, error))
        return false;
    return Invoke (
        [&] (char* m, size_t n) { return tapioca_sunstudy_annual_add_day (handle_, day.handle_, &request, m, n); },
        error);
}

bool RustAnnualSun::AddSunless (int month, double weightDays, std::string& error)
{
    TapiocaAnnualDayRequest request {};
    request.weightDays = weightDays;
    if (!Month (month, false, request.month, error))
        return false;
    return Invoke (
        [&] (char* m, size_t n) { return tapioca_sunstudy_annual_add_day (handle_, nullptr, &request, m, n); }, error);
}

bool RustAnnualSun::Info (AnnualInfo& info, std::string& error) const
{
    TapiocaAnnualInfo out {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_annual_info (handle_, &out, m, n); }, error))
        return false;
    info = AnnualInfo ();
    info.sampleCount = out.sampleCount;
    info.sampledDays = out.sampledDays;
    info.representedDays = out.representedDays;
    std::copy (out.siteDaylight, out.siteDaylight + 12, info.siteDaylight.begin ());
    std::copy (out.monthDays, out.monthDays + 12, info.monthDays.begin ());
    info.minHours = out.unoccluded.minHours;
    info.meanHours = out.unoccluded.meanHours;
    info.maxHours = out.unoccluded.maxHours;
    info.fullyLit = out.unoccluded.fullyLit;
    info.fullyShaded = out.unoccluded.fullyShaded;
    info.ceilingHours = out.unoccluded.daylightHours;
    return true;
}

bool RustAnnualSun::Values (AnnualQuantity quantity, int month, bool perDay, std::vector<double>& values,
                            double& ceiling, std::string& error) const
{
    TapiocaAnnualValuesRequest request {};
    request.quantity = static_cast<uint8_t> (quantity);
    request.perDay = perDay ? 1 : 0;
    if (!Month (month, true, request.month, error))
        return false;
    size_t count = 0;
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_annual_values (handle_, &request, &count, &ceiling, m, n);
    };
    if (!Invoke (read, error)) // shape
        return false;
    values.assign (count, 0.0);
    request.values = values.data ();
    request.capacity = values.size ();
    return values.empty () || Invoke (read, error); // fill
}

bool RustAnnualSun::Present (const RustStudyCore& carrier, AnnualQuantity quantity, int month, bool perDay,
                             std::string& error) const
{
    uint8_t selected = 0;
    if (!Month (month, true, selected, error))
        return false;
    return Invoke (
        [&] (char* m, size_t n) {
            return tapioca_sunstudy_annual_present (handle_, carrier.handle_, static_cast<uint8_t> (quantity), selected,
                                                    perDay ? 1 : 0, m, n);
        },
        error);
}

} // namespace evp::sunstudy::rust
