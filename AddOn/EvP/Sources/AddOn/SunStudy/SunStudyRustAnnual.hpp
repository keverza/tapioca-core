#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTANNUAL_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTANNUAL_HPP

// SunStudy/SunStudyRustAnnual -- a period's direct sun, one traced day at a
// time (#152, annual stage).
//
// Each scheduled day is an ordinary converged day study on the SAME pinned
// snapshot and samples; Add folds its hours into per-sample monthly totals
// and the day study can then be erased. No step mask is kept, so memory is
// 192 bytes a sample whatever the period -- unless the run asks to keep its
// days for playback (`keepDays`): then every day's lit bits stay, about 11
// bytes a sample and day at 15-minute steps. Present draws a selection of the
// totals through one day study's samples and atlas, which is how the viewer
// shows a period without a second display path.
//
// Quantities (per sample and month, one schedule):
//   0 <= unoccluded <= surface facing <= site daylight
// Obstruction loss = facing - unoccluded; back-facing time is never loss.
//
// One thread at a time: the runner that owns it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TapiocaSunStudyAnnual;

namespace evp::sunstudy::rust {

class RustStudyCore;

enum class AnnualQuantity : uint8_t {
    Unoccluded = 0,
    SurfaceFacing = 1,
    ObstructionLoss = 2,
    AccessRatio = 3, // unoccluded / facing, 0-1
    DaysAtLeast = 4, // days with at least AnnualSampleRow::edges[threshold] hours; whole period only
};

// One sample's row for a tooltip or a graph.
struct AnnualSampleRow {
    std::array<double, 12> unoccluded {}, facing {}; // hours per month
    std::array<double, 13> edges {};                 // daily-hours bin edges
    std::array<float, 14> daily {};                  // represented days per bin
};

struct AnnualInfo {
    size_t sampleCount = 0;
    size_t sampledDays = 0;
    double representedDays = 0.0;
    std::array<double, 12> siteDaylight {}; // hours per month
    std::array<double, 12> monthDays {};    // represented days per month
    double minHours = 0.0, meanHours = 0.0, maxHours = 0.0;
    size_t fullyLit = 0, fullyShaded = 0;
    double ceilingHours = 0.0; // the period's site daylight
};

struct AnnualDay {
    int year = 0, month = 0, day = 0;
    double weightDays = 0.0; // calendar days it stands for, in its own month
};

// A day kept for playback: its date and each kept step's clock minute.
struct ArchivedDay {
    int month = 0, day = 0;
    double weightDays = 0.0;
    double hoursPerStep = 0.0; // 0 for a sunless day
    std::vector<uint16_t> minutes;
};

enum class AnnualSampling : uint8_t {
    Preview = 0,       // one middle day per covered month: an estimate
    DailyResolved = 1, // every covered day
};

// The days a period traces, in date order. `fromMonth/fromDay` to
// `toMonth/toDay` inclusive, within `year`. `representedDays` is the period's
// calendar-day count.
bool ScheduleAnnualDays (int year, int fromMonth, int fromDay, int toMonth, int toDay, AnnualSampling sampling,
                         std::vector<AnnualDay>& days, uint32_t& representedDays, std::string& error);

class RustAnnualSun final {
  public:
    static std::unique_ptr<RustAnnualSun> Create (size_t sampleCount, std::string& error, bool keepDays = false);
    ~RustAnnualSun ();
    RustAnnualSun (const RustAnnualSun&) = delete;
    RustAnnualSun& operator= (const RustAnnualSun&) = delete;

    // A converged day study's hours, weighted by the calendar days it stands
    // for. Refused (and nothing changed) for other samples, an unconverged or
    // already-presented study, or a weight that is not a positive day count.
    // `dayOfMonth` is kept with the day when the handle keeps days.
    bool Add (const RustStudyCore& day, int month, double weightDays, std::string& error, int dayOfMonth = 0);
    // A day with no sun above the floor: its days count, at zero hours.
    bool AddSunless (int month, double weightDays, std::string& error, int dayOfMonth = 0);

    bool Info (AnnualInfo& info, std::string& error) const;
    // One value per sample; `month` 0 is the whole period. `perDay` divides
    // by the selection's represented days: the mean hours a day, which the
    // day study's colour bands (hours of one day) can show. `ceiling` is the
    // site daylight of the same selection, on the same scale.
    // AccessRatio ignores `perDay`; DaysAtLeast reads `threshold` and with
    // `perDay` gives the share of represented days.
    bool Values (AnnualQuantity quantity, int month, bool perDay, std::vector<double>& values, double& ceiling,
                 std::string& error, int threshold = 0) const;
    // `scale` multiplies every value and the ceiling: 10 draws a 0-1 ratio on
    // the 0-10 h ramp the colour bands use.
    bool Present (const RustStudyCore& carrier, AnnualQuantity quantity, int month, bool perDay, std::string& error,
                  int threshold = 0, double scale = 1.0) const;
    // False with an empty error past the last sample.
    bool Sample (size_t sample, AnnualSampleRow& row, std::string& error) const;

    // Playback (created with keepDays): the kept days, and one drawn through
    // the carrier -- `step` >= 0 that step lit or not (lit at `scale`), `step`
    // -1 the day's `quantity` (not DaysAtLeast) against its daylight.
    bool ArchivedDays (std::vector<ArchivedDay>& days, size_t& bytes, std::string& error) const;
    bool PresentDay (const RustStudyCore& carrier, size_t day, int step, AnnualQuantity quantity, double scale,
                     std::string& error) const;

  private:
    explicit RustAnnualSun (TapiocaSunStudyAnnual* handle);
    TapiocaSunStudyAnnual* handle_ = nullptr;
};

} // namespace evp::sunstudy::rust

#endif
