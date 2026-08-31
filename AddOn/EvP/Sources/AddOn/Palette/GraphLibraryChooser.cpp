#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/GraphLibraryChooser.hpp"

#include "NodeGraph/GraphStore.hpp"

#include "DGFileDialog.hpp"    // DG::FileDialog - the ordinary Save/Open dialog
#include "FileTypeManager.hpp" // FTM::FileType - the *.json filter
#include "Location.hpp"        // IO::Location <-> path string
#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <filesystem>
#include <string>

namespace evp {

const char* const kGraphLibraryBrowseCommand = "Tapioca.GraphLibraryBrowse";

namespace {

namespace graph = ::evp::nodegraph;

// The store's own suffix. Repeated rather than exported: GraphStore keeps it
// private on purpose, and the chooser only needs to recognise and offer it.
constexpr const char* kGraphFileSuffix = ".tapiocagraph.json";

std::string Utf8 (const GS::UniString& value)
{
    return value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}

// UniString <-> the platform's own UTF-16, with no narrow encoding in between.
// Both are needed because a chosen path has to reach std::filesystem::path
// without ever being decoded as the ANSI code page; see the comment at the
// comparison below.
std::wstring WideText (const GS::UniString& value)
{
    const auto text = value.ToUStr ();
    return std::wstring (reinterpret_cast<const wchar_t*> (text.Get ()), value.GetLength ());
}

GS::UniString NarrowText (const std::wstring& value)
{
    static_assert (sizeof (wchar_t) == sizeof (GS::UniChar::Layout),
                   "UniString expects UTF-16, which is what wchar_t is on Windows");
    return GS::UniString (reinterpret_cast<const GS::UniChar::Layout*> (value.c_str ()), USize (value.size ()));
}

// Every answer, success or not, has the same shape, so the client renders one
// thing. A refusal is a REPORTED OUTCOME here exactly as it is for save and
// load: "you picked a folder the library does not own" is an answer, not a
// command failure.
GS::UniString Envelope (bool ok, const char* status, const std::string& error, const std::string& name,
                        const std::string& location)
{
    GS::ObjectState data;
    data.Add ("ok", ok);
    data.Add ("status", GS::UniString (status));
    data.Add ("error", Text (error));
    data.Add ("name", Text (name));
    data.Add ("location", Text (location));

    GS::ObjectState envelope;
    envelope.Add ("ok", true);
    envelope.Add ("data", data);

    GS::UniString json;
    if (JSON::CreateFromObjectState (envelope, json) != NoError)
        return GS::UniString ("{\"ok\":false,\"error\":{\"code\":\"Internal\",\"message\":\"The workflow chooser "
                              "answer could not be serialized.\"}}");
    return json;
}

// The graph name a chosen file carries. The dialog lets the user type anything,
// so accept the three spellings a person actually produces - the full
// "name.tapiocagraph.json", a bare "name.json", and a bare "name" - and let
// IsValidGraphId refuse the rest.
std::string GraphNameFromFileName (const std::string& fileName)
{
    const std::string suffix (kGraphFileSuffix);
    if (fileName.size () > suffix.size () &&
        fileName.compare (fileName.size () - suffix.size (), suffix.size (), suffix) == 0) {
        return fileName.substr (0, fileName.size () - suffix.size ());
    }
    const std::string json (".json");
    if (fileName.size () > json.size () &&
        fileName.compare (fileName.size () - json.size (), json.size (), json) == 0) {
        return fileName.substr (0, fileName.size () - json.size ());
    }
    return fileName;
}

// Two directories as the FILESYSTEM sees them, not as two strings. The chosen
// path arrives from a shell dialog, so it can differ from the library root in
// case, in separator, in a trailing dot, or through a junction - and every one
// of those compares unequal as text while naming the same directory.
bool SameDirectory (const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code code;
    if (std::filesystem::equivalent (left, right, code))
        return true;
    return std::filesystem::weakly_canonical (left, code) == std::filesystem::weakly_canonical (right, code);
}

} // namespace

GS::UniString RunGraphLibraryChooser (const GS::UniString& paramsJson)
{
    GS::ObjectState params;
    if (!paramsJson.IsEmpty () && JSON::ConvertToObjectState (paramsJson, params) != NoError)
        return Envelope (false, "invalid", "the chooser parameters are not valid JSON", "", "");

    GS::UniString mode;
    params.Get ("mode", mode);
    const bool saving = mode == "save";

    GS::UniString suggested;
    params.Get ("name", suggested);

    const std::string root = graph::FileGraphStore::DefaultWorkflowDirectory ();
    if (root.empty ())
        return Envelope (false, "noLocation", "there is no workflow library location on this machine", "", "");

    // The dialog cannot start in a folder that does not exist yet, and on a
    // fresh profile it does not: the library is created by the first save.
    const std::filesystem::path rootPath (root);
    std::error_code code;
    std::filesystem::create_directories (rootPath, code);

    // The library location as the CLIENT will render it. `root` is a narrow
    // string in the platform's encoding; every answer below reports this one
    // instead, so the location a support question quotes is the real one.
    const std::string shownRoot = Utf8 (NarrowText (rootPath.wstring ()));

    FTM::FileTypeManager fileTypeManager ("Tapioca.Workflow");
    const FTM::GroupID filterRoot = fileTypeManager.AddGroup ("Tapioca workflows");
    const FTM::FileType workflowType ("Tapioca workflow (*.json)", "json", 0, 0, 0);
    const FTM::TypeID typeID = fileTypeManager.AddType (workflowType, filterRoot);

    DG::FileDialog dialog (saving ? DG::FileDialog::Save : DG::FileDialog::OpenFile);
    // AddFilter only fills the popup; the dialog VALIDATES against the filter
    // ROOT, and the default root refuses everything. Both calls are required.
    if (typeID != FTM::UnknownType)
        dialog.AddFilter (typeID);
    dialog.SetFilterRoot (filterRoot);

    // Via the native wide form for the same reason the comparison below is: the
    // root is a narrow string in the platform's encoding, not UTF-8.
    dialog.SetFolder (IO::Location (NarrowText (rootPath.wstring ())));

    if (saving && !suggested.IsEmpty ()) {
        const std::wstring fileName = WideText (suggested) + WideText (GS::UniString (kGraphFileSuffix));
        const std::wstring full = (rootPath / fileName).wstring ();
        // false: keep the whole name. The default strips what it takes for an
        // extension, which for a double extension is the wrong half.
        dialog.SelectFile (IO::Location (NarrowText (full)), false);
    }

    if (!dialog.Invoke ())
        return Envelope (false, "cancelled", "", "", shownRoot);

    GS::UniString chosenText;
    if (dialog.GetSelectedFile ().ToPath (&chosenText) != NoError)
        return Envelope (false, "invalid", "the chosen file has no usable path", "", shownRoot);

    // ⚠️ THE TWO PATHS ARE COMPARED WIDE, NOT AS NARROW STRINGS. On Windows a
    // std::filesystem::path built from a std::string is decoded as the ANSI code
    // page, and the chosen path arrives here as UTF-16 from the shell. Passing
    // it through UTF-8 would make an accented profile name compare unequal to
    // itself and refuse a perfectly legal save. The library root goes on being
    // built from its own narrow string, which is what GraphStore does with the
    // same value, so both sides end up in the one native encoding.
    const std::filesystem::path chosen (WideText (chosenText));
    if (!SameDirectory (chosen.parent_path (), rootPath)) {
        return Envelope (false, "outsideLibrary",
                         "a workflow has to live in the library folder, and that one does not: " + shownRoot, "",
                         shownRoot);
    }

    // Back to UTF-8 for the answer. IsValidGraphId below allows only ASCII, so
    // nothing survives this step that could have been damaged by it.
    const std::string name = GraphNameFromFileName (Utf8 (NarrowText (chosen.filename ().wstring ())));
    if (!graph::IsValidGraphId (name))
        return Envelope (false, "invalid", "'" + name + "' is not a usable workflow name", name, shownRoot);

    return Envelope (true, "ok", "", name, shownRoot);
}

} // namespace evp
