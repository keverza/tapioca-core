#include "ArchViz/ViewerStatus.hpp"

#include <mutex>
#include <utility>
#include <vector>

namespace geomsrv::archviz::viewerstatus {

namespace {

std::string Percent (size_t done, size_t total)
{
    return std::to_string (QuantisedPercent (done, total)) + "%";
}

std::string ViewBlock (const ViewFacts& view)
{
    if (!view.known)
        return std::string ();
    if (view.planMode)
        return "Plan";
    return view.orthographic ? "Parallel" : "Perspective";
}

std::string InspectionBlock (const Inputs& in)
{
    if (in.selectedCount == 0)
        return std::string ();
    return std::to_string (in.selectedCount) + " selected";
}

// The study's own words, before any hint is joined to them. `alert` is set for
// what the user has to act on.
std::string AnalysisState (const Inputs& in, bool& alert)
{
    alert = false;
    // ⚠️ A RUN THE USER ASKED FOR OUTRANKS THE FOLLOWER: it is the thing they are
    // waiting on, and while it runs the follower is paused by it anyway.
    if (in.runRunning) {
        if (in.runStage == "waiting")
            return "Sun study: waiting for the model";
        if (in.runStage == "calculating")
            return "Sun study " + Percent (in.runResolved, in.runTotal);
        if (in.runStage == "showing")
            return "Sun study: drawing results";
        return "Sun study: preparing";
    }
    if (in.runStage == "failed") {
        alert = true;
        return in.runError.empty () ? "Sun study failed" : "Sun study failed: " + in.runError;
    }

    switch (in.follow) {
        case FollowState::Starting:
        case FollowState::UpdatingVisible:
            return "Sun study updating " + Percent (in.followResolved, in.followTotal);
        case FollowState::DirtyVisible:
            alert = true;
            return in.followDirtyReason.empty () ? "Sun study results outdated"
                                                 : "Sun study results outdated (" + in.followDirtyReason + ")";
        case FollowState::Failed:
            alert = true;
            return in.followError.empty () ? "Sun study update failed" : "Sun study update failed: " + in.followError;
        case FollowState::Current:
            return "Sun study current";
        case FollowState::NoStudy:
            break;
    }
    return in.studyShown ? "Sun study shown" : std::string ();
}

std::string ModelBlock (const Inputs& in, bool& alert)
{
    alert = false;
    if (in.extractionRunning)
        return in.extractionTotal == 0 ? "Reading model"
                                       : "Reading model " + Percent (in.extractionDone, in.extractionTotal);
    if (in.extractionGaveUp) {
        alert = true;
        return "Model incomplete";
    }
    if (!in.watchError.empty ()) {
        alert = true;
        return "Model sync error";
    }
    return "Model current";
}

// ---- the publish channel ----------------------------------------------------
struct Published {
    std::mutex mutex;
    // Insertion order is recency: the newest hint is the last one.
    std::vector<std::pair<std::string, std::string>> hints;
    ViewFacts view;
};

Published& Channel ()
{
    static Published channel;
    return channel;
}

} // namespace

int QuantisedPercent (size_t done, size_t total)
{
    if (total == 0)
        return 0;
    if (done >= total)
        return 100;
    const int raw = int ((done * 100) / total);
    return raw - raw % kPercentStep;
}

Blocks Compose (const Inputs& in)
{
    Blocks out;
    out.view = ViewBlock (in.view);
    out.inspection = InspectionBlock (in);

    const std::string state = AnalysisState (in, out.analysisAlert);
    // ⚠️ THE HINT GOES FIRST: it is the one thing on the line that asks for an
    // action now, and the analysis block is the only one wide enough for a
    // sentence. The study's state stays beside it rather than being dropped --
    // picking a sun study's centre while another study updates is ordinary.
    if (!in.hint.empty ())
        out.analysis = state.empty () ? "Next: " + in.hint : "Next: " + in.hint + "  |  " + state;
    else
        out.analysis = state;

    out.model = ModelBlock (in, out.modelAlert);
    return out;
}

std::string Join (const Blocks& blocks)
{
    std::string line;
    for (const std::string* part : { &blocks.view, &blocks.inspection, &blocks.analysis, &blocks.model }) {
        if (part->empty ())
            continue;
        if (!line.empty ())
            line += "  |  ";
        line += *part;
    }
    return line;
}

void PublishHint (const std::string& source, const std::string& text)
{
    Published& channel = Channel ();
    std::lock_guard<std::mutex> lock (channel.mutex);
    for (auto it = channel.hints.begin (); it != channel.hints.end (); ++it) {
        if (it->first == source) {
            // Unchanged text keeps its place; a NEW text is the newest hint.
            if (it->second == text)
                return;
            channel.hints.erase (it);
            break;
        }
    }
    if (!text.empty ())
        channel.hints.emplace_back (source, text);
}

std::string CurrentHint ()
{
    Published& channel = Channel ();
    std::lock_guard<std::mutex> lock (channel.mutex);
    return channel.hints.empty () ? std::string () : channel.hints.back ().second;
}

void PublishView (const ViewFacts& view)
{
    Published& channel = Channel ();
    std::lock_guard<std::mutex> lock (channel.mutex);
    channel.view = view;
}

ViewFacts CurrentView ()
{
    Published& channel = Channel ();
    std::lock_guard<std::mutex> lock (channel.mutex);
    return channel.view;
}

void ResetPublished ()
{
    Published& channel = Channel ();
    std::lock_guard<std::mutex> lock (channel.mutex);
    channel.hints.clear ();
    channel.view = ViewFacts ();
}

} // namespace geomsrv::archviz::viewerstatus
