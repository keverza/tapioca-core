#include "APIEnvir.h"
#include "ACAPinc.h"

#include "AddOnCommands.hpp"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/SchemaValidator.hpp"
#include "NativeCommands/ArchVizCommands.hpp"
#include "NativeCommands/ViewerSyncCommands.hpp"
#include "NativeCommands/AttributeCommands.hpp"
#include "NativeCommands/CaptureCommands.hpp"
#include "NativeCommands/Component3DCommands.hpp"
#include "NativeCommands/CreateCommands.hpp"
#include "NativeCommands/CuttingPlaneCommands.hpp"
#include "NativeCommands/DraftingCommands.hpp"
#include "NativeCommands/DrawingCommands.hpp"
#include "NativeCommands/ElementModifyCommands.hpp"
#include "NativeCommands/ElementReadCommands.hpp"
#include "NativeCommands/FavoriteCommands.hpp"
#include "NativeCommands/GdlPreviewCommands.hpp"
#include "NativeCommands/IdentityCommands.hpp"
#include "NativeCommands/IssueCommands.hpp"
#include "NativeCommands/LayoutCommands.hpp"
#include "NativeCommands/LibraryObjectCommands.hpp"
#include "NativeCommands/ModelAppearanceCommands.hpp"
#include "NativeCommands/ModelGeometryCommands.hpp"
#include "NativeCommands/NotifyCommands.hpp"
#include "NativeCommands/NodeGraphCommands.hpp"
#include "NativeCommands/NodeGraphLibraryCommands.hpp"
#include "NativeCommands/NurbsCommands.hpp"
#include "NativeCommands/PlanGeometryCommands.hpp"
#include "NativeCommands/PlanOverlayCommands.hpp"
#include "NativeCommands/PlanTrackCommands.hpp"
#include "NativeCommands/PointCloudCommands.hpp"
#include "NativeCommands/PreviewCommands.hpp"
#include "NativeCommands/ProjectCommands.hpp"
#include "NativeCommands/QueryCommands.hpp"
#include "NativeCommands/RoofCreateCommands.hpp"
#include "NativeCommands/SelectionCommands.hpp"
#include "NativeCommands/SnapshotCommands.hpp"
#include "NativeCommands/SurfaceCommands.hpp"
#include "NativeCommands/TopologyCommands.hpp"

#include "ObjectState.hpp"
// GSRoot's GS::ProcessControl (progress + TestBreak/cancel) — NOT GSModelDevLib's
// ProcessControl.hpp, which only forward-declares a different class of the same name.
#include "GSProcessControl.hpp"

#include <exception>
#include <memory>

// The native-command registry: the four facade functions from AddOnCommands.hpp
// plus the one place that knows every domain exists.
//
// Adding a command = ONE edit, in its domain's Make<Domain>Command chain.
// Adding a domain  = a new NativeCommands file pair + one line in the array below.
// This file should only ever gain registry lines.
namespace geomsrv {

namespace {

// Every domain's factory, in the order MakeCommand tries them. Order is not
// semantically significant — command names are unique across domains — but a
// stable order keeps dispatch behaviour reproducible.
//
// The signature is deliberately the same for all of them so this stays a data
// table rather than a hand-written if-chain.
using DomainRegistrationProvider = NativeCommandRegistrations (*) ();

constexpr DomainRegistrationProvider domainProviders[] = {
    &GetSnapshotCommandRegistrations,    &GetCaptureCommandRegistrations,       &GetQueryCommandRegistrations,
    &GetElementReadCommandRegistrations, &GetElementModifyCommandRegistrations, &GetIdentityCommandRegistrations,
    &GetAttributeCommandRegistrations,   &GetProjectCommandRegistrations,       &GetCreateCommandRegistrations,
    &GetRoofCreateCommandRegistrations,  &GetDraftingCommandRegistrations,      &GetDrawingCommandRegistrations,
    &GetLayoutCommandRegistrations,      &GetLibraryObjectCommandRegistrations, &GetFavoriteCommandRegistrations,
    &GetSelectionCommandRegistrations,   &GetTopologyCommandRegistrations,      &GetIssueCommandRegistrations,
    &GetSurfaceCommandRegistrations,     &GetModelGeometryCommandRegistrations, &GetModelAppearanceCommandRegistrations,
    &GetNurbsCommandRegistrations,       &GetComponent3DCommandRegistrations,   &GetCuttingPlaneCommandRegistrations,
    &GetNotifyCommandRegistrations,      &GetPlanGeometryCommandRegistrations,  &GetPlanOverlayCommandRegistrations,
    &GetPlanTrackCommandRegistrations,   &GetArchVizCommandRegistrations,       &GetPointCloudCommandRegistrations,
    &GetViewerSyncCommandRegistrations,  &GetPreviewCommandRegistrations,       &GetNodeGraphCommandRegistrations,
    &GetNodeGraphLibraryCommandRegistrations,
    &GetGdlPreviewCommandRegistrations,
};

// ---------------------------------------------------------------------------
// In-process dispatch for evp.api.call — see the header for the main-thread rule.
//
// A fresh instance per call rather than a shared registry: Archicad OWNS the
// instances handed to InstallAddOnCommandHandler, and these commands are
// stateless (they only touch the global stores), so constructing one is
// equivalent and keeps the classes private to their domain TU.
std::unique_ptr<MainThreadCommand> MakeCommand (const GS::String& name)
{
    for (const DomainRegistrationProvider provide : domainProviders) {
        for (const NativeCommandRegistration& registration : provide ()) {
            if (name == registration.name)
                return registration.make (registration);
        }
    }
    return nullptr;
}

} // namespace

GS::Array<NativeCommandInfo> EnumerateNativeCommands ()
{
    GS::Array<NativeCommandInfo> commands;
    for (const DomainRegistrationProvider provide : domainProviders) {
        for (const NativeCommandRegistration& registration : provide ()) {
            const std::unique_ptr<MainThreadCommand> command = registration.make (registration);
            NativeCommandInfo info;
            info.name = registration.name;
            info.isWrite = command->IsWrite ();
            info.isStructural = command->IsStructural ();
            info.needsMainThread = command->NeedsMainThread ();
            info.cancelExempt = registration.cancelExempt;
            info.inputSchema = command->GetInputParametersSchema ();
            info.responseSchema = command->GetResponseSchema ();
            commands.Push (std::move (info));
        }
    }
    return commands;
}

NativeSchemaReport GetNativeSchemaReport ()
{
    NativeSchemaReport report;
    for (const NativeCommandInfo& command : EnumerateNativeCommands ()) {
        ++report.total;
        if (!command.inputSchema.HasValue ())
            report.missingInput.Push (GS::UniString (command.name.ToCStr ()));
        if (!command.responseSchema.HasValue ())
            report.missingResponse.Push (GS::UniString (command.name.ToCStr ()));
        if (command.inputSchema.HasValue () && command.responseSchema.HasValue ())
            ++report.complete;
    }
    return report;
}

bool ValidateNativeCommandCatalog (GS::UniString& error)
{
    GS::Array<GS::String> names;
    for (const DomainRegistrationProvider provide : domainProviders) {
        for (const NativeCommandRegistration& registration : provide ()) {
            if (registration.name == nullptr || registration.name[0] == '\0' || registration.make == nullptr) {
                error = "native command registration has an empty name or maker";
                return false;
            }
            for (const GS::String& existing : names) {
                if (existing == registration.name) {
                    error = "duplicate native command registration: " + GS::UniString (registration.name);
                    return false;
                }
            }
            const std::unique_ptr<MainThreadCommand> command = registration.make (registration);
            if (command->IsWrite () && command->IsStructural ()) {
                error = "native command is both write and structural: " + GS::UniString (registration.name);
                return false;
            }
            names.Push (registration.name);
        }
    }
    return true;
}

bool IsCancelExemptNativeCommand (const GS::String& name)
{
    for (const DomainRegistrationProvider provide : domainProviders) {
        for (const NativeCommandRegistration& registration : provide ()) {
            if (name == registration.name)
                return registration.cancelExempt;
        }
    }
    return false;
}

bool IsWriteCommand (const GS::String& name, bool& isWrite)
{
    const std::unique_ptr<MainThreadCommand> command = MakeCommand (name);
    if (command == nullptr)
        return false;
    isWrite = command->IsWrite ();
    return true;
}

bool IsStructuralCommand (const GS::String& name, bool& isStructural)
{
    const std::unique_ptr<MainThreadCommand> command = MakeCommand (name);
    if (command == nullptr)
        return false;
    isStructural = command->IsStructural ();
    return true;
}

bool CommandNeedsMainThread (const GS::String& name, bool& needsMainThread)
{
    const std::unique_ptr<MainThreadCommand> command = MakeCommand (name);
    if (command == nullptr)
        return false;
    needsMainThread = command->NeedsMainThread ();
    return true;
}

NativeCommandResult ExecuteNativeCommand (const GS::String& name, const GS::ObjectState& params)
{
    NativeCommandResult result;
    const std::unique_ptr<MainThreadCommand> command = MakeCommand (name);
    if (command == nullptr) {
        result.error = "Unknown Tapioca command: " + GS::UniString (name.ToCStr ());
        return result;
    }

    GS::UniString schemaError;
    if (!ValidateObjectStateSchema (params, command->GetInputParametersSchema (), schemaError)) {
        return NativeCommandResult::Failure ("Tapioca." + GS::UniString (name.ToCStr ()) +
                                                 " input schema violation: " + schemaError,
                                             NativeCommandFailureKind::SchemaValidation);
    }

    // Disregards progress feedback, still honours interrupts. The scripted path
    // has no process window; long commands surface progress via the palette.
    GS::ProcessControlInterruptDelegate processControl (nullptr);

    try {
        result = command->ExecuteNative (params, processControl);
    }
    catch (const GS::Exception& ex) {
        // ⚠️ GetMessage() IS OFTEN EMPTY. A GS exception thrown by a failed
        // internal precondition carries no text at all, and reporting it alone
        // produced literally "EvP.GetModelLights threw: " — an error that names
        // nothing, which is the one thing this codebase does not allow. Confirmed
        // live 2026-08-03 on a 3769-element project.
        //
        // Everything else the exception knows is free and is what actually
        // identifies the bug: GetName() is the exception's CLASS, and the file and
        // line are inside the DevKit, i.e. the throwing accessor itself.
        result.error = "Tapioca." + GS::UniString (name.ToCStr ()) + " threw " +
                       GS::UniString (ex.GetName () != nullptr ? ex.GetName () : "GS::Exception");
        const GS::UniString message = ex.GetMessage ();
        result.error += message.IsEmpty () ? GS::UniString (" (no message)") : GS::UniString (": ") + message;
        if (ex.GetErrCode () != NoError)
            result.error += GS::UniString::Printf (" [errCode %d]", (int) ex.GetErrCode ());
        if (ex.GetFileName () != nullptr)
            result.error += GS::UniString::Printf (" at %s:%u", ex.GetFileName (), (unsigned) ex.GetLineNumber ());
        return result;
    }
    catch (const std::exception& ex) {
        // Not every throw in reach is a GS one, and "unknown exception" for a
        // perfectly self-describing std::exception is thrown-away evidence.
        result.error = "Tapioca." + GS::UniString (name.ToCStr ()) +
                       " threw std::exception: " + GS::UniString (ex.what () != nullptr ? ex.what () : "(no message)");
        return result;
    }
    catch (...) {
        result.error = "Tapioca." + GS::UniString (name.ToCStr ()) + " threw an unknown exception.";
        return result;
    }

    if (!result.ok)
        return result;

    if (!ValidateObjectStateSchema (result.data, command->GetResponseSchema (), schemaError)) {
        return NativeCommandResult::Failure ("Tapioca." + GS::UniString (name.ToCStr ()) +
                                                 " response schema violation: " + schemaError,
                                             NativeCommandFailureKind::SchemaValidation);
    }

    return result;
}

// ProjectInfoField (declared in AddOnCommands.hpp) is implemented in
// NativeCommands/ProjectCommands.cpp — its only helper, ReadProjectInfoFields,
// is file-local there.

GSErrCode InstallAddOnCommands ()
{
    GS::UniString catalogError;
    if (!ValidateNativeCommandCatalog (catalogError)) {
        ACAPI_WriteReport ("Tapioca native catalog invalid: %T", true, catalogError.ToPrintf ());
        return APIERR_GENERAL;
    }
    // Installation is per-domain because ACAPI's handler API takes ownership via
    // GS::NewOwned<Concrete>(), which needs the CONCRETE class — and those stay
    // private to their domain .cpp, which is the point of the split. Each
    // function installs only that domain's READ commands.
    const GSErrCode err = InstallSnapshotJsonCommands ();
    if (err != NoError)
        return err;

    GSErrCode err2 = InstallCaptureJsonCommands ();
    if (err2 != NoError)
        return err2;

    return InstallPlanOverlayJsonCommands ();

    // WRITES ARE DELIBERATELY NOT REGISTERED HERE (breaking change, P1 2026-07-17).
    //
    // PlaceLevelDimension and CreateMesh are reachable ONLY through the EvP
    // command bus (evp.api.call / evp.transaction) now. Their Execute is the bare
    // undo-free core — see WriteCommand — so whoever calls it must open the undo
    // scope. Archicad's JSON dispatcher does not, and cannot be told to, so
    // exposing them here would create elements outside any undo scope.
    //
    // This is the intended direction: the command system is the one way scripts
    // write to the project, and there is exactly ONE version of each command.
    // Reads stay on the JSON port — the existing Python toolkit depends on them
    // and they need no undo scope.
}

} // namespace geomsrv
