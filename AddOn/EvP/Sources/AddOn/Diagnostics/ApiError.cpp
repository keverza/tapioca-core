#include "Diagnostics/ApiError.hpp"
#include "Python/PathUtils.hpp"     // EvpDataDir, AppendTextLine (ACAPI-free, like this file)

// The DevKit's error ENUM only — no ACAPI.h. This is what lets every layer
// include ApiError.hpp without dragging the API surface behind it.
#include "APIdefs_ErrorCodes.h"

#include "Array.hpp"

#include <atomic>
#include <ctime>
#include <mutex>
#include <vector>

namespace evp {

namespace {

// ---------------------------------------------------------------------------
// The code table
//
// Copied from Support/Inc/APIdefs_ErrorCodes.h and GSRoot/Definitions.hpp, and
// static_assert'ed against those enums below — so if a DevKit upgrade renumbers
// or drops a code, THIS FILE fails to compile rather than quietly printing a
// wrong symbol. That is the point: a decoder that can be wrong is worse than no
// decoder, because it sends you looking in the wrong place.
//
// Meanings are the DevKit's own doc comments, trimmed to one line.
// ---------------------------------------------------------------------------
struct CodeEntry {
    GSErrCode   code;
    const char* name;
    const char* meaning;
};

const CodeEntry codeTable[] = {
    { NoError, "NoError", "The operation succeeded." },

    // --- GSRoot (Definitions.hpp) — these reach the API layer through file and
    //     memory operations, and are the ones most often mistaken for API codes.
    { GS::ErrRead,          "ErrRead",          "I/O read error." },
    { GS::ErrDskFul,        "ErrDskFul",        "The disk is full." },
    { GS::ErrIO,            "ErrIO",            "General I/O error." },
    { GS::ErrEof,           "ErrEof",           "End of file." },
    { GS::ErrFnf,           "ErrFnf",           "File not found." },
    { GS::ErrParam,         "ErrParam",         "Invalid parameter." },
    { GS::ErrPerm,          "ErrPerm",          "Permission denied (on file open)." },
    { GS::ErrWrPerm,        "ErrWrPerm",        "Write permission denied." },
    { GS::ErrMemoryFull,    "ErrMemoryFull",    "Out of memory." },
    { GS::ErrNilHandle,     "ErrNilHandle",     "A nil handle was passed." },
    { GS::ErrUserCanceled,  "ErrUserCanceled",  "The user cancelled the operation." },
    { GS::ErrTime,          "ErrTime",          "Invalid time." },
    { GS::ErrNilPtr,        "ErrNilPtr",        "A nil pointer was passed." },
    { GS::ErrEmptyHandle,   "ErrEmptyHandle",   "An empty handle was passed." },
    { GS::Error,            "Error",            "General GSRoot error." },
    { GS::Cancel,           "Cancel",           "The operation was cancelled." },
    { GS::ExceptionError,   "ExceptionError",   "An exception was thrown." },

    // --- API general
    { APIERR_GENERAL,   "APIERR_GENERAL",   "General error code." },
    { APIERR_MEMFULL,   "APIERR_MEMFULL",   "Insufficient memory." },
    { APIERR_CANCEL,    "APIERR_CANCEL",    "The operation has been canceled by the user, in case of a long process." },

    // --- API bad input (the 101-117 block: overwhelmingly what a write command hits)
    { APIERR_BADID,             "APIERR_BADID",             "The passed identifier is not a valid one, or valid, but not proper for the given operation." },
    { APIERR_BADINDEX,          "APIERR_BADINDEX",          "The passed index is out of range." },
    { APIERR_BADNAME,           "APIERR_BADNAME",           "The passed name is not proper or not found in the existing list." },
    { APIERR_BADPARS,           "APIERR_BADPARS",           "The passed parameters are inconsistent." },
    { APIERR_BADPOLY,           "APIERR_BADPOLY",           "The passed polygon cannot be interpreted." },
    { APIERR_BADDATABASE,       "APIERR_BADDATABASE",       "The command cannot be executed on the current database." },
    { APIERR_BADWINDOW,         "APIERR_BADWINDOW",         "The command cannot be executed while the current window is active." },
    { APIERR_BADKEYCODE,        "APIERR_BADKEYCODE",        "The key code cannot be found in the listing database." },
    { APIERR_BADPLATFORMSIGN,   "APIERR_BADPLATFORMSIGN",   "The passed platform sign is not valid." },
    { APIERR_BADPLANE,          "APIERR_BADPLANE",          "The plane equation is incorrect." },
    { APIERR_BADUSERID,         "APIERR_BADUSERID",         "The passed user ID (TeamWork client) is not valid." },
    { APIERR_BADVALUE,          "APIERR_BADVALUE",          "The passed autotext value is not valid." },
    { APIERR_BADELEMENTTYPE,    "APIERR_BADELEMENTTYPE",    "The function cannot be applied to the passed element type." },
    { APIERR_IRREGULARPOLY,     "APIERR_IRREGULARPOLY",     "The passed polygon or polyline is irregular (self-intersecting, repeated or collinear vertices, wrong orientation)." },
    { APIERR_BADEXPRESSION,     "APIERR_BADEXPRESSION",     "The passed expression string is syntactically incorrect." },
    { APIERR_BADGUID,           "APIERR_BADGUID",           "The passed guid is invalid or valid, but not proper for the given operation." },
    { APIERR_BADTOKEN,          "APIERR_BADTOKEN",          "The passed token is invalid." },

    // --- API "nothing there"
    { APIERR_NO3D,              "APIERR_NO3D",              "There is no 3D information assigned to the passed element." },
    { APIERR_NOMORE,            "APIERR_NOMORE",            "No more database items can be returned." },
    { APIERR_NOPLAN,            "APIERR_NOPLAN",            "There is no open project." },
    { APIERR_NOLIB,             "APIERR_NOLIB",             "No library was loaded." },
    { APIERR_NOLIBSECT,         "APIERR_NOLIBSECT",         "The requested LibPart section is not found." },
    { APIERR_NOSEL,             "APIERR_NOSEL",             "No selection. The operation cannot be executed without any element selected." },
    { APIERR_NOTEDITABLE,       "APIERR_NOTEDITABLE",       "The referenced element is not editable." },
    { APIERR_NOTSUBTYPEOF,      "APIERR_NOTSUBTYPEOF",      "The first library part unique ID does not refer to a subtype of the second." },
    { APIERR_NOTEQUALMAIN,      "APIERR_NOTEQUALMAIN",      "The main GUID parts of the two library part unique IDs are not equal." },
    { APIERR_NOTEQUALREVISION,  "APIERR_NOTEQUALREVISION",  "The main GUIDs are equal but the revision IDs differ." },
    { APIERR_NOTEAMWORKPROJECT, "APIERR_NOTEAMWORKPROJECT", "There is no open project, or not in Teamwork mode." },

    { APIERR_NOUSERDATA,        "APIERR_NOUSERDATA",        "The element has no user data assigned." },
    { APIERR_MOREUSER,          "APIERR_MOREUSER",          "No free storage block available for the element's user data." },
    { APIERR_LINKEXIST,         "APIERR_LINKEXIST",         "The link already exists." },
    { APIERR_LINKNOTEXIST,      "APIERR_LINKNOTEXIST",      "The link doesn't exist." },
    { APIERR_WINDEXIST,         "APIERR_WINDEXIST",         "The window to be opened already exists." },
    { APIERR_WINDNOTEXIST,      "APIERR_WINDNOTEXIST",      "The referenced window does not exist." },
    { APIERR_UNDOEMPTY,         "APIERR_UNDOEMPTY",         "No undoable entry got into the opened undo operation." },
    { APIERR_REFERENCEEXIST,    "APIERR_REFERENCEEXIST",    "The reference already exists." },
    { APIERR_NAMEALREADYUSED,   "APIERR_NAMEALREADYUSED",   "The resource name must be unique but the specified one is already taken." },

    // --- API database / attributes
    { APIERR_ATTREXIST,         "APIERR_ATTREXIST",         "The attribute already exists." },
    { APIERR_DELETED,           "APIERR_DELETED",           "Reference to a deleted, purged or non-existent database item." },
    { APIERR_LOCKEDLAY,         "APIERR_LOCKEDLAY",         "The referenced layer is LOCKED — unlock it in Layer Settings." },
    { APIERR_HIDDENLAY,         "APIERR_HIDDENLAY",         "The referenced layer is HIDDEN — show it in Layer Settings." },
    { APIERR_INVALFLOOR,        "APIERR_INVALFLOOR",        "The passed floor index is out of range." },
    { APIERR_NOTMINE,           "APIERR_NOTMINE",           "The database item is not in the user's Teamwork workspace." },
    { APIERR_NOACCESSRIGHT,     "APIERR_NOACCESSRIGHT",     "No right to access / create / modify / delete this item on the Teamwork server." },
    { APIERR_BADPROPERTY,       "APIERR_BADPROPERTY",       "The property is not available for the passed element or attribute." },
    { APIERR_BADCLASSIFICATION, "APIERR_BADCLASSIFICATION", "The classification cannot be set for the passed element or attribute." },
    { APIERR_NOTEXISTINGLAYER,  "APIERR_NOTEXISTINGLAYER",  "The given layer parameter index does not exist." },
    { APIERR_NOCURRATTRSET,     "APIERR_NOCURRATTRSET",     "No current attribute set." },

    // --- API add-on communication
    { APIERR_MODULNOTINSTALLED,        "APIERR_MODULNOTINSTALLED",        "The referenced add-on is not installed." },
    { APIERR_MODULCMDMINE,             "APIERR_MODULCMDMINE",             "The target add-on is the caller add-on." },
    { APIERR_MODULCMDNOTSUPPORTED,     "APIERR_MODULCMDNOTSUPPORTED",     "The referenced command is not supported by the target add-on." },
    { APIERR_MODULCMDVERSNOTSUPPORTED, "APIERR_MODULCMDVERSNOTSUPPORTED", "The requested command version is newer than the target add-on supports." },
    { APIERR_NOMODULEDATA,             "APIERR_NOMODULEDATA",             "No custom data section is saved into the project file for this add-on." },
    { APIERR_DEPRECATEDCALL,           "APIERR_DEPRECATEDCALL",           "This is now a C++ API call in ArchicadAPI and is no longer available via the Old API — it did NOTHING. Find the C++ equivalent." },

    // --- API text runs / paragraphs
    { APIERR_PAROVERLAP,    "APIERR_PAROVERLAP",    "Two or more paragraphs overlap." },
    { APIERR_PARMISSING,    "APIERR_PARMISSING",    "The number of paragraphs is zero." },
    { APIERR_PAROVERFLOW,   "APIERR_PAROVERFLOW",   "A paragraph end offset runs over the content length." },
    { APIERR_PARIMPLICIT,   "APIERR_PARIMPLICIT",   "The content contains a line end (CR) inside a paragraph range." },
    { APIERR_RUNOVERLAP,    "APIERR_RUNOVERLAP",    "Two or more runs overlap." },
    { APIERR_RUNMISSING,    "APIERR_RUNMISSING",    "The number of runs is zero." },
    { APIERR_RUNOVERFLOW,   "APIERR_RUNOVERFLOW",   "A run end offset runs over the content length." },
    { APIERR_RUNIMPLICIT,   "APIERR_RUNIMPLICIT",   "A run's begin offset is greater than the previous run's end offset." },
    { APIERR_RUNPROTECTED,  "APIERR_RUNPROTECTED",  "Attempted to overwrite a protected text run." },
    { APIERR_EOLOVERLAP,    "APIERR_EOLOVERLAP",    "The EOL array is not monotonically ascending." },
    { APIERR_TABOVERLAP,    "APIERR_TABOVERLAP",    "The tabulator array is not monotonically ascending." },

    // --- API command protocol (the undo-scope codes: the ones that mean YOU
    //     wrapped the call wrongly, not that the data was bad)
    { APIERR_NOTINIT,           "APIERR_NOTINIT",           "The command needs initialization by another API call first." },
    { APIERR_NESTING,           "APIERR_NESTING",           "The API function is not reentrant and nesting occurred." },
    { APIERR_NOTSUPPORTED,      "APIERR_NOTSUPPORTED",      "The command is not supported by the server application at all." },
    { APIERR_REFUSEDCMD,        "APIERR_REFUSEDCMD",        "The passed identifier is not subject to this operation. From ACAPI_CallUndoableCommand this means an undo scope is ALREADY open — undoable commands cannot nest (see WriteCommand)." },
    { APIERR_REFUSEDPAR,        "APIERR_REFUSEDPAR",        "The command cannot be executed with the passed parameters (they parsed, but Archicad refuses this combination)." },
    { APIERR_READONLY,          "APIERR_READONLY",          "The specified location is read-only." },
    { APIERR_SERVICEFAILED,     "APIERR_SERVICEFAILED",     "The invoked Teamwork service has failed." },
    { APIERR_COMMANDFAILED,     "APIERR_COMMANDFAILED",     "The invoked undoable command threw an exception — the REAL cause is the inner call's own error code, log that too." },
    { APIERR_NEEDSUNDOSCOPE,    "APIERR_NEEDSUNDOSCOPE",    "This call must be inside an ACAPI_CallUndoableCommand scope — in EvP the dispatcher or a transaction supplies it, so the command was reached outside one." },
    { APIERR_UNDOSCOPEMISUSE,   "APIERR_UNDOSCOPEMISUSE",   "This call must NOT be inside an ACAPI_CallUndoableCommand scope." },

    { APIERR_MISSINGCODE,       "APIERR_MISSINGCODE",       "The function is not implemented yet." },
    { APIERR_MISSINGDEF,        "APIERR_MISSINGDEF",        "The originating library part file is missing." },
};

// If a DevKit upgrade moves any of these, the build breaks here rather than the
// decoder lying at runtime. Spot-checked across every block of the enum, plus
// the arithmetic the unknown-code path relies on.
static_assert (APIERR_GENERAL          == (GS::ErrorFlagMask | (262 << 16)) + 1,    "APIERR block base moved");
static_assert (APIERR_BADPARS          == APIERR_GENERAL + 103,                     "APIERR_BADPARS moved");
static_assert (APIERR_IRREGULARPOLY    == APIERR_GENERAL + 113,                     "APIERR_IRREGULARPOLY moved");
static_assert (APIERR_NOSEL            == APIERR_GENERAL + 205,                     "APIERR_NOSEL moved");
static_assert (APIERR_LOCKEDLAY        == APIERR_GENERAL + 302,                     "APIERR_LOCKEDLAY moved");
static_assert (APIERR_MODULNOTINSTALLED == APIERR_GENERAL + 400,                    "APIERR module block moved");
static_assert (APIERR_REFUSEDPAR       == APIERR_GENERAL + 904,                     "APIERR_REFUSEDPAR moved");
static_assert (APIERR_MISSINGDEF       == APIERR_GENERAL + 1001,                    "APIERR tail moved");
static_assert (API_ModuleId            == 262,                                      "the API module id changed");

const CodeEntry* Lookup (GSErrCode err)
{
    for (const CodeEntry& entry : codeTable) {
        if (entry.code == err)
            return &entry;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The in-flight bus call, per thread
//
// Thread-local and a plain stack: a worker thread's calls and a main-thread job
// each see their own, so two concurrent runs cannot smear each other's call_id
// across the trail. A raw vector rather than anything clever — this is only ever
// touched from the thread that owns it.
// ---------------------------------------------------------------------------
struct CallFrame {
    GS::UniString callId;
    GS::UniString command;
    GS::UniString paramsJson;
};

thread_local std::vector<CallFrame> callStack;

// ---------------------------------------------------------------------------
// The session's failure trail
//
// A small ring the palette and EvP.GetErrorTrail read back. Shared across
// threads, hence the mutex; kept tiny because its only job is "what just went
// wrong" — the full history is the log file.
// ---------------------------------------------------------------------------
const size_t kTrailCap = 32;

std::mutex                 trailMutex;
std::vector<GS::UniString> trail;
size_t                     trailNext = 0;      // next slot to overwrite once full
std::atomic<UInt64>        failureCount { 0 };

void PushTrail (const GS::UniString& line)
{
    std::lock_guard<std::mutex> lock (trailMutex);
    if (trail.size () < kTrailCap) {
        trail.push_back (line);
    } else {
        trail[trailNext] = line;
        trailNext = (trailNext + 1) % kTrailCap;
    }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

GS::UniString Timestamp ()
{
    const std::time_t now = std::time (nullptr);
    std::tm           local {};
#if defined (_MSC_VER)
    localtime_s (&local, &now);
#else
    local = *std::localtime (&now);
#endif
    char buffer[32] = {};
    std::strftime (buffer, sizeof (buffer), "%Y-%m-%d %H:%M:%S", &local);
    return GS::UniString (buffer);
}

// The file name without its directory — the log is read by a human who knows
// the repo, and the full build path is noise on every single line.
GS::UniString BaseName (const char* path)
{
    if (path == nullptr)
        return GS::UniString ("?");
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/')
            last = p + 1;
    }
    return GS::UniString (last);
}

// A params echo long enough to identify the call, short enough not to bury the
// entry. The full params are already in commands.log / api_trace.log; what this
// needs to answer is "which call was that".
const UIndex kParamsEcho = 400;

GS::UniString Clip (const GS::UniString& text, UIndex limit)
{
    if (text.GetLength () <= limit)
        return text;
    return text.GetSubstring (0, limit) + GS::UniString ("... [clipped]");
}

// Writes one block. Opens and closes the file once (AppendTextLine appends the
// final newline itself), so the entry is whole on disk even if Archicad dies
// immediately after — which is exactly when you need it.
void WriteBlock (const GS::UniString& headline, const GS::UniString& body)
{
    const GS::UniString path = ApiErrorLogPath ();
    if (path.IsEmpty ())
        return;                                 // no %LOCALAPPDATA%; nothing we can do

    // Ensure logs\ exists — AppendTextLine's CreateFileW(OPEN_ALWAYS) fails on a
    // missing directory, and this file is often the FIRST thing written in a
    // fresh profile (a failure during startup, before any command has run and
    // made the folder). Once per process: the error path must stay cheap enough
    // that a loop reporting hundreds of failures does not become the bottleneck.
    static std::once_flag logsDirOnce;
    std::call_once (logsDirOnce, [] () {
        const GS::UniString dataDir = EvpDataDir ();
        if (!dataDir.IsEmpty ())
            CreateDirectoryChain (dataDir + GS::UniString ("\\logs"));
    });

    GS::UniString block ("---------------------------------------------------------------\r\n");
    block += Timestamp () + GS::UniString ("  ") + headline;
    if (!body.IsEmpty ())
        block += GS::UniString ("\r\n") + body;

    AppendTextLine (path, block);
}

// The "who asked for this" lines, shared by both report paths.
GS::UniString CallFrameLines ()
{
    if (callStack.empty ())
        return GS::UniString ();

    const CallFrame& frame = callStack.back ();
    GS::UniString lines = GS::UniString::Printf ("  call    [%T] %T",
                                                 frame.callId.ToPrintf (),
                                                 frame.command.ToPrintf ());
    if (!frame.paramsJson.IsEmpty () && frame.paramsJson != "{}") {
        lines += GS::UniString::Printf ("\r\n  params  %T",
                                        Clip (frame.paramsJson, kParamsEcho).ToPrintf ());
    }
    return lines;
}

GS::UniString CurrentCommand ()
{
    return callStack.empty () ? GS::UniString ("(no bus call)") : callStack.back ().command;
}

}   // namespace

// ---------------------------------------------------------------------------

const char* ErrCodeName (GSErrCode err)
{
    const CodeEntry* entry = Lookup (err);
    return (entry != nullptr) ? entry->name : nullptr;
}

const char* ErrCodeMeaning (GSErrCode err)
{
    const CodeEntry* entry = Lookup (err);
    return (entry != nullptr) ? entry->meaning : nullptr;
}

GS::UniString DescribeErr (GSErrCode err)
{
    const CodeEntry* entry = Lookup (err);
    if (entry != nullptr)
        return GS::UniString::Printf ("%s (%d) - %s", entry->name, (int) err, entry->meaning);

    // Unknown, but not opaque: every Graphisoft error packs a module id and a
    // sub-code, and those two are enough to grep the DevKit headers by hand.
    // Saying so beats printing a bare negative number that looks like garbage.
    const Int32 moduleId = ((Int32) err & GS::ModuleMask) >> 16;
    const Int32 subCode  =  (Int32) err & GS::ErrorCodeMask;
    if (((Int32) err & GS::ErrorFlagMask) != 0) {
        return GS::UniString::Printf (
            "unknown error %d (module %d%s, sub-code %d) - not in EvP's table; grep "
            "AddOn/reference/archicad29-api-devkit for this module's error header",
            (int) err, (int) moduleId, (moduleId == API_ModuleId) ? " 'API'" : "", (int) subCode);
    }
    return GS::UniString::Printf ("unknown error %d - not in EvP's table", (int) err);
}

// ---------------------------------------------------------------------------

CallScope::CallScope (const GS::UniString& callId, const GS::UniString& command,
                      const GS::UniString& paramsJson)
{
    callStack.push_back (CallFrame { callId, command, paramsJson });
}

CallScope::~CallScope ()
{
    if (!callStack.empty ())
        callStack.pop_back ();
}

// ---------------------------------------------------------------------------

GS::UniString ReportApiFailure (const Site& site, const char* acapiFunction,
                                GSErrCode err, const GS::UniString& context)
{
    const GS::UniString decoded  = DescribeErr (err);
    const GS::UniString function (acapiFunction != nullptr ? acapiFunction : "an ACAPI call");

    // What goes back to the script: the call, the decoded code, and what we were
    // trying to do. Deliberately one line — it ends up in an alert and in the
    // palette status, where nothing longer survives.
    GS::UniString message = GS::UniString::Printf ("%T failed: %T", function.ToPrintf (), decoded.ToPrintf ());
    if (!context.IsEmpty ())
        message += GS::UniString::Printf (" [%T]", context.ToPrintf ());

    GS::UniString body = GS::UniString::Printf ("  %T failed\r\n  %T\r\n  at      %T:%d in %s",
                                                function.ToPrintf (), decoded.ToPrintf (),
                                                BaseName (site.file).ToPrintf (), site.line,
                                                site.function != nullptr ? site.function : "?");
    if (!context.IsEmpty ())
        body += GS::UniString::Printf ("\r\n  while   %T", context.ToPrintf ());

    const GS::UniString callLines = CallFrameLines ();
    if (!callLines.IsEmpty ())
        body += GS::UniString ("\r\n") + callLines;

    WriteBlock (GS::UniString::Printf ("ACAPI  %T", CurrentCommand ().ToPrintf ()), body);
    PushTrail (GS::UniString::Printf ("%T: %T", CurrentCommand ().ToPrintf (), message.ToPrintf ()));
    failureCount.fetch_add (1);

    return message;
}

GS::UniString ReportFailure (const Site& site, const GS::UniString& message,
                             const GS::UniString& context)
{
    GS::UniString full = message;
    if (!context.IsEmpty ())
        full += GS::UniString::Printf (" [%T]", context.ToPrintf ());

    GS::UniString body = GS::UniString::Printf ("  %T\r\n  at      %T:%d in %s",
                                                message.ToPrintf (),
                                                BaseName (site.file).ToPrintf (), site.line,
                                                site.function != nullptr ? site.function : "?");
    if (!context.IsEmpty ())
        body += GS::UniString::Printf ("\r\n  while   %T", context.ToPrintf ());

    const GS::UniString callLines = CallFrameLines ();
    if (!callLines.IsEmpty ())
        body += GS::UniString ("\r\n") + callLines;

    WriteBlock (GS::UniString::Printf ("LOGIC  %T", CurrentCommand ().ToPrintf ()), body);
    PushTrail (GS::UniString::Printf ("%T: %T", CurrentCommand ().ToPrintf (), full.ToPrintf ()));
    failureCount.fetch_add (1);

    return full;
}

void LogEnvelopeFailure (const GS::UniString& command, const GS::UniString& callId,
                         const GS::UniString& code, const GS::UniString& message,
                         const GS::UniString& detail, const GS::UniString& paramsJson)
{
    GS::UniString body = GS::UniString::Printf ("  code    %T\r\n  message %T",
                                                code.ToPrintf (), message.ToPrintf ());
    if (!detail.IsEmpty ())
        body += GS::UniString::Printf ("\r\n  detail  %T", detail.ToPrintf ());
    if (!paramsJson.IsEmpty () && paramsJson != "{}")
        body += GS::UniString::Printf ("\r\n  params  %T", Clip (paramsJson, kParamsEcho).ToPrintf ());

    WriteBlock (GS::UniString::Printf ("ENVELOPE [%T] %T", callId.ToPrintf (), command.ToPrintf ()), body);
    // NOT counted in failureCount and NOT pushed onto the trail: an envelope
    // failure is usually the SAME failure a command already reported, one layer
    // out. Counting it twice would make "one failure" read as two in a probe.
}

GS::UniString ApiErrorLogPath ()
{
    const GS::UniString dataDir = EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\logs\\api_errors.log");
}

GS::Array<GS::UniString> RecentFailures (UInt32 maxCount)
{
    std::lock_guard<std::mutex> lock (trailMutex);

    // Oldest-first regardless of where the ring's write head sits.
    std::vector<GS::UniString> ordered;
    if (trail.size () < kTrailCap) {
        ordered = trail;
    } else {
        for (size_t i = 0; i < kTrailCap; ++i)
            ordered.push_back (trail[(trailNext + i) % kTrailCap]);
    }

    GS::Array<GS::UniString> result;
    const size_t take  = (maxCount == 0 || maxCount >= ordered.size ()) ? ordered.size () : (size_t) maxCount;
    const size_t start = ordered.size () - take;
    for (size_t i = start; i < ordered.size (); ++i)
        result.Push (ordered[i]);
    return result;
}

UInt64 FailureCount ()
{
    return failureCount.load ();
}

GS::UniString FailureTrailBlock (UInt32 maxCount)
{
    const GS::Array<GS::UniString> entries = RecentFailures (maxCount);
    if (entries.IsEmpty ())
        return GS::UniString ();

    GS::UniString block ("--- native failures this session (newest last) ---");
    for (const GS::UniString& entry : entries)
        block += GS::UniString ("\r\n  ") + entry;
    block += GS::UniString ("\r\nfull detail: ") + ApiErrorLogPath ();
    return block;
}

}   // namespace evp
