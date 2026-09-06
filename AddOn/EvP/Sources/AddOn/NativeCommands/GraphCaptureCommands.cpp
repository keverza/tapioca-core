#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/GraphCaptureCommands.hpp"

#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"

namespace geomsrv {
namespace {

// ---------------------------------------------------------------------------
// Tapioca.List3DViews {} -> the View Map's 3D views.
//
// ⚠️ THE BROWSER NEVER ENUMERATES THIS, for the same reason it never enumerates
// layers: which views exist is THIS project's answer and changes with the open
// document. The parameter names the domain and the client asks for the members.
constexpr const char kList3DViewsInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
constexpr const char kList3DViewsResponse[] =
    R"json({"type":"object","properties":{"views":{"type":"array","items":{"type":"object","properties":{"guid":{"type":"string","minLength":1},"name":{"type":"string"},"path":{"type":"string"},"projection":{"type":"string","enum":["perspective","axonometric"]}},"additionalProperties":false,"required":["guid","name","path","projection"]}}},"additionalProperties":false,"required":["views"]})json";

class List3DViewsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "List3DViews";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<NavigatorEntry> entries;
        // Both maps, as DrawingCommands.cpp already does: the public View Map is
        // the shared one and My View Map is the user's own, and a view the user
        // can see in the Navigator must appear here whichever it lives in.
        CollectNavigatorItems (API_PublicViewMap, entries);
        CollectNavigatorItems (API_MyViewMap, entries);

        GS::Array<GS::ObjectState> views;
        for (const NavigatorEntry& entry : entries) {
            // ⚠️ ONLY THE 3D ONES. A section, a schedule and a layout are all
            // Navigator items too, and offering them in a picker whose captures
            // can only render the model would be a list of things that fail.
            const bool perspective = entry.itemType == API_PerspectiveNavItem;
            if (!perspective && entry.itemType != API_AxonometryNavItem)
                continue;
            GS::ObjectState view;
            view.Add ("guid", APIGuidToString (entry.guid));
            view.Add ("name", entry.name);
            view.Add ("path", entry.path);
            // Reported rather than filtered: an axonometric view is a perfectly
            // good SCOPE - it is what the view shows that matters here - even
            // though it could never supply a camera. Hiding it would look like
            // a missing view.
            view.Add ("projection", perspective ? "perspective" : "axonometric");
            views.Push (view);
        }

        GS::ObjectState os;
        os.Add ("views", views);
        return os;
    }
};

const NativeCommandRegistration registrations[] = {
    { "List3DViews", &MakeRegisteredNativeCommand<List3DViewsCommand>, false, kList3DViewsInput, kList3DViewsResponse },
};

} // namespace

NativeCommandRegistrations GetGraphCaptureCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
