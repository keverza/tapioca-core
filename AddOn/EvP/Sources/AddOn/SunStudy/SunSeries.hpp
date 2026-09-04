#ifndef EVP_SUNSTUDY_SUNSERIES_HPP
#define EVP_SUNSTUDY_SUNSERIES_HPP

// SunStudy/SunSeries — the day, as a list of sun directions.
//
// Timestep enumeration, the below-horizon filter, and the hours arithmetic. Pure
// data in, pure data out.
//
// ⚠️ IT COMPUTES NO SOLAR POSITION, AND MUST NOT. Archicad's own place settings
// are the authority for where the sun is; a second implementation would be a
// second answer, and the two would disagree for a project whose sun was typed by
// hand rather than derived. The caller samples the host once per timestep and
// hands the vectors in. An independent solar formula is legitimate only as a
// CROSS-CHECK on those vectors, never as a source for them.
//
// Transcribed from private/Commands/SunStudy/sunseries.py, whose tests pin the
// same behaviour on the Python side.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

struct TimeOfDay {
    int hour = 0;
    int minute = 0;
};

// One timestep: when, and where the sun was.
struct SunStep {
    TimeOfDay time;

    // Unit vector pointing TO the sun, in MODEL space — already north-corrected
    // by the host, so nothing here rotates it again.
    double direction[3] = { 0.0, 0.0, 1.0 };

    double altitudeDegrees = 0.0;
};

// (hour, minute) across the day at the given step. `hourTo` is exclusive.
//
// ⚠️ THE DEFAULT IS THE WHOLE DAY and the horizon filter then decides what
// counts, which is the honest default for "how much sun does this surface get".
//
// ⚠️ A WINDOW MAKES TOTALS INCOMPARABLE between tools. Reference implementations
// commonly default to an 08:00–20:00 window, so a whole-day total from here
// reads higher than the same model elsewhere. Whenever a window is set, the
// report has to say so.
std::vector<TimeOfDay> EnumerateTimesteps (int timestepMinutes, int hourFrom = 0, int hourTo = 24);

double HoursPerStep (int timestepMinutes);

// The day's sun directions, above the horizon only.
class SunSeries final {
  public:
    SunSeries () = default;

    // Keeps the steps whose altitude exceeds `minAltitudeDegrees`, in order.
    //
    // ⚠️ THE CUTOFF IS 0 DEGREES BY DEFAULT, AND A HIGHER FLOOR BIASES EVERY
    // TOTAL DOWN. A three-degree floor silently discards the first and last
    // steps of every day — roughly half an hour on an equinox at mid latitude —
    // against references that count a step whenever altitude exceeds zero, with
    // no refraction and no sun-disc term. It stays an input because a grazing
    // sun behind distant terrain is a legitimate thing to exclude; it is simply
    // not the default.
    static SunSeries FromSteps (const std::vector<SunStep>& steps, int timestepMinutes,
                                double minAltitudeDegrees = 0.0);

    size_t StepCount () const
    {
        return steps_.size ();
    }
    bool Empty () const
    {
        return steps_.empty ();
    }
    const SunStep& Step (size_t index) const
    {
        return steps_[index];
    }
    const std::vector<SunStep>& Steps () const
    {
        return steps_;
    }

    int TimestepMinutes () const
    {
        return timestepMinutes_;
    }
    double HoursPerStep () const;

    // What a fully sunlit sample would score. The denominator of every
    // percentage the report prints.
    double DaylightHours () const;

    // How many steps the source had before the horizon filter, so a report can
    // say what it dropped rather than silently showing a shorter day.
    size_t SourceStepCount () const
    {
        return sourceStepCount_;
    }

    // ⚠️ A HASH OF THE CONTENTS, NOT A COUNTER, and that is what makes it
    // impossible to drift. A counter has to be bumped by whoever changes the
    // date, the window, the timestep, the horizon floor or the project's place —
    // five callers, any one of which can forget, and a forgotten bump serves a
    // stale study that looks correct. Hashing what is actually stored cannot be
    // forgotten.
    //
    // Zero for an empty series, so "no sun above the horizon" is distinguishable
    // from "unchanged".
    uint64_t Version () const
    {
        return version_;
    }

  private:
    std::vector<SunStep> steps_;
    int timestepMinutes_ = 60;
    size_t sourceStepCount_ = 0;
    uint64_t version_ = 0;
};

} // namespace evp::sunstudy

#endif
