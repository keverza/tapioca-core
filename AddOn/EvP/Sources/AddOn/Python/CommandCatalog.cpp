#include "CommandCatalog.hpp"

#include "PathUtils.hpp"
#include "PythonHost.hpp"

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>

namespace evp {

ScanOutcome ScanCommandFolders ()
{
    ScanOutcome outcome;

    // Nothing below writes scan.log until the scan has succeeded, so these
    // breadcrumbs are the only evidence left by an interpreter startup crash.
    StartupTrace ("ScanCommandFolders enter");

    GS::UniString error;
    if (!PythonHost::Get ().EnsureInitialized (error)) {
        outcome.status = "Python init failed - see logs\\scan.log";
        AppendTextLine (ScanLogPath (), "Rescan: interpreter init failed: " + error);
        return outcome;
    }
    if (!EnsureSampleCommandExists (error))
        AppendTextLine (ScanLogPath (), "Rescan: sample commands: " + error);

    StartupTrace ("ScanCommandFolders interpreter ready, scanning");
    const GS::UniString root = GetScriptsRoot ();
    GS::UniString json;
    if (!PythonHost::Get ().ScanCommands (root, json, error)) {
        outcome.status = "Scan failed - see logs\\scan.log";
        AppendTextLine (ScanLogPath (), "Rescan FAILED: " + error);
        return outcome;
    }
    AppendTextLine (ScanLogPath (), "=========================================================");
    AppendTextLine (ScanLogPath (), json);

    GS::ObjectState result;
    if (JSON::ConvertToObjectState (json, result) != NoError) {
        outcome.status = "Scan result was not valid JSON - see logs\\scan.log";
        return outcome;
    }

    // The scanner returns nested data as arrays of JSON strings, the only shape
    // ObjectState reliably reads back here. See evp/_scanner.py.
    GS::Array<GS::UniString> commandJsons, diagnosticJsons;
    result.Get ("commandsJson", commandJsons);
    result.Get ("diagnosticsJson", diagnosticJsons);

    for (const GS::UniString& commandJson : commandJsons) {
        GS::ObjectState os;
        if (JSON::ConvertToObjectState (commandJson, os) != NoError)
            continue;
        CommandInfo info;
        os.Get ("folder", info.folder);
        os.Get ("path", info.path);
        os.Get ("title", info.title);
        os.Get ("category", info.category);
        os.Get ("description", info.description);
        os.Get ("requires_api", info.requiresApi);
        os.Get ("requires_tapir", info.requiresTapir);
        os.Get ("runtime", info.runtime);
        os.Get ("requires", info.requirements);
        os.Get ("tags", info.tags);
        os.Get ("selection_sets", info.selectionSets);
        os.Get ("needs_selection", info.needsSelection);
        os.Get ("needs_preview", info.needsPreview);
        os.Get ("preview_kind", info.previewKind);
        os.Get ("actions", info.actions);
        os.Get ("action_labels", info.actionLabels);
        os.Get ("timeout_s", info.timeoutSeconds);
        os.Get ("paramsJson", info.paramJsons);
        outcome.commands.push_back (info);
    }

    // The scanner sorts by folder; this stable category sort preserves that order
    // within each display group.
    std::stable_sort (outcome.commands.begin (), outcome.commands.end (),
                      [] (const CommandInfo& a, const CommandInfo& b) { return a.category < b.category; });

    for (const GS::UniString& diagnosticJson : diagnosticJsons) {
        GS::ObjectState os;
        if (JSON::ConvertToObjectState (diagnosticJson, os) != NoError)
            continue;
        GS::UniString folder, message;
        GS::Int32 line = 0;
        os.Get ("folder", folder);
        os.Get ("error", message);
        os.Get ("line", line);
        AppendTextLine (ScanLogPath (), GS::UniString::Printf ("  DIAGNOSTIC %T (line %d): %T", folder.ToPrintf (),
                                                               (int) line, message.ToPrintf ()));
        ++outcome.broken;
    }

    if (outcome.broken > 0)
        outcome.status = GS::UniString::Printf ("%u command(s); %u folder(s) FAILED - see logs\\scan.log",
                                                (unsigned) outcome.commands.size (), (unsigned) outcome.broken);
    else
        outcome.status =
            GS::UniString::Printf ("%u command(s) in %T", (unsigned) outcome.commands.size (), root.ToPrintf ());

    return outcome;
}

GS::UniString CommandInfoJson (const CommandInfo& info)
{
    GS::ObjectState command;
    command.Add ("folder", info.folder);
    command.Add ("path", info.path);
    command.Add ("title", info.title);
    command.Add ("category", info.category);
    command.Add ("description", info.description);
    command.Add ("requires_api", info.requiresApi);
    command.Add ("requires_tapir", info.requiresTapir);
    command.Add ("runtime", info.runtime);
    command.Add ("requires", info.requirements);
    command.Add ("tags", info.tags);
    command.Add ("selection_sets", info.selectionSets);
    command.Add ("needs_selection", info.needsSelection);
    command.Add ("needs_preview", info.needsPreview);
    command.Add ("preview_kind", info.previewKind);
    command.Add ("actions", info.actions);
    command.Add ("action_labels", info.actionLabels);
    command.Add ("timeout_s", info.timeoutSeconds);
    command.Add ("params", info.paramJsons);

    GS::UniString json;
    if (JSON::CreateFromObjectState (command, json) != NoError)
        return GS::EmptyUniString;
    return json;
}

GS::Array<GS::UniString> CommandInfoJsons (const std::vector<CommandInfo>& commands)
{
    GS::Array<GS::UniString> jsons;
    for (const CommandInfo& command : commands) {
        const GS::UniString json = CommandInfoJson (command);
        if (!json.IsEmpty ())
            jsons.Push (json);
    }
    return jsons;
}

} // namespace evp
