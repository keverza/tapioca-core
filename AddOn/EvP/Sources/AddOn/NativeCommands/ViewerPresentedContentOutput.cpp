// NativeCommands/ViewerPresentedContentOutput -- see the header.

#include "NativeCommands/ViewerPresentedContentOutput.hpp"

#include "ArchViz/Dxgi/PresentedContent.hpp"

#include <cstdio>
#include <string>

namespace geomsrv {

namespace {

const char* RelationName (geomsrv::archviz::dxgi::presentedcontent::Relation relation)
{
    using Relation = geomsrv::archviz::dxgi::presentedcontent::Relation;
    switch (relation) {
        case Relation::Composited:
            return "COMPOSITED";
        case Relation::SameBuffer:
            return "SAME_BUFFER";
        case Relation::OtherBuffer:
            return "OTHER_BUFFER";
        case Relation::Undecidable:
            return "UNDECIDABLE";
        case Relation::Unidentified:
            return "UNIDENTIFIED";
        default:
            return "UNKNOWN";
    }
}

} // namespace

GS::ObjectState BuildPresentedContentOutput (uint64_t epoch)
{
    namespace presentedcontent = geomsrv::archviz::dxgi::presentedcontent;

    const presentedcontent::Stats stats = presentedcontent::GetStats ();
    const auto text = [] (uint64_t value) { return GS::UniString (std::to_string (value).c_str (), CC_UTF8); };
    const auto real = [] (double value) {
        char buffer[32];
        std::snprintf (buffer, sizeof (buffer), "%.9g", value);
        return GS::UniString (buffer, CC_UTF8);
    };

    GS::ObjectState out;
    out.Add ("enabled", stats.enabled);
    out.Add ("presentsSampled", text (stats.presentsSampled));
    out.Add ("presentsProcessed", text (stats.presentsProcessed));
    out.Add ("slotsBusy", text (stats.slotsBusy));
    out.Add ("readbacksPending", text (stats.readbacksPending));
    out.Add ("readbackFailures", text (stats.readbackFailures));
    out.Add ("createFailures", text (stats.createFailures));
    out.Add ("abandoned", text (stats.abandoned));
    out.Add ("chainBreaks", text (stats.chainBreaks));
    out.Add ("targetChanges", text (stats.targetChanges));
    out.Add ("composited", text (stats.composited));
    out.Add ("compositedChanged", text (stats.compositedChanged));
    out.Add ("repeats", text (stats.repeats));
    out.Add ("repeatSame", text (stats.repeatSame));
    out.Add ("repeatOther", text (stats.repeatOther));
    out.Add ("repeatUndecidable", text (stats.repeatUndecidable));
    out.Add ("repeatUnidentified", text (stats.repeatUnidentified));
    out.Add ("firstRepeats", text (stats.firstRepeats));
    out.Add ("firstRepeatSame", text (stats.firstRepeatSame));
    out.Add ("firstRepeatOther", text (stats.firstRepeatOther));
    out.Add ("firstRepeatUnidentified", text (stats.firstRepeatUnidentified));
    out.Add ("overlayBound", text (stats.overlayBound));
    GS::Array<GS::UniString> trueDeltaOut;
    GS::Array<GS::UniString> screenSecondsOut;
    for (size_t b = 0; b < presentedcontent::kDeltaBuckets; ++b) {
        trueDeltaOut.Push (text (stats.trueDelta[b]));
        screenSecondsOut.Push (real (stats.screenSeconds[b]));
    }
    out.Add ("trueDelta", trueDeltaOut);
    out.Add ("screenSeconds", screenSecondsOut);
    out.Add ("trueDeltaUnknown", text (stats.trueDeltaUnknown));
    out.Add ("screenSecondsUnknown", real (stats.screenSecondsUnknown));
    out.Add ("bookkeepingDisagrees", text (stats.bookkeepingDisagrees));
    out.Add ("framesReady", GS::Int32 (stats.framesReady));

    // Stage 76: what the context hooks saw before each Present, per relation,
    // and which hooked slots the runtime re-pointed during the window.
    GS::Array<GS::ObjectState> coverageOut;
    for (size_t r = 0; r < presentedcontent::kRelations; ++r) {
        const presentedcontent::CoverageStats& coverage = stats.coverage[r];
        GS::ObjectState row;
        row.Add ("relation", GS::UniString (RelationName (presentedcontent::Relation (r)), CC_UTF8));
        row.Add ("presents", text (coverage.presents));
        row.Add ("calls", text (coverage.calls));
        row.Add ("draws", text (coverage.draws));
        row.Add ("noDraws", text (coverage.noDraws));
        row.Add ("repairs", text (coverage.repairs));
        coverageOut.Push (row);
    }
    out.Add ("coverage", coverageOut);
    out.Add ("presentsWithRepairAfterCalls", text (stats.presentsWithRepairAfterCalls));
    GS::Array<GS::ObjectState> slotRepairsOut;
    for (size_t i = 0; i < presentedcontent::kHookSlots; ++i) {
        GS::ObjectState row;
        row.Add ("slot", GS::UniString (presentedcontent::HookSlotName (i), CC_UTF8));
        row.Add ("repairs", text (stats.slotRepairs[i]));
        slotRepairsOut.Push (row);
    }
    out.Add ("slotRepairs", slotRepairsOut);

    presentedcontent::FrameInfo frames[presentedcontent::kFrames];
    const size_t count = presentedcontent::WriteFrames (epoch, frames, presentedcontent::kFrames);
    GS::Array<GS::ObjectState> framesOut;
    for (size_t f = 0; f < count; ++f) {
        const presentedcontent::FrameInfo& frame = frames[f];
        GS::ObjectState frameOut;
        frameOut.Add ("presentSerial", text (frame.presentSerial));
        frameOut.Add ("seconds", real (frame.seconds));
        frameOut.Add ("handoffSerial", text (frame.handoffSerial));
        frameOut.Add ("imageGeneration", text (frame.imageGeneration));
        frameOut.Add ("cameraImageGeneration", text (frame.cameraImageGeneration));
        frameOut.Add ("cameraBound", frame.cameraBound);
        frameOut.Add ("relation", GS::UniString (RelationName (frame.relation), CC_UTF8));
        frameOut.Add ("displayedKnown", frame.displayedKnown);
        frameOut.Add ("displayed", text (frame.displayed));
        frameOut.Add ("x", GS::Int32 (frame.x));
        frameOut.Add ("y", GS::Int32 (frame.y));
        frameOut.Add ("width", GS::Int32 (frame.width));
        frameOut.Add ("height", GS::Int32 (frame.height));
        frameOut.Add ("path", GS::UniString (frame.path.c_str (), CC_UTF8));
        frameOut.Add ("pathBefore", GS::UniString (frame.pathBefore.c_str (), CC_UTF8));
        framesOut.Push (frameOut);
    }
    out.Add ("frames", framesOut);
    return out;
}

} // namespace geomsrv
