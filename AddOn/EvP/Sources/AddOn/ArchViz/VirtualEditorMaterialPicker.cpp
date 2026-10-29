// OVERLAY-INVARIANTS.md §§8,11,12: queued native attribute UI only. No model
// writes, renderer changes or SDK work on layout/Present threads.
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/VirtualEditorMaterialPicker.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorMaterial.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "NativeCommands/HardscapeMaterial.hpp"
#include "Palette/HardscapeMaterialDialog.hpp"
#include "Python/MainThreadGate.hpp"
#include <algorithm>

namespace geomsrv::archviz {
void FollowMaterialPicker (const std::shared_ptr<overlayhud::State>& state)
{
    namespace lg = tapioca::geometry;
    const auto session = state->virtualEditor;
    if (!session || session->materialPickerQueued || session->materialCatalogQueued)
        return;
    const auto request = session->materialPicker;
    const auto revision = session->edits.Doc ().revision;
    if (!request && session->materialCatalogRevision == revision)
        return;
    if (request)
        session->materialPickerQueued = true;
    else
        session->materialCatalogQueued = true;
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post (
            [state, session, request, revision] {
                const auto current = [&] {
                    return state == guesttext::HudState () && state->virtualEditor == session && !session->close &&
                           state->shown && state->editLocked && overlayhud::LayerShown (*state, editlayers::kLayer) &&
                           session->edits.Doc ().revision == revision &&
                           (!request || (request->revision == revision && session->materialPicker == request));
                };
                const auto finish = [&] {
                    if (session->materialPicker == request)
                        session->materialPicker.reset ();
                    session->materialPickerQueued = session->materialCatalogQueued = false;
                    overlayinput::RequestLayout (overlayinput::View::ThreeD);
                    overlayinput::RequestLayout (overlayinput::View::Plan);
                };
                if (!current ()) {
                    if (request)
                        session->message = "Material choice cancelled: the editor changed, was hidden or unlocked.";
                    finish ();
                    return;
                }
                auto catalog = std::make_shared<lg::MaterialCatalog> ();
                std::string failure;
                if (!tapioca::materialui::ReadCatalog (*catalog, failure)) {
                    session->materialCatalog.reset ();
                    session->materialCatalogError = failure;
                    session->materialCatalogRevision = revision;
                    if (request)
                        session->message = failure;
                    finish ();
                    return;
                }
                if (!current ()) {
                    finish ();
                    return;
                }
                session->materialCatalog = catalog;
                session->materialCatalogRevision = revision;
                session->materialCatalogError.clear ();
                if (request && session->edits.Doc ().landscape) {
                    const auto& objects = session->edits.Doc ().landscape->objects;
                    const auto object = std::find_if (objects.begin (), objects.end (),
                                                      [&] (const auto& o) { return o.id == request->object; });
                    if (object != objects.end ()) {
                        const auto before = object->baseMaterial;
                        lg::MaterialSpec picked;
                        if (tapioca::materialui::Pick (before, *catalog, picked, failure)) {
                            if (current ())
                                edithud::ApplyPickedMaterial (*session, request, picked);
                            else
                                session->message =
                                    "Material choice cancelled: the project/editor changed while the dialog was open.";
                        }
                        else if (!failure.empty ())
                            session->message = failure;
                    }
                }
                finish ();
            },
            error)) {
        session->materialPickerQueued = session->materialCatalogQueued = false;
        session->materialPicker.reset ();
        session->materialCatalogError = std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    }
}
} // namespace geomsrv::archviz
