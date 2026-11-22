#include "ArchViz/TemporalConvergence.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {

float TentOffsetPixels (float u, float radius)
{
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    if (!(radius > 0.5f))
        return u - 0.5f;
    return u < 0.5f ? radius * (std::sqrt (2.0f * u) - 1.0f) : radius * (1.0f - std::sqrt (2.0f - 2.0f * u));
}

const char* TemporalPhaseName (TemporalPhase phase)
{
    switch (phase) {
        case TemporalPhase::Off:
            return "off";
        case TemporalPhase::Moving:
            return "moving";
        case TemporalPhase::Settling:
            return "settling";
        case TemporalPhase::Accumulating:
            return "accumulating";
        case TemporalPhase::Converged:
            return "converged";
    }
    return "?";
}

const char* TemporalRestartName (TemporalRestart reason)
{
    switch (reason) {
        case TemporalRestart::None:
            return "none";
        case TemporalRestart::Camera:
            return "camera";
        case TemporalRestart::Content:
            return "content";
        case TemporalRestart::Unresolved:
            return "unresolved";
        case TemporalRestart::Reset:
            return "reset";
    }
    return "?";
}

void ContentKey::Add (const void* data, size_t bytes)
{
    const unsigned char* p = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < bytes; ++i) {
        hash_ ^= p[i];
        hash_ *= 1099511628211ull;
    }
}

void ContentKey::Add (float value)
{
    // ⚠️ +0 AND -0 ARE ONE VALUE. A setting that toggles the sign of a zero (a negated
    // offset, a slider dragged back to its origin) must not read as a change.
    if (value == 0.0f)
        value = 0.0f;
    Add (&value, sizeof (value));
}

void ContentKey::Add (uint64_t value)
{
    Add (&value, sizeof (value));
}

std::string TemporalActivityLog::Note (const TemporalStatus& status, bool idle, double nowSeconds)
{
    if (!haveStart_ || started_ < 0.0) {
        started_ = nowSeconds;
        for (int i = 0; i < 5; ++i)
            restartsAtStart_[i] = status.restartsBy[i];
        haveStart_ = true;
    }
    ++frames_;
    ++phaseFrames_[int (status.phase)];
    if (idle)
        ++idleFrames_;
    if (nowSeconds - started_ < window_)
        return {};

    std::string line;
    if (phaseFrames_[int (TemporalPhase::Off)] < frames_) {
        uint32_t by[5];
        for (int i = 0; i < 5; ++i)
            by[i] = status.restartsBy[i] - restartsAtStart_[i];
        char text[320];
        std::snprintf (text, sizeof (text),
                       "still image: %u frames over %.1f s -- moving %u, settling %u, accumulating %u, converged %u "
                       "(idle %u), off %u; still image restarted by camera %u, content %u, unresolved %u, reset %u; "
                       "now %s %u/%u",
                       unsigned (frames_), nowSeconds - started_, unsigned (phaseFrames_[1]),
                       unsigned (phaseFrames_[2]), unsigned (phaseFrames_[3]), unsigned (phaseFrames_[4]),
                       unsigned (idleFrames_), unsigned (phaseFrames_[0]), unsigned (by[1]), unsigned (by[2]),
                       unsigned (by[3]), unsigned (by[4]), TemporalPhaseName (status.phase), unsigned (status.samples),
                       unsigned (status.targetSamples));
        line = text;
    }
    started_ = nowSeconds;
    frames_ = 0;
    idleFrames_ = 0;
    for (int i = 0; i < 5; ++i) {
        phaseFrames_[i] = 0;
        restartsAtStart_[i] = status.restartsBy[i];
    }
    return line;
}

void TemporalConvergence::Restart (TemporalPhase phase, TemporalRestart reason)
{
    // Only an interrupted still image counts: Moving -> Moving is the camera still moving.
    if (phase == TemporalPhase::Moving && reason != TemporalRestart::None &&
        (phase_ == TemporalPhase::Settling || phase_ == TemporalPhase::Accumulating ||
         phase_ == TemporalPhase::Converged)) {
        ++restarts_;
        ++restartsBy_[int (reason)];
        lastRestart_ = reason;
    }
    phase_ = phase;
    stillFrames_ = 0;
    samples_ = 0;
}

void TemporalConvergence::Reset (TemporalRestart reason)
{
    haveCamera_ = false;
    haveContent_ = false;
    Restart (phase_ == TemporalPhase::Off ? TemporalPhase::Off : TemporalPhase::Moving, reason);
}

void TemporalConvergence::BeginFrame (const float view[16], const float projection[16], uint32_t width, uint32_t height,
                                      bool running, const TemporalConvergenceSettings& settings)
{
    settings_ = settings;
    if (!running || !settings.enabled || settings.targetSamples == 0 || view == nullptr || projection == nullptr) {
        haveCamera_ = false;
        haveContent_ = false;
        Restart (TemporalPhase::Off, TemporalRestart::None);
        return;
    }

    // ⚠️ BITWISE, NOT WITH A TOLERANCE. A still camera produces the same floats every frame
    // (the camera state is not re-derived from anything that drifts), and any real move, however
    // small, changes what TAA sees and must restart the mean.
    ContentKey camera;
    camera.Add (view, sizeof (float) * 16);
    camera.Add (projection, sizeof (float) * 16);
    camera.Add (uint64_t (width) << 32 | height);
    cameraMovedThisFrame_ = !haveCamera_ || camera.Value () != cameraKey_;
    cameraKey_ = camera.Value ();
    haveCamera_ = true;

    // ⚠️ RESTARTED HERE, NOT ONLY IN EndFrame, because the jitter for THIS frame is chosen
    // between the two calls. A converged view whose camera starts moving must be Moving -- and so
    // unjittered (JitterThisFrame, #14) -- on that very frame, not one frame later.
    if (cameraMovedThisFrame_ || phase_ == TemporalPhase::Off)
        Restart (TemporalPhase::Moving, cameraMovedThisFrame_ ? TemporalRestart::Camera : TemporalRestart::None);
}

TemporalStep TemporalConvergence::EndFrame (uint64_t contentKey, bool resolved)
{
    if (phase_ == TemporalPhase::Off)
        return {};

    const bool contentChanged = !haveContent_ || contentKey != contentKey_;
    contentKey_ = contentKey;
    haveContent_ = true;

    if (!resolved || cameraMovedThisFrame_ || contentChanged) {
        Restart (TemporalPhase::Moving, cameraMovedThisFrame_ ? TemporalRestart::Camera
                                        : contentChanged      ? TemporalRestart::Content
                                                              : TemporalRestart::Unresolved);
        return {};
    }
    if (phase_ == TemporalPhase::Converged)
        return { false, 0.0f, true };

    ++stillFrames_;
    if (stillFrames_ <= settings_.settleFrames) {
        phase_ = TemporalPhase::Settling;
        return {};
    }

    ++samples_;
    TemporalStep step;
    step.accumulate = true;
    step.weight = 1.0f / float (samples_);
    step.present = true;
    phase_ = samples_ >= settings_.targetSamples ? TemporalPhase::Converged : TemporalPhase::Accumulating;
    return step;
}

} // namespace archviz
} // namespace geomsrv
