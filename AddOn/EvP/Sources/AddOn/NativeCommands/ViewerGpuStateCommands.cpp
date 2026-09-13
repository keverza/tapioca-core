// NativeCommands/ViewerGpuStateCommands -- see the header for the boundary this
// file exists to keep. Extracted OUT of ViewerSyncCommands.cpp (2026-09-13) when
// the auto-orbit verb pushed that file past the size cap; the seam was already
// there, because nothing in here is part of how the viewer follows Archicad.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerGpuStateCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/AutoOrbit.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/DeviceIdentity.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"
#include "ArchViz/PatchProfile.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <string>

namespace geomsrv {

namespace av = geomsrv::archviz;

namespace {

// ---- the patch profile (PLAT-RE153, stage 1a) -------------------------------
//
// ⚠️ THIS IS THE ONLY WAY TO PIN A BUILD, AND IT IS DELIBERATELY A DECISION
// SOMEBODY MAKES. See ArchViz/PatchProfile.hpp: the GPU-state hooks read
// Archicad's own GPU buffers, whose layout moves with an Archicad update, so
// they refuse to install on anything but a build that was explicitly pinned
// after being tested. There is no compiled-in table of blessed hashes because
// there could not be an honest one -- this add-on is built on a machine that has
// no idea which Archicad the user runs.
//
// Called with no arguments it REPORTS: what is running, what is pinned, and
// where the file is. `pin: true` records the running build as the pinned one.
class ViewerPatchProfileCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerPatchProfile"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool pin = false;
        params.Get ("pin", pin);

        GS::ObjectState os;
        if (pin) {
            // ⚠️ THE TARGETS ARE FINGERPRINTED FIRST. A profile written without
            // them would verify the executable and check nothing about the slots
            // being patched -- `patchprofile::Pin` refuses that outright, and
            // this is where the discovery that prevents it happens.
            std::string error;
            if (!av::dxgi::FingerprintContextTargets (error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                              "reading Archicad's own D3D11 context vtable to fingerprint "
                              "the hook targets"));
            if (!av::patchprofile::Pin (error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                              "writing the patch profile"));
            // ⚠️ PINNING IS THE THING THAT CHANGES THE ANSWER, so it clears the
            // install latch. Without this the camera tick would keep returning
            // the refusal it cached before the profile existed, and the only way
            // to pick up a fresh pin would be to disarm and arm again -- which
            // reads as "pinning did not work".
            av::dxgi::RetryContextHookInstall ();
        }

        const auto& current = av::patchprofile::Current ();
        os.Add ("path", GS::UniString (av::patchprofile::PinFilePath ().c_str (), CC_UTF8));
        os.Add ("pinned", av::patchprofile::HasPin ());
        os.Add ("pinnedSummary",
                GS::UniString (av::patchprofile::PinnedSummary ().c_str (), CC_UTF8));
        os.Add ("currentSummary",
                GS::UniString (av::patchprofile::CurrentSummary ().c_str (), CC_UTF8));
        os.Add ("hostPath", GS::UniString (current.hostPath.c_str (), CC_UTF8));
        os.Add ("hostVersion", GS::UniString (current.hostVersion.c_str (), CC_UTF8));
        os.Add ("hostSha256", GS::UniString (current.hostSha256.c_str (), CC_UTF8));
        os.Add ("targets", (GS::Int32) current.targets.size ());

        // ⚠️ THE VERDICT IS REPORTED EVEN WHEN NOTHING IS ARMED, because "why is
        // it not syncing" has to be answerable on a machine nobody can attach a
        // debugger to, before anyone tries to arm anything.
        std::string verifyError;
        os.Add ("verifies", av::patchprofile::Verify (verifyError));
        os.Add ("verifyError", GS::UniString (verifyError.c_str (), CC_UTF8));
        return os;
    }
};

// ---- the GPU-state discovery slots (PLAT-RE153, stage 1) --------------------
//
// ⚠️ WITHOUT THIS COMMAND STAGE 3 CAN NEVER SCORE ANYTHING. The three slots that
// carry constant-buffer CONTENTS -- Map, Unmap and UpdateSubresource -- default
// to OFF at every arm, because reading a mapped upload buffer is an uncached
// read of write-combined memory on Archicad's render thread and PLAT-RE118
// measured that thread to be sensitive to added work. The cheap slots answer
// stages 1 and 2 on their own; stage 3 needs the expensive ones, and turning
// them on has to be a deliberate act taken AFTER the frame clock has been
// checked without them.
//
// ⚠️ THE DEFAULTS ARE REAPPLIED AT EVERY ARM, so this is something a run does
// AFTER `SetCameraSyncMode`, never before. Reporting the live state rather than
// the intent is what makes that safe to get wrong.
class ViewerGpuStateSlotsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuStateSlots"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        // The shorthand, and the one a run actually wants: the three slots that
        // capture buffer contents, named for what they do rather than for three
        // D3D method names a caller should not have to know.
        bool constantBuffers = false;
        if (params.Get ("constantBuffers", constantBuffers)) {
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::Map, constantBuffers);
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::Unmap, constantBuffers);
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::UpdateSubresource,
                                             constantBuffers);
        }

        // One slot by name, for isolating a cost to a single hook -- the header's
        // "enable one at a time" advice, made reachable.
        GS::UniString slotName;
        if (params.Get ("slot", slotName)) {
            bool enabled = true;
            params.Get ("enabled", enabled);
            const std::string wanted (slotName.ToCStr (0, MaxUSize, CC_UTF8).Get ());
            bool matched = false;
            for (uint32_t i = 0; i < uint32_t (av::dxgi::ContextSlot::Count); ++i) {
                const av::dxgi::ContextSlot slot = av::dxgi::ContextSlot (i);
                if (wanted == av::dxgi::ContextSlotName (slot)) {
                    av::dxgi::SetContextSlotEnabled (slot, enabled);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return NativeCommandResult::Failure (
                    EVP_FAIL ("unknown GPU-state slot '" + slotName +
                                  "'; ask with no arguments to list them",
                              "gating a GPU-state discovery slot"));
        }

        const auto stats = av::dxgi::GetContextHookStats ();
        GS::ObjectState os;
        os.Add ("installed", stats.installed);
        GS::Array<GS::ObjectState> slots;
        for (uint32_t i = 0; i < uint32_t (av::dxgi::ContextSlot::Count); ++i) {
            const av::dxgi::ContextSlot slot = av::dxgi::ContextSlot (i);
            GS::ObjectState row;
            row.Add ("name", GS::UniString (av::dxgi::ContextSlotName (slot), CC_UTF8));
            row.Add ("enabled", av::dxgi::ContextSlotEnabled (slot));
            row.Add ("calls", (GS::Int32) stats.perSlot[i]);
            slots.Push (row);
        }
        os.Add ("slots", slots);
        return os;
    }
};

// ---- the scored constant-buffer candidates (PLAT-RE155, stage 3) ------------
//
// ⚠️ THE COUNTERS ALONE CANNOT ANSWER STAGE 3. `CameraSyncModeState` reports the
// best candidate's pixel error, which says whether SOMETHING matched; it cannot
// say whether the thing that matched behaves like a camera. That takes the row:
// which buffer, at what offset, under which storage convention, and -- the
// discriminator the handoff actually asks for -- how often those 64 bytes
// changed while the view was moving against while it was still. A region with a
// good error and a high `changesWhileStill` is not a view matrix; it is a buffer
// that happens to hold sixteen agreeable floats this frame.
class ViewerGpuStateCandidatesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuStateCandidates"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 limit = 8;
        params.Get ("limit", limit);
        if (limit < 1)
            limit = 1;
        if (limit > 16)
            limit = 16;

        av::dxgi::viewmatrix::Candidate found[16];
        const size_t count = av::dxgi::viewmatrix::Classify (found, size_t (limit));

        GS::ObjectState os;
        // ⚠️ `scored` FALSE IS THE COMMON CASE AND IS NOT A FAULT. Classification
        // only runs on a SETTLED view -- the reference camera is stale by
        // construction while the view moves, and scoring against it mid-drag
        // would penalise a candidate for being fresher than the reference, which
        // is the entire property being looked for. A caller that asks during a
        // drag gets nothing and should hold still and ask again.
        os.Add ("scored", count > 0);
        os.Add ("referenceValid", av::dxgi::viewmatrix::HasReference ());

        // Spelled out rather than reported as the raw enum: "1" means nothing to
        // somebody reading a log a month later, and the transpose being the
        // EXPECTED hit is the most useful thing a reader can be told here.
        // 0..3 are one captured block; 4..7 are the PRODUCT of an affine block
        // and a projective one, which is the only form a renderer that uploads
        // view and projection separately can be found in.
        static const char* const kVariants[] = {"as-stored", "transposed",
                                                "inverse", "inverse-transposed",
                                                "view x proj", "viewT x proj",
                                                "view x projT", "viewT x projT",
                                                "captured-view x OURproj",
                                                "captured-viewT x OURproj",
                                                "OURview x captured-proj",
                                                "OURview x captured-projT"};
        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            const av::dxgi::viewmatrix::Candidate& candidate = found[i];
            GS::ObjectState row;
            // As a STRING: a buffer pointer does not fit an Int32, and the value
            // is only ever compared for equality across rows and runs.
            row.Add ("buffer",
                     GS::UniString (std::to_string (candidate.buffer).c_str (), CC_UTF8));
            row.Add ("byteOffset", (GS::Int32) candidate.byteOffset);
            // Which window of a ring it came from; 0 for a buffer that is its
            // own window. `byteOffset` already includes it.
            row.Add ("windowOffset", (GS::Int32) candidate.windowOffset);
            // Only meaningful for a product variant: where the projective half
            // came from. `byteOffset` is the affine half.
            row.Add ("pairedOffset", (GS::Int32) candidate.pairedOffset);
            row.Add ("byteWidth", (GS::Int32) candidate.byteWidth);
            row.Add ("variant", GS::UniString (kVariants[candidate.variant % 12], CC_UTF8));
            const char* stage = (candidate.shaderStage < uint32_t (av::dxgi::ContextSlot::Count))
                ? av::dxgi::ContextSlotName (av::dxgi::ContextSlot (candidate.shaderStage))
                : "(never seen bound)";
            row.Add ("boundBy", GS::UniString (stage, CC_UTF8));
            row.Add ("bindSlot", (GS::Int32) candidate.bindSlot);
            row.Add ("maxPixelError", candidate.maxPixelError);
            row.Add ("meanPixelError", candidate.meanPixelError);
            row.Add ("changesWhileMoving", (GS::Int32) candidate.changesWhileMoving);
            row.Add ("changesWhileStill", (GS::Int32) candidate.changesWhileStill);
            rows.Push (row);
        }
        os.Add ("candidates", rows);
        return os;
    }
};

// ---- which GPU API is Archicad's 3D window actually drawn with? -------------
//
// ⚠️ THIS COMMAND EXISTS BECAUSE THE HOOK WORKED AND SAW NOTHING. On 2026-09-13
// the context hook installed on Archicad's own immediate context, correctly and
// exclusively, and then recorded 53 viewport sets and 46 target binds across
// 2722 Archicad frames -- roughly two a second while the user orbited at 100 fps.
// A 3D scene pass does not look like that. Something else is drawing the model.
//
// Before reverse-engineering anything, two cheap in-process facts settle where
// to look, and this reports both:
//
//   * `is11On12` -- a D3D11On12 device means the real renderer is D3D12 and the
//     immediate context is a presentation shim. No amount of
//     `ID3D11DeviceContext` hooking will ever see a view matrix, because the
//     transform is going out through `ID3D12GraphicsCommandList` instead. That
//     moves the whole rung, and it is the single most valuable bit here.
//   * the swap-chain inventory -- the rival hypothesis is that we nominated the
//     wrong chain. `busiestSwapChain` reports a winner and hides the field;
//     this prints the field, with the window each one presents into.
//
// The draw counters in `CameraSyncModeState` are the third leg: if Archicad drew
// on this context we would see thousands per second, not tens.
// ---------------------------------------------------------------------------
// Tapioca.ViewerAutoOrbit { enabled, degreesPerStep? } -> { running, steps }
//
// Turn Archicad's own 3D camera at a fixed rate so a measurement phase does the
// same amount of work every time. See ArchViz/AutoOrbit.hpp for why this is a
// step on the camera-sync tick rather than a loop in the diagnostic, and for the
// promise that the projection is saved and restored inside the add-on.
//
// ⚠️ IT MOVES THE USER'S OWN 3D VIEW. Anything that turns it on owes a `Stop`,
// and `Stop` is also what `CameraSyncReset` calls: a cancelled run must not be
// able to leave the view rotated, and after a Stop the bus refuses the calls a
// diagnostic's own `finally` would make.
// ---------------------------------------------------------------------------
class ViewerAutoOrbitCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerAutoOrbit"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace orbit = geomsrv::archviz::autoorbit;

        bool enabled = false;
        params.Get ("enabled", enabled);

        if (!enabled) {
            orbit::Stop ();
        } else {
            double degreesPerStep = 0.4;
            params.Get ("degreesPerStep", degreesPerStep);
            GS::UniString error;
            if (!orbit::Start (degreesPerStep, error))
                return NativeCommandResult::Failure (error);
        }

        GS::ObjectState os;
        os.Add ("running", orbit::IsRunning ());
        os.Add ("steps", (GS::Int32) orbit::StepsTaken ());
        return os;
    }
};

class ViewerGpuDeviceInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuDeviceInfo"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        const auto device = av::dxgi::deviceidentity::Describe (
            (ID3D11Device*) av::dxgi::DiscoveredArchicadDevice ());
        os.Add ("deviceFound", device.found);
        os.Add ("is11On12", device.is11On12);
        os.Add ("creationFlags", (GS::Int32) device.creationFlags);
        os.Add ("featureLevel", (GS::Int32) device.featureLevel);
        os.Add ("debugLayer", device.debugLayer);
        os.Add ("singleThreaded", device.singleThreaded);
        os.Add ("bgraSupport", device.bgraSupport);
        os.Add ("highestDeviceInterface", (GS::Int32) device.highestDeviceInterface);

        // Which graphics runtimes are live, and whether the canvas the overlay
        // covers is an OpenGL window. See DeviceIdentity.hpp: after five runs the
        // D3D11 path is exonerated and the question is which stack draws the
        // model at all.
        const auto stack = av::dxgi::deviceidentity::DescribeRenderStack (
            uint64_t (uintptr_t (av::viewportoverlay::Stats ().target)));
        os.Add ("openglLoaded", stack.openglLoaded);
        os.Add ("openglIcdLoaded", stack.openglIcdLoaded);
        os.Add ("d3d12Loaded", stack.d3d12Loaded);
        os.Add ("vulkanLoaded", stack.vulkanLoaded);
        os.Add ("d2dLoaded", stack.d2dLoaded);
        os.Add ("dcompLoaded", stack.dcompLoaded);
        os.Add ("targetWindow",
                GS::UniString (std::to_string (stack.targetWindow).c_str (), CC_UTF8));
        os.Add ("targetHasPixelFormat", stack.targetHasPixelFormat);
        os.Add ("targetPixelFormat", (GS::Int32) stack.targetPixelFormat);
        os.Add ("targetSupportsOpenGL", stack.targetSupportsOpenGL);
        os.Add ("targetSupportsGdi", stack.targetSupportsGdi);
        os.Add ("targetDoubleBuffered", stack.targetDoubleBuffered);

        av::dxgi::ChainInfo chains[8];
        const size_t count = av::dxgi::GetChainInventory (chains, 8);
        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            GS::ObjectState row;
            // As strings: a swap chain or HWND does not fit an Int32, and these
            // are only ever compared for equality and read by eye.
            row.Add ("swapChain",
                     GS::UniString (std::to_string (chains[i].swapChain).c_str (), CC_UTF8));
            row.Add ("window",
                     GS::UniString (std::to_string (chains[i].window).c_str (), CC_UTF8));
            row.Add ("presents", (GS::Int32) chains[i].presents);
            row.Add ("width", (GS::Int32) chains[i].width);
            row.Add ("height", (GS::Int32) chains[i].height);
            row.Add ("ours", chains[i].ours);
            row.Add ("nominated", chains[i].nominated);
            rows.Push (row);
        }
        os.Add ("chains", rows);
        return os;
    }
};

const NativeCommandRegistration kViewerGpuStateCommandRegistrations[] = {
    { "ViewerPatchProfile", &MakeRegisteredNativeCommand<ViewerPatchProfileCommand>, false,
      R"json({"type":"object","properties":{"pin":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"path":{"type":"string"},"pinned":{"type":"boolean"},"pinnedSummary":{"type":"string"},"currentSummary":{"type":"string"},"hostPath":{"type":"string"},"hostVersion":{"type":"string"},"hostSha256":{"type":"string"},"targets":{"type":"integer","minimum":0},"verifies":{"type":"boolean"},"verifyError":{"type":"string"}},"additionalProperties":false,"required":["path","pinned","verifies"]})json" },
    { "ViewerGpuStateSlots", &MakeRegisteredNativeCommand<ViewerGpuStateSlotsCommand>, false,
      R"json({"type":"object","properties":{"constantBuffers":{"type":"boolean"},"slot":{"type":"string"},"enabled":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"installed":{"type":"boolean"},"slots":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"enabled":{"type":"boolean"},"calls":{"type":"integer"}},"additionalProperties":false,"required":["name","enabled","calls"]}}},"additionalProperties":false,"required":["installed","slots"]})json" },
    { "ViewerGpuStateCandidates", &MakeRegisteredNativeCommand<ViewerGpuStateCandidatesCommand>, false,
      R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":16}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scored":{"type":"boolean"},"referenceValid":{"type":"boolean"},"candidates":{"type":"array","items":{"type":"object","properties":{"buffer":{"type":"string"},"byteOffset":{"type":"integer"},"windowOffset":{"type":"integer"},"pairedOffset":{"type":"integer"},"byteWidth":{"type":"integer"},"variant":{"type":"string"},"boundBy":{"type":"string"},"bindSlot":{"type":"integer"},"maxPixelError":{"type":"number"},"meanPixelError":{"type":"number"},"changesWhileMoving":{"type":"integer"},"changesWhileStill":{"type":"integer"}},"additionalProperties":false,"required":["buffer","byteOffset","variant","maxPixelError"]}}},"additionalProperties":false,"required":["scored","referenceValid","candidates"]})json" },
    { "ViewerGpuDeviceInfo", &MakeRegisteredNativeCommand<ViewerGpuDeviceInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"deviceFound":{"type":"boolean"},"is11On12":{"type":"boolean"},"creationFlags":{"type":"integer"},"featureLevel":{"type":"integer"},"debugLayer":{"type":"boolean"},"singleThreaded":{"type":"boolean"},"bgraSupport":{"type":"boolean"},"highestDeviceInterface":{"type":"integer","minimum":0,"maximum":5},"openglLoaded":{"type":"boolean"},"openglIcdLoaded":{"type":"boolean"},"d3d12Loaded":{"type":"boolean"},"vulkanLoaded":{"type":"boolean"},"d2dLoaded":{"type":"boolean"},"dcompLoaded":{"type":"boolean"},"targetWindow":{"type":"string"},"targetHasPixelFormat":{"type":"boolean"},"targetPixelFormat":{"type":"integer"},"targetSupportsOpenGL":{"type":"boolean"},"targetSupportsGdi":{"type":"boolean"},"targetDoubleBuffered":{"type":"boolean"},"chains":{"type":"array","items":{"type":"object","properties":{"swapChain":{"type":"string"},"window":{"type":"string"},"presents":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"ours":{"type":"boolean"},"nominated":{"type":"boolean"}},"additionalProperties":false,"required":["swapChain","window","presents","ours","nominated"]}}},"additionalProperties":false,"required":["deviceFound","is11On12","chains"]})json" },
    { "ViewerAutoOrbit", &MakeRegisteredNativeCommand<ViewerAutoOrbitCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"degreesPerStep":{"type":"number","minimum":0.01,"maximum":15}},"additionalProperties":false,"required":["enabled"]})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"steps":{"type":"integer"}},"additionalProperties":false,"required":["running","steps"]})json" },
};

}   // namespace

NativeCommandRegistrations GetViewerGpuStateCommandRegistrations ()
{
    return MakeRegistrationView (kViewerGpuStateCommandRegistrations);
}

}   // namespace geomsrv
