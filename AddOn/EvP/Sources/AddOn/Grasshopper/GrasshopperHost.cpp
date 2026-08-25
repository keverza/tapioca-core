#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GrasshopperHost.hpp"
#include "GrasshopperHostApi.h"
#include "HostState.hpp"
#include "ManagedRuntime.hpp"
#include "Python/MainThreadGate.hpp" // the main-thread question, already answered once
#include "Python/PathUtils.hpp"      // EvpDataDir / CreateDirectoryChain / AppendTextLine

#include <objbase.h> // CoGetApartmentType — the STA gate below; not pulled in by Win32Interface.hpp

#include <vector>

// ============================================================================
// PLAT-RHINO-INSIDE. Slice 0: hostfxr -> managed bootstrap -> hidden RhinoCore
// -> stock Grasshopper, and back down again cleanly. Slice 1 adds the editor
// on top of exactly that core — OpenEditor starts the host if it is not up and
// then shows the canvas, so there is one runtime no matter which menu item the
// user reaches for, and no path here ever constructs a second one.
//
// The whole file is written so that EVERY failure has a name. A menu command
// that says "could not start Grasshopper" is worth nothing: the four things
// that actually go wrong on a real machine are a missing .NET Desktop Runtime,
// a missing or unlicensed Rhino, a managed assembly that was not redeployed
// after a rebuild, and Grasshopper failing to load after the core came up.
// They have four different fixes, so they get four different messages.
// ============================================================================

namespace evp {
namespace grasshopper {

namespace {

HostLifecycle lifecycle;
ManagedRuntime runtime;

TapiocaGhStartFn managedStart = nullptr;
TapiocaGhStopFn managedStop = nullptr;
TapiocaGhStateFn managedState = nullptr;
TapiocaGhCopyLastMessageFn managedCopyLastMessage = nullptr;
TapiocaGhShowEditorFn managedShowEditor = nullptr;
TapiocaGhHideEditorFn managedHideEditor = nullptr;

GS::UniString lastMessage;
GS::UniString runtimeDescription;
GS::UniString assemblyDirectory;

GS::UniString FromWide (const std::wstring& text)
{
    if (text.empty ())
        return GS::UniString ();
    return GS::UniString (text.c_str ());
}

std::wstring ToWide (const GS::UniString& text)
{
    return std::wstring ((const wchar_t*) text.ToUStr ().Get ());
}

// logs\grasshopper.log, next to startup.log and archviz.log. One line per step
// of the startup order, because the interesting failures here are the ones that
// take the process down before anything can be returned — the same argument
// PathUtils::StartupTrace makes, applied to a runtime that is worse at dying.
GS::UniString LogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    const GS::UniString logs (dataDir + GS::UniString ("\\logs"));
    if (!evp::CreateDirectoryChain (logs))
        return GS::UniString ();
    return logs + GS::UniString ("\\grasshopper.log");
}

void Log (const GS::UniString& line)
{
    const GS::UniString path = LogPath ();
    if (!path.IsEmpty ())
        evp::AppendTextLine (path, line);
}

// ---- The crash-loop breadcrumb ---------------------------------------------
// Same mechanism, and the same reasoning, as ArchViz's ExperimentGuard
// (PLAT-RE81): a file that exists only WHILE the dangerous call is in flight.
// Written and flushed before the call, deleted after it returns by any path.
// Found at the next start, it can only mean one thing — the previous attempt
// did not return, i.e. it took the process down with it.
//
// This exists because starting a foreign runtime is the one thing in this
// add-on that can kill Archicad without leaving a catchable error: an
// AccessViolation inside Rhino is a corrupted-state exception that .NET Core
// refuses to let anyone catch, so there is no return value to inspect and no
// dialog to show. The breadcrumb is the only evidence that survives, and
// refusing the second click is the only thing that stops a user from crashing
// Archicad twice in a row trying to work out what happened.
GS::UniString ArmMarkerPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\GRASSHOPPER_START_ARMED");
}

void Arm ()
{
    const GS::UniString path = ArmMarkerPath ();
    if (path.IsEmpty ())
        return;
    GS::UniString error;
    evp::WriteTextFile (path, GS::UniString ("Rhino.Inside start in flight.\n"), error);
}

void Disarm ()
{
    const GS::UniString path = ArmMarkerPath ();
    if (!path.IsEmpty ())
        DeleteFileW ((LPCWSTR) path.ToUStr ().Get ());
}

bool WasArmed ()
{
    const GS::UniString path = ArmMarkerPath ();
    return !path.IsEmpty () && evp::PathExists (path);
}

// The .apx's own directory. The managed host is staged beside it by the build,
// so this is where the assembly, its .deps.json and its .runtimeconfig.json are
// looked for — never the process directory, which is Archicad's.
bool OwnDirectory (std::wstring& dir, GS::UniString& error)
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) &Log, &self) == 0) {
        error = "GetModuleHandleEx failed for the add-on module.";
        return false;
    }

    std::vector<wchar_t> buffer (MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW (self, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
        if (written == 0) {
            error = "GetModuleFileName failed for the add-on module.";
            return false;
        }
        if (written < buffer.size () - 1)
            break;
        buffer.resize (buffer.size () * 2);
    }

    std::wstring path (buffer.data ());
    const size_t separator = path.find_last_of (L'\\');
    if (separator == std::wstring::npos) {
        error = "The add-on module path has no directory component.";
        return false;
    }
    dir = path.substr (0, separator);
    return true;
}

// ⚠️ THE STA GATE. RhinoCore is thread-affine and expects a single-threaded
// apartment; constructing it on an MTA thread is not a supported configuration
// and misbehaves later rather than failing here. Archicad's main thread is
// expected to be an STA, but "expected" is not evidence, so the apartment is
// MEASURED and recorded — that is the probe's "verified STA owner thread" gate.
//
// ole32 is resolved dynamically rather than linked: every Windows process that
// has a window already has it loaded, and a new import in the .apx for one
// diagnostic call is a worse trade than a GetProcAddress.
enum class Apartment { Sta, Mta, Uninitialized, Unknown };

Apartment CurrentApartment ()
{
    typedef HRESULT (__stdcall * CoGetApartmentTypeFn) (APTTYPE*, APTTYPEQUALIFIER*);
    const HMODULE ole32 = GetModuleHandleW (L"ole32.dll");
    if (ole32 == nullptr)
        return Apartment::Unknown;
    const CoGetApartmentTypeFn query = (CoGetApartmentTypeFn) GetProcAddress (ole32, "CoGetApartmentType");
    if (query == nullptr)
        return Apartment::Unknown;

    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    const HRESULT result = query (&type, &qualifier);
    if (result == CO_E_NOTINITIALIZED)
        return Apartment::Uninitialized;
    if (FAILED (result))
        return Apartment::Unknown;
    if (type == APTTYPE_STA || type == APTTYPE_MAINSTA)
        return Apartment::Sta;
    if (type == APTTYPE_MTA)
        return Apartment::Mta;
    return Apartment::Unknown;
}

const char* DescribeApartment (Apartment apartment)
{
    switch (apartment) {
        case Apartment::Sta:
            return "STA";
        case Apartment::Mta:
            return "MTA";
        case Apartment::Uninitialized:
            return "COM not initialized on this thread";
        case Apartment::Unknown:
            break;
    }
    return "unknown";
}

// ⚠️ THE OPENNURBS COLLISION. THIS CHECK IS WHY THE MENU ITEM NO LONGER TAKES
// ARCHICAD DOWN, AND IT MUST NOT BE REMOVED AS "DEFENSIVE".
//
// Archicad 29 SHIPS ITS OWN opennurbs.dll (8.4.24044.15000 as measured), and it
// is in the process from startup: Archicad loads every add-on to ask it
// CheckEnvironment, and Add-Ons\Import-Export\Rhino_In.apx and Rhino_Out.apx —
// Archicad's own 3DM import/export — statically import opennurbs.dll, so the
// loader brings it in with them. Rhino 8's own opennurbs.dll is the SAME BASE
// NAME, 27 service releases newer (8.31.26126.13431).
//
// The binding happens through STATIC IMPORTS, and that distinction matters
// because it is what makes the problem unfixable from here. An explicit
// LoadLibrary of a full path does get its own copy — measured, two distinct
// module handles. But RhinoLibrary.dll, which is the FIRST thing the
// Rhino.Inside resolver loads, imports "opennurbs.dll" by bare name in its
// import table (so do RhinoCore.dll, rhcommon_c.dll and seven others), and the
// loader satisfies a bare-name import from an already-resident module without
// consulting any search path. So RhinoLibrary binds to ARCHICAD'S opennurbs no
// matter what directory Rhino was loaded from; LOAD_WITH_ALTERED_SEARCH_PATH
// cannot reach it, and neither can anything else on our side of the boundary.
// Rhino then calls a 27-releases-older ABI and access-violates deep inside
// Rhino.Runtime.InProcess.Interop.StartupInProcess.
//
// That crash is NOT survivable in managed code: an AccessViolationException is a
// corrupted-state exception, and .NET Core does not let anyone catch it — the
// runtime terminates the process. Archicad dies with the user's unsaved project
// in it. So the ONLY protection is to never make the call, which is what this
// does: it asks, natively and before any Rhino code runs, whether a foreign
// opennurbs is already resident.
//
// The discriminator is RhinoLibrary.dll rather than a version number or a path
// pattern: Rhino's System directory always contains it and no other opennurbs
// host does, so this stays correct across Rhino service releases, non-default
// install locations, and whatever Archicad ships next.
bool ForeignOpenNurbsLoaded (GS::UniString& detail)
{
    const HMODULE loaded = GetModuleHandleW (L"opennurbs.dll");
    if (loaded == nullptr)
        return false; // nothing resident; Rhino's own copy will resolve cleanly

    std::vector<wchar_t> buffer (MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW (loaded, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
        if (written == 0) {
            detail = "an opennurbs.dll is loaded in this process but its path could not be read";
            return true; // resident and unidentifiable is still a refusal
        }
        if (written < buffer.size () - 1)
            break;
        buffer.resize (buffer.size () * 2);
    }

    const std::wstring path (buffer.data ());
    const size_t separator = path.find_last_of (L'\\');
    if (separator == std::wstring::npos) {
        detail = FromWide (path);
        return true;
    }

    const std::wstring sibling = path.substr (0, separator) + L"\\RhinoLibrary.dll";
    if (GetFileAttributesW ((LPCWSTR) sibling.c_str ()) != INVALID_FILE_ATTRIBUTES)
        return false; // it is Rhino's own, next to RhinoLibrary.dll — the good case

    detail = FromWide (path);
    return true;
}

// The managed side's own account of what just happened, copied into memory we
// own. Never a pointer handed back across the boundary — see GrasshopperHostApi.h.
GS::UniString ManagedMessage ()
{
    if (managedCopyLastMessage == nullptr)
        return GS::UniString ();
    const int32_t needed = managedCopyLastMessage (nullptr, 0);
    if (needed <= 0)
        return GS::UniString ();
    std::vector<uint16_t> buffer ((size_t) needed + 1, 0);
    const int32_t written = managedCopyLastMessage (buffer.data (), (int32_t) buffer.size ());
    if (written <= 0)
        return GS::UniString ();
    return GS::UniString ((const wchar_t*) buffer.data ());
}

GS::UniString DescribeStatus (int32_t status)
{
    switch (status) {
        case TapiocaGhStatus_Ok:
            return "started";
        case TapiocaGhStatus_AlreadyRunning:
            return "already running";
        case TapiocaGhStatus_StartInProgress:
            return "another start is already in progress";
        case TapiocaGhStatus_Terminal:
            return "the host was stopped in this session and cannot be restarted; restart Archicad";
        case TapiocaGhStatus_NotRunning:
            return "the host is not running";
        case TapiocaGhStatus_AbiMismatch:
            return "the managed host was built against a different add-on ABI; rebuild and redeploy the add-on";
        case TapiocaGhStatus_WrongThread:
            return "the host was called from the wrong thread";
        case TapiocaGhStatus_RuntimeMissing:
            return "no compatible .NET runtime was found";
        case TapiocaGhStatus_AssemblyMissing:
            return "the managed host assembly was not deployed beside the add-on";
        case TapiocaGhStatus_RhinoMissing:
            return "no Rhino 8 installation was found; Tapioca does not install Rhino";
        case TapiocaGhStatus_RhinoInitFailed:
            return "Rhino would not start in this process";
        case TapiocaGhStatus_LicenceUnavailable:
            return "Rhino is installed but no licence is available";
        case TapiocaGhStatus_GrasshopperFailed:
            return "Rhino started but Grasshopper would not load";
        case TapiocaGhStatus_Faulted:
            return "the managed host threw an exception";
        case TapiocaGhStatus_EditorUnavailable:
            return "Rhino is running but Grasshopper's editor is not available";
        default:
            break;
    }
    return GS::UniString::Printf ("unrecognized status %d", (int) status);
}

// Resolving all four entry points before calling any of them is deliberate: a
// host that can Start but not Stop is worse than one that never started, because
// it is Archicad's quit path that pays for it.
bool ResolveEntryPoints (const std::wstring& assemblyPath, GS::UniString& error)
{
    struct Entry {
        const wchar_t* method;
        void** slot;
    };
    void* startSlot = nullptr;
    void* stopSlot = nullptr;
    void* stateSlot = nullptr;
    void* messageSlot = nullptr;
    void* showEditorSlot = nullptr;
    void* hideEditorSlot = nullptr;
    const Entry entries[] = {
        { L"Start", &startSlot },           { L"Stop", &stopSlot },
        { L"State", &stateSlot },           { L"CopyLastMessage", &messageSlot },
        { L"ShowEditor", &showEditorSlot }, { L"HideEditor", &hideEditorSlot },
    };

    for (size_t index = 0; index < sizeof (entries) / sizeof (entries[0]); ++index) {
        std::wstring failure;
        if (!runtime.Resolve (assemblyPath, TAPIOCA_GH_TYPE, entries[index].method, entries[index].slot, failure)) {
            error = FromWide (failure);
            return false;
        }
    }

    managedStart = (TapiocaGhStartFn) startSlot;
    managedStop = (TapiocaGhStopFn) stopSlot;
    managedState = (TapiocaGhStateFn) stateSlot;
    managedCopyLastMessage = (TapiocaGhCopyLastMessageFn) messageSlot;
    managedShowEditor = (TapiocaGhShowEditorFn) showEditorSlot;
    managedHideEditor = (TapiocaGhHideEditorFn) hideEditorSlot;
    return true;
}

void ForgetEntryPoints ()
{
    managedStart = nullptr;
    managedStop = nullptr;
    managedState = nullptr;
    managedCopyLastMessage = nullptr;
    managedShowEditor = nullptr;
    managedHideEditor = nullptr;
}

} // namespace

GrasshopperHost& GrasshopperHost::Get ()
{
    static GrasshopperHost instance;
    return instance;
}

bool GrasshopperHost::Start (GS::UniString& message)
{
    if (!evp::MainThreadGate::Get ().IsMainThread ()) {
        message = "Rhino.Inside can only be started from Archicad's main thread.";
        Log (message);
        return false;
    }

    const StartDecision decision = lifecycle.BeginStart ();
    switch (decision) {
        case StartDecision::AlreadyRunning:
            message = "Rhino.Inside is already running. " + lastMessage;
            return true;
        case StartDecision::InProgress:
            message = "Rhino.Inside is already starting.";
            return false;
        case StartDecision::Terminal:
            message = "Rhino.Inside was stopped in this Archicad session and cannot be started again. "
                      "Restart Archicad to host Rhino once more.";
            Log (message);
            return false;
        case StartDecision::Proceed:
            break;
    }

    Log (GS::UniString ("===== Rhino.Inside start ====="));

    const Apartment apartment = CurrentApartment ();
    Log (GS::UniString::Printf ("apartment: %s", DescribeApartment (apartment)));
    if (apartment == Apartment::Mta) {
        message = "Archicad's main thread is a multi-threaded apartment; RhinoCore requires an STA and was "
                  "not started.";
        lifecycle.FailStart (std::string ("MTA main thread"));
        lastMessage = message;
        Log (message);
        return false;
    }

    // The previous attempt in some earlier session never returned. Refusing is
    // the whole point: the alternative is crashing Archicad again to find out.
    if (WasArmed ()) {
        message = GS::UniString ("A previous Rhino.Inside start did not return — it took Archicad down. "
                                 "Rhino.Inside is refusing to try again until that is cleared.\n\n"
                                 "Check logs\\grasshopper.log for how far it got, then delete\n  ") +
                  ArmMarkerPath () + GS::UniString ("\nto re-arm it.");
        lifecycle.FailStart (std::string ("previous start crashed"));
        lastMessage = message;
        Log (message);
        return false;
    }

    // ⚠️ BEFORE ANY RHINO CODE RUNS. See ForeignOpenNurbsLoaded: past this point
    // a mismatched opennurbs is an access violation nothing can catch.
    GS::UniString conflict;
    if (ForeignOpenNurbsLoaded (conflict)) {
        message = GS::UniString ("Rhino.Inside cannot start: this Archicad process has already loaded a different "
                                 "copy of opennurbs.dll, and Rhino would bind to it and crash.\n\nLoaded: ") +
                  conflict +
                  // ⚠️ "Show hidden add-ons" IS THE LOAD-BEARING HALF OF THIS MESSAGE.
                  // Rhino_In and Rhino_Out are hidden entries: the Add-On Manager does not
                  // list them at all until that box is ticked, so a user told only to
                  // "disable them" opens the dialog, cannot find them, and reasonably
                  // concludes the instruction is wrong. Confirmed live — disabling both is
                  // what makes Rhino.Inside start.
                  GS::UniString ("\n\nIt is Archicad's own 3DM import/export that brings it in. In "
                                 "Options > Add-On Manager, tick \"Show hidden add-ons\" (Rhino_In and "
                                 "Rhino_Out are hidden and are not listed without it), disable both, "
                                 "restart Archicad, and try again.");
        lifecycle.FailStart (std::string ("conflicting opennurbs.dll"));
        lastMessage = message;
        Log (message);
        return false;
    }

    std::wstring directory;
    GS::UniString error;
    if (!OwnDirectory (directory, error)) {
        message = error;
        lifecycle.FailStart (std::string ("add-on directory unknown"));
        lastMessage = message;
        Log (message);
        return false;
    }
    assemblyDirectory = FromWide (directory);

    const std::wstring assemblyPath = directory + L"\\" + std::wstring (TAPIOCA_GH_ASSEMBLY);
    const std::wstring configPath = directory + L"\\Tapioca.GrasshopperHost.runtimeconfig.json";

    std::wstring failure;
    if (!runtime.Start (configPath, failure)) {
        message = FromWide (failure);
        lifecycle.FailStart (std::string ("runtime start failed"));
        lastMessage = message;
        Log (message);
        return false;
    }
    runtimeDescription = FromWide (runtime.Description ());
    Log (runtimeDescription);

    if (!ResolveEntryPoints (assemblyPath, error)) {
        message = error;
        lifecycle.FailStart (std::string ("entry points unresolved"));
        lastMessage = message;
        Log (message);
        // The runtime stays loaded on purpose: it is process-wide and cannot be
        // undone, and pretending otherwise by closing the context would only
        // hide that fact from the next attempt.
        return false;
    }
    Log (GS::UniString ("managed entry points resolved"));

    const std::wstring logPath = ToWide (LogPath ());

    TapiocaGhStartRequest request;
    request.structSize = (uint32_t) sizeof (TapiocaGhStartRequest);
    request.abiVersion = TAPIOCA_GH_ABI_VERSION;
    // P0 loads Grasshopper but never SHOWS it: the editor window is P1's, and
    // starting it here would make the lifecycle evidence depend on a UI surface
    // that has not been designed yet.
    request.flags = TAPIOCA_GH_FLAG_LOAD_GRASSHOPPER;
    request.reserved = 0;
    request.rhinoSystemDir = nullptr; // let the Rhino.Inside resolver find it
    request.logPath = logPath.empty () ? nullptr : (const uint16_t*) logPath.c_str ();

    // This call is synchronous on the main thread, and stays that way
    // deliberately: RhinoCore is affine to the STA that constructs it, so moving
    // the construction to a worker would move Rhino's ownership off Archicad's
    // main thread and break the rule the whole host is built on. Grasshopper
    // loads at its normal speed — measured in Archicad, this is not a long wait.
    //
    // ⚠️ ONE THING TO KNOW BEFORE "FIXING" AN APPARENT HANG HERE. The menu
    // command runs while Archicad's menu is still tracking, so nothing visibly
    // happens until the menu closes. That reads exactly like a freeze, and it
    // was misread as one once already. The work is not slow; it has not started.
    Log (GS::UniString ("calling managed Start"));
    Arm ();
    const int32_t status = managedStart (&request);
    Disarm (); // reached only if the call returned at all — which is the point
    const GS::UniString managedText = ManagedMessage ();
    const bool started = (status == TapiocaGhStatus_Ok || status == TapiocaGhStatus_AlreadyRunning);

    message = DescribeStatus (status);
    if (!managedText.IsEmpty ())
        message += GS::UniString (": ") + managedText;

    if (started) {
        lifecycle.CompleteStart ();
        Log (GS::UniString ("Rhino.Inside running. ") + message);
    }
    else {
        lifecycle.FailStart (std::string ("managed start failed"));
        Log (GS::UniString ("Rhino.Inside failed to start. ") + message);
    }
    lastMessage = message;
    return started;
}

bool GrasshopperHost::OpenEditor (GS::UniString& message)
{
    // Start-if-needed lives HERE and not in the managed half, because this side
    // owns the lifecycle state machine, the preflight checks and the crash
    // breadcrumb. A managed shortcut would be a second, weaker start path that
    // skipped all three.
    if (!IsRunning ()) {
        if (!Start (message))
            return false;
    }

    if (managedShowEditor == nullptr) {
        message = "The Grasshopper editor entry point is not available; rebuild and redeploy the add-on.";
        Log (message);
        return false;
    }

    const int32_t status = managedShowEditor ();
    const GS::UniString managedText = ManagedMessage ();
    message = DescribeStatus (status);
    if (!managedText.IsEmpty ())
        message += GS::UniString (": ") + managedText;
    lastMessage = message;
    Log (GS::UniString ("show editor -> ") + message);
    return status == TapiocaGhStatus_Ok;
}

bool GrasshopperHost::HideEditor (GS::UniString& message)
{
    // Deliberately does NOT start anything: "hide the canvas" is already true
    // when there is no canvas, and starting Rhino to satisfy a request to see
    // less of it would be absurd.
    if (!IsRunning () || managedHideEditor == nullptr) {
        message = "Grasshopper is not running.";
        return true;
    }

    const int32_t status = managedHideEditor ();
    const GS::UniString managedText = ManagedMessage ();
    message = DescribeStatus (status);
    if (!managedText.IsEmpty ())
        message += GS::UniString (": ") + managedText;
    lastMessage = message;
    Log (GS::UniString ("hide editor -> ") + message);
    return status == TapiocaGhStatus_Ok;
}

void GrasshopperHost::Stop ()
{
    if (!lifecycle.BeginStop ()) {
        // Nothing running. Still drop the entry points and the host context if a
        // failed start left them behind, so that nothing of ours is reachable
        // from the runtime after this returns.
        ForgetEntryPoints ();
        runtime.Close ();
        return;
    }

    Log (GS::UniString ("===== Rhino.Inside stop ====="));
    if (managedStop != nullptr) {
        const int32_t status = managedStop ();
        const GS::UniString managedText = ManagedMessage ();
        Log (DescribeStatus (status) +
             (managedText.IsEmpty () ? GS::UniString () : GS::UniString (": ") + managedText));
    }

    // ⚠️ ORDER. The managed session detaches its own handlers first (above),
    // THEN the native side drops every pointer the runtime could still call
    // through, and only then is the host context released. Reversing any pair
    // leaves a live callback into a module that is about to unload — the exact
    // hazard FreeData spends its whole body avoiding for Win32 hooks.
    ForgetEntryPoints ();
    runtime.Close ();
    lifecycle.CompleteStop ();
    Log (GS::UniString ("Rhino.Inside stopped. The CLR stays in this process; Archicad must be restarted "
                        "before Rhino can be hosted again."));
}

bool GrasshopperHost::IsRunning () const
{
    return lifecycle.IsRunning ();
}

HostState GrasshopperHost::State () const
{
    return lifecycle.State ();
}

GS::UniString GrasshopperHost::Describe () const
{
    GS::UniString text = GS::UniString::Printf ("Rhino.Inside host: %s", DescribeHostState (lifecycle.State ()));

    // The two sides' opinions of the state, side by side and never merged: a
    // disagreement is the first symptom of a half-torn-down host, and averaging
    // them into one line would hide exactly the case worth seeing.
    if (managedState != nullptr)
        text += GS::UniString::Printf ("\nManaged session state: %d", (int) managedState ());

    if (!runtimeDescription.IsEmpty ())
        text += GS::UniString ("\nRuntime: ") + runtimeDescription;
    else
        text += GS::UniString ("\nRuntime: not loaded");

    if (!assemblyDirectory.IsEmpty ())
        text += GS::UniString ("\nAdd-on directory: ") + assemblyDirectory;

    const std::string failure = lifecycle.LastError ();
    if (!failure.empty ())
        text += GS::UniString ("\nLast failure: ") + GS::UniString (failure.c_str ());
    if (!lastMessage.IsEmpty ())
        text += GS::UniString ("\nLast message: ") + lastMessage;

    const GS::UniString logPath = LogPath ();
    if (!logPath.IsEmpty ())
        text += GS::UniString ("\nLog: ") + logPath;
    return text;
}

void GrasshopperHost::OpenEditorFromMenu ()
{
    GrasshopperHost& host = Get ();
    GS::UniString message;
    if (host.OpenEditor (message))
        return; // the canvas is on screen; that IS the feedback, so no dialog

    // Only failures get a dialog here, unlike the Rhino.Inside item: that one
    // has nothing to show on success, this one has a Grasshopper window.
    const GS::UniString report = GS::UniString ("The Grasshopper editor did not open.\n\n") + message +
                                 GS::UniString ("\n\n") + host.Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

void GrasshopperHost::OpenFromMenu ()
{
    GrasshopperHost& host = Get ();
    GS::UniString message;
    const bool running = host.Start (message);

    // One dialog, always — including on success. This menu command has no
    // window of its own in P0 (the editor is P1), so without a report a
    // successful start is indistinguishable from a menu item that does nothing,
    // which is precisely the confusion the startup log exists to prevent.
    const GS::UniString report =
        (running ? GS::UniString ("Rhino.Inside is running.\n\n") : GS::UniString ("Rhino.Inside did not start.\n\n")) +
        message + GS::UniString ("\n\n") + host.Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

} // namespace grasshopper
} // namespace evp
