#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/RoofSlopeCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/SlabSliceSource.hpp"

namespace geomsrv {
namespace {
class RoofSlopeEditorCommand : public MainThreadCommand {
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace rs = archviz::roofslope;
        namespace hud = archviz::overlayhud;
        auto state = archviz::guesttext::HudState ();
        GS::UniString action;
        params.Get ("action", action);
        if (action == "stop") {
            hud::StopRoofEditor (*state);
            archviz::overlaylayers::Clear (rs::kLayer);
        }
        else if (action == "start") {
            double percent = 2, cap = 200, height = 0.18;
            GS::UniString source;
            GS::Array<GS::Array<GS::ObjectState>> rings;
            if (!params.Get ("outline", rings) || !params.Get ("sourceGuid", source))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("Supply the slab outline and source GUID.", "opening roof slopes"));
            const auto reading =
                archviz::slabsource::Read ({ source.ToCStr (0, MaxUSize, CC_UTF8).Get () }, archviz::ReadStoreys ());
            if (reading.slabs.size () != 1)
                return NativeCommandResult::Failure (
                    EVP_FAIL ("Source slab or its story datum is unavailable.", "opening roof slopes"));
            const double top = reading.slabs.front ().top;
            if (params.Contains ("slopePercent"))
                ReadFiniteNumber (params, "slopePercent", percent);
            if (params.Contains ("maxArea"))
                ReadFiniteNumber (params, "maxArea", cap);
            if (params.Contains ("textHeight"))
                ReadFiniteNumber (params, "textHeight", height);
            std::vector<rs::Ring> outline;
            for (const auto& ring : rings) {
                rs::Ring out;
                for (const auto& point : ring) {
                    double x = 0, y = 0;
                    if (!ReadFiniteNumber (point, "x", x) || !ReadFiniteNumber (point, "y", y))
                        return NativeCommandResult::Failure (
                            EVP_FAIL ("Footprint points must be finite.", "opening roof slopes"));
                    out.push_back ({ x, y });
                }
                outline.push_back (std::move (out));
            }
            rs::Data data;
            std::string error;
            if (!rs::Plan (outline, top, percent, cap, data, error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8), "opening roof slopes"));
            data.textHeight = height;
            auto layer = rs::Layer (data);
            error = archviz::overlaylayers::Validate (layer);
            if (!error.empty ())
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8), "publishing roof slopes"));
            hud::StartRoofEditor (*state, std::move (data));
            archviz::overlaylayers::Set (std::move (layer));
        }
        const bool roofKeys = hud::RoofEditorActive (*state) && hud::EditLocked (*state) &&
                              hud::ContentShown (*state) && hud::LayerShown (*state, rs::kLayer);
        if (!archviz::overlayinput::EnableRoofEditKeys (roofKeys)) {
            hud::StopRoofEditor (*state);
            archviz::overlaylayers::Clear (rs::kLayer);
            archviz::overlaycontrol::PublishLayers ();
            archviz::overlayinput::RequestLayout (archviz::overlayinput::View::ThreeD);
            archviz::overlayinput::RequestLayout (archviz::overlayinput::View::Plan);
            return NativeCommandResult::Failure (EVP_FAIL (
                "Could not acquire scoped keyboard ownership; roof editing was refused.", "opening roof slopes"));
        }
        archviz::overlaycontrol::PublishLayers ();
        archviz::overlayinput::RequestLayout (archviz::overlayinput::View::ThreeD);
        archviz::overlayinput::RequestLayout (archviz::overlayinput::View::Plan);
        GS::ObjectState result;
        result.Add ("active", hud::RoofEditorActive (*state));
        result.Add ("locked", hud::EditLocked (*state));
        if (const auto* data = hud::RoofEditorData (*state)) {
            result.Add ("rectangles", GS::Int32 (data->cells.size ()));
            result.Add ("triangles", GS::Int32 (data->triangles.size ()));
            result.Add ("slabTopZ", data->topZ);
            const auto measures = rs::Measure (*data);
            result.Add ("faces", measures.faces);
            result.Add ("maxAspect", measures.maxAspect);
            result.Add ("maxSpan", measures.maxSpan);
            result.Add ("maxBorderRise", measures.maxBorderRise);
        }
        return result;
    }
};
} // namespace
NativeCommandRegistrations GetRoofSlopeCommandRegistrations ()
{
    static const NativeCommandRegistration registrations[] = {
        // Read-only UI lifecycle must release its borrowed lock even after Run cancellation.
        // Python checks cancellation before and after start; no model write is exempted.
        { "RoofSlopeEditor", &MakeRegisteredNativeCommand<RoofSlopeEditorCommand>, true,
          R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","state"]},"sourceGuid":{"type":"string"},"outline":{"type":"array","minItems":1,"maxItems":1000,"items":{"type":"array","minItems":3,"maxItems":20000,"items":{"$ref":"#Point2D"}}},"slopePercent":{"type":"number","exclusiveMinimum":0,"maximum":100},"maxArea":{"type":"number","exclusiveMinimum":0,"maximum":200},"textHeight":{"type":"number","minimum":0.01,"maximum":5}},"required":["action"],"additionalProperties":false})json",
          R"json({"type":"object","properties":{"active":{"type":"boolean"},"locked":{"type":"boolean"},"rectangles":{"type":"integer"},"triangles":{"type":"integer"},"slabTopZ":{"type":"number"},"faces":{"type":"integer"},"maxAspect":{"type":"number"},"maxSpan":{"type":"number"},"maxBorderRise":{"type":"number"}},"required":["active","locked"],"additionalProperties":false})json" }
    };
    return MakeRegistrationView (registrations);
}
} // namespace geomsrv
