#ifndef EVP_COMMANDCATALOG_HPP
#define EVP_COMMANDCATALOG_HPP

// Command discovery and scanner metadata shared by native callers and the palette.
// This layer owns no DG controls; UI-specific search adaptation stays in
// Palette/CommandScan.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <vector>

namespace evp {

struct CommandInfo {
    GS::UniString folder;
    GS::UniString path;
    GS::UniString title;
    GS::UniString category;
    GS::UniString description;
    GS::UniString requiresApi;
    GS::UniString requiresTapir;
    GS::UniString runtime;
    GS::Array<GS::UniString> requirements;
    GS::Array<GS::UniString> tags;
    GS::Array<GS::UniString> selectionSets;
    GS::Array<GS::UniString> paramJsons;
    // Names from evp.outputs.STANDARD_ACTIONS the palette offers as buttons under
    // the results ("csv", "pdf", "bake", ...). A named action needs no command
    // code: the framework builds it from the declared Outputs.
    // Parallel: actions[i] is the name run_action takes, actionLabels[i] is what
    // the button says. Two flat arrays because C++ reads string arrays out of
    // ObjectState reliably and nested objects it does not.
    GS::Array<GS::UniString> actions;
    GS::Array<GS::UniString> actionLabels;
    // Which preview band to size: "text" (the Plan.diff, free for every planning
    // command), "3d" or "plan2d". DECLARED by the command rather than inferred,
    // because the band has to be laid out before any command code runs.
    GS::UniString previewKind = "text";
    bool needsPreview = false;
    bool needsSelection = false;
    double timeoutSeconds = 0.0;
};

struct ScanOutcome {
    std::vector<CommandInfo> commands;
    UIndex broken = 0;
    GS::UniString status;
};

// Never returns an unexplained empty list: failures include a user-facing status
// and scanner diagnostics are written to logs\scan.log.
ScanOutcome ScanCommandFolders ();

// Nested scanner metadata crosses ObjectState as JSON strings because that is the
// representation it preserves reliably.
GS::UniString CommandInfoJson (const CommandInfo& info);
GS::Array<GS::UniString> CommandInfoJsons (const std::vector<CommandInfo>& commands);

} // namespace evp

#endif
