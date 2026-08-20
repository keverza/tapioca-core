#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Notify/ModelDiff.hpp"

#include "Diagnostics/ApiError.hpp"     // EVP_ACAPI_FAIL — never print a bare GSErrCode

#include <chrono>

namespace geomsrv {
namespace modeldiff {

namespace {

API_ElemDifferenceGeneratorTypeID ToApiType (Scope scope)
{
    return (scope == Scope::File) ? APIDiff_ModificationStampBased : APIDiff_3DModelBased;
}

std::string GuidText (const API_Guid& guid)
{
    return std::string (APIGuidToString (guid).ToCStr ().Get ());
}

}   // namespace

struct Baseline::Impl {
    // ⚠️ HELD BY VALUE, AND THAT IS SAFE ONLY BECAUSE THE DEVKIT SAYS SO.
    // API_ElemDifferenceGeneratorState owns its GSHandle: the destructor calls
    // BMKillHandle and operator= deep-copies through BMHandleToHandle
    // (APIdefs_ElementDifferenceGenerator.h). So adopting a new baseline frees
    // the old handle by itself, and nothing here needs a manual dispose.
    API_ElemDifferenceGeneratorState state;
    bool  haveState = false;
    Scope scope     = Scope::Model;
};

Baseline::Baseline (Scope scope) : impl_ (std::make_unique<Impl> ())
{
    impl_->scope = scope;
}

Baseline::~Baseline () = default;

void Baseline::SetScope (Scope scope)
{
    if (impl_->scope == scope)
        return;
    impl_->scope     = scope;
    // The two generators do not produce comparable states — see the header.
    impl_->haveState = false;
}

Scope Baseline::GetScope () const { return impl_->scope; }

Result Baseline::Poll (bool reset)
{
    Result result;
    const auto started = std::chrono::steady_clock::now ();

    const API_ElemDifferenceGeneratorTypeID type = ToApiType (impl_->scope);

    // ⚠️ CAPTURE FIRST, DIFF SECOND, ADOPT LAST. The capture is what the NEXT
    // call compares against, and taking it after the diff would leave a window
    // in which an edit lands between the two and is reported by neither call.
    API_ElemDifferenceGeneratorState current;
    current.stateType = APIDiffState_InMemory;
    const GSErrCode stateErr = ACAPI_DifferenceGenerator_GetState (type, &current);
    if (stateErr != NoError) {
        result.error = EVP_ACAPI_FAIL ("ACAPI_DifferenceGenerator_GetState", stateErr,
                                       "capturing the current model state for a difference")
                           .ToCStr ()
                           .Get ();
        return result;
    }

    const bool first = (!impl_->haveState || reset);

    if (!first) {
        API_ElemDifference difference;
        // The CURRENT project, not our freshly captured copy: the generator is
        // asked to compare the stored baseline against Archicad as it stands.
        API_ElemDifferenceGeneratorState now;
        now.stateType = APIDiffState_CurrentProject;

        const GSErrCode diffErr = ACAPI_DifferenceGenerator_GenerateDifference (
            type, &impl_->state, &now, difference);
        if (diffErr != NoError) {
            result.error = EVP_ACAPI_FAIL ("ACAPI_DifferenceGenerator_GenerateDifference",
                                           diffErr,
                                           "diffing the model against the previous state")
                               .ToCStr ()
                               .Get ();
            return result;
        }

        for (const API_Guid& g : difference.newElements)
            result.created.push_back (GuidText (g));
        for (const API_Guid& g : difference.modifiedElements)
            result.modified.push_back (GuidText (g));
        for (const API_Guid& g : difference.deletedElements)
            result.deleted.push_back (GuidText (g));
        result.environmentChanged = difference.isEnvironmentChanged;
    }

    impl_->state     = current;     // deep copy; frees the previous handle
    impl_->haveState = true;

    result.ok        = true;
    result.firstCall = first;
    result.elapsedMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - started)
                           .count ();
    return result;
}

}   // namespace modeldiff
}   // namespace geomsrv
