#include "ControlPalette.hpp"

#include "Grasshopper/GhWorkerHost.hpp"
#include "NativeCommands/SelectionSetStore.hpp"

#include "DGFileDialog.hpp"
#include "FileTypeManager.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

std::string ToUtf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

} // namespace

void ControlPalette::ChooseWorkflowDefinition ()
{
    FTM::FileTypeManager fileTypeManager ("Tapioca.Grasshopper");
    const FTM::GroupID filterRoot = fileTypeManager.AddGroup ("Grasshopper definitions");
    const FTM::TypeID binary =
        fileTypeManager.AddType (FTM::FileType ("Grasshopper definition (*.gh)", "gh", 0, 0, 0), filterRoot);
    const FTM::TypeID xml =
        fileTypeManager.AddType (FTM::FileType ("Grasshopper definition (*.ghx)", "ghx", 0, 0, 0), filterRoot);
    const FTM::TypeID gh2 =
        fileTypeManager.AddType (FTM::FileType ("Grasshopper 2 definition (*.ghz)", "ghz", 0, 0, 0), filterRoot);

    DG::FileDialog dialog (DG::FileDialog::OpenFile);
    if (binary != FTM::UnknownType)
        dialog.AddFilter (binary);
    if (xml != FTM::UnknownType)
        dialog.AddFilter (xml);
    if (gh2 != FTM::UnknownType)
        dialog.AddFilter (gh2);
    dialog.SetFilterRoot (filterRoot);

    if (!dialog.Invoke ())
        return;

    GS::UniString path;
    if (dialog.GetSelectedFile ().ToPath (&path) != NoError || path.IsEmpty ()) {
        SetCommandStatus ("That file's location could not be read.");
        return;
    }

    std::string extension = std::filesystem::path (ToUtf8 (path)).extension ().string ();
    std::transform (extension.begin (), extension.end (), extension.begin (),
                    [] (unsigned char c) { return (char) std::tolower (c); });
    evp::grasshopper::GhWorkerHost& host = evp::grasshopper::GhWorkerHost::Get ();
    if (extension != ".ghz" && extension != ".gh" && extension != ".ghx") {
        SetCommandStatus ("Choose a .gh, .ghx or .ghz definition.");
        return;
    }
    selectedWorkflowGh2 = extension == ".ghz";
    if (extension == ".ghz") {
        // The GH2 handshake and read-only Ping are available now; the typed
        // Player session protocol is a later slice. Never pass a GH2 document
        // to the GH1 archive loader or show GH1 inputs under this selection.
        GS::UniString startMessage;
        if (!host.EnsureHeadlessGh2 (startMessage)) {
            SetCommandStatus (startMessage);
            workflowLog.Note ("! " + ToUtf8 (startMessage));
            return;
        }
        pendingWorkflowPath.clear ();
        loadedWorkflowPath.clear ();
        lastWorkflowSchema.clear ();
        workflow.Clear ();
        workflowCommit = false;
        geomsrv::SelectionSetStore::Get ().Clear ();
        Layout ();
        SetCommandStatus (host.IsAttachedPeer ()
                              ? "GH2 peer selected for read-only Ping; Player loading is not enabled yet."
                              : "GH2 worker selected for read-only Ping; Player loading is not enabled yet.");
        NoteWorkflowLine (
            std::string (host.IsAttachedPeer () ? "GH2 attached peer selected for " : "GH2 worker selected for ") +
            ToUtf8 (path) + ". Player load is not enabled yet.");
        return;
    }

    // A saved GH1 definition chooses Rhino 8. Switching engines requires an
    // explicit Stop so a main-thread palette gesture never waits on teardown.
    if (!host.IsRunning () || host.IsGh2 ()) {
        GS::UniString startMessage;
        if (!host.EnsureHeadless (startMessage)) {
            SetCommandStatus (startMessage);
            workflowLog.Note ("! " + ToUtf8 (startMessage));
            return;
        }
        pendingWorkflowPath = ToUtf8 (path);
        workflowLog.Note ("Starting Grasshopper, then loading " + pendingWorkflowPath + " ...");
        SetCommandStatus (startMessage);
        return;
    }

    LoadWorkflowDefinition (ToUtf8 (path));
}
