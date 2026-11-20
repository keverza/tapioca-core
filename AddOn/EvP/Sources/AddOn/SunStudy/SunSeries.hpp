#ifndef EVP_SUNSTUDY_SUNSERIES_HPP
#define EVP_SUNSTUDY_SUNSERIES_HPP

// SunStudy/SunSeries — the day, as a list of sun directions: the value type.
//
// Timestep enumeration, the below-horizon filter and the content hash run in
// the Rust core (tapioca_sunstudy::series), reached through
// SunStudy/SunStudyKernels.hpp (kernels::EnumerateTimesteps,
// kernels::SeriesFromSteps). This header keeps the value and its hours arithmetic.
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

inline double HoursPerStep (int timestepMinutes)
{
    return static_cast<double> (timestepMinutes) / 60.0;
}

// The day's sun directions, above the horizon only.
class SunSeries final {
  public:
    SunSeries () = default;

    // A series built by the Rust kernel (kernels::SeriesFromSteps), restored as
    // it was built:
    // the kept steps, the source count and the content hash, not recomputed.
    static SunSeries FromParts (std::vector<SunStep> steps, int timestepMinutes, size_t sourceStepCount,
                                uint64_t version)
    {
        SunSeries series;
        series.steps_ = std::move (steps);
        series.timestepMinutes_ = timestepMinutes;
        series.sourceStepCount_ = sourceStepCount;
        series.version_ = version;
        return series;
    }

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
    double HoursPerStep () const
    {
        return evp::sunstudy::HoursPerStep (timestepMinutes_);
    }

    // What a fully sunlit sample would score. The denominator of every
    // percentage the report prints.
    double DaylightHours () const
    {
        return static_cast<double> (steps_.size ()) * HoursPerStep ();
    }

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
