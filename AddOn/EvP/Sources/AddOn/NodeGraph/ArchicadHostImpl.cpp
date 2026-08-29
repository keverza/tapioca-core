#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NodeGraph/ArchicadHostImpl.hpp"

#include "Geometry/GeometryExtractor.hpp" // ResolveSelectableOwner
#include "Notify/ChangeTracker.hpp"
#include "Python/MainThreadGate.hpp"
#include "Server/ServerState.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// The ONLY translation unit in the graph runtime that includes ACAPI.
//
// Everything above IArchicadHost is DevKit-free and covered by the offline
// suite; this file is the part that cannot be, so it is kept small and it does
// nothing clever. Two hazards it exists to contain, both learned the hard way
// elsewhere in this repository:
//
//  * ACAPI_Selection_Get ALLOCATES THE MARQUEE HANDLE even when nothing is
//    selected, and it is ours to free. ArchViz/SelectionBridge.cpp carries the
//    same note; a leak here is once per evaluation rather than once per frame,
//    but it is the same leak.
//  * A GPU or sub-part guid is not selectable. Columns, railings, curtain walls
//    and stairs enumerate as their sub-parts, and
//    ACAPI_Selection_SetSelectedElementNeig refuses those.
//    GeometryExtractor::ResolveSelectableOwner walks back up to the owner, and
//    reusing it is why Set Selection works on those four types.
//
// Every ACAPI call below runs inside MainThreadGate, and every gate lambda
// captures BY VALUE - the gate's own header explains why a by-reference capture
// is a use-after-free that only fires when the gate is slow.

namespace evp::nodegraph {
namespace {

constexpr int kGateTimeoutMs = evp::MainThreadGate::DefaultTimeoutMs;

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

bool ProjectIsOpen ()
{
    return geomsrv::ServerState::Get ().modelOpen.load ();
}

// Runs `work` on Archicad's thread. Returns false with `error` set when the gate
// could not deliver it, which is a real outcome and not an exception.
bool OnHostThread (const std::function<void ()>& work, std::string& error)
{
    GS::UniString gateError;
    if (evp::MainThreadGate::Get ().Invoke (work, kGateTimeoutMs, gateError)) {
        return true;
    }
    error = gateError.IsEmpty () ? std::string ("Archicad did not respond") : Utf8 (gateError);
    return false;
}

// MAIN THREAD. The current selection as guid strings, in Archicad's order.
void ReadSelectionOnHostThread (std::vector<std::string>& out)
{
    API_SelectionInfo info = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false);
    // Ours to free whether or not anything was selected, and whether or not the
    // call succeeded.
    if (info.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
    // APIERR_NOSEL among others means "nothing is selected", which is an answer,
    // not a failure. A graph asking what is selected when nothing is has a
    // correct empty result.
    if (err != NoError)
        return;
    for (UInt32 i = 0; i < neigs.GetSize (); ++i)
        out.push_back (Utf8 (APIGuidToString (neigs[i].guid)));
}

} // namespace

// --- generations ------------------------------------------------------------

bool ArchicadGenerationSource::Sample (GenerationDomain domain, uint64_t& value, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    switch (domain) {
        case GenerationDomain::Project: {
            // ChangeTracker's token already is a model generation, maintained on
            // Archicad's thread with no ACAPI call to read it. Deliberately
            // CONSERVATIVE: it also moves for element edits and for Tapioca's own
            // writes, so a selection node may re-run when nothing it reads
            // changed. That costs one cheap re-read; the opposite error - a node
            // serving a stale answer after the model moved - is the one that
            // makes a BIM graph untrustworthy.
            value = geomsrv::ChangeTracker::Get ().Token ();
            return true;
        }

        case GenerationDomain::Selection: {
            // A hash of the current selection rather than a counter. Nothing in
            // Archicad notifies on selection change, and polling for one would
            // need a timer and a lifecycle; hashing the list is one batched read
            // that is exactly right by construction - equal selections hash
            // equal, so an unchanged selection is a cache hit.
            auto guids = std::make_shared<std::vector<std::string>> ();
            if (!OnHostThread ([guids] { ReadSelectionOnHostThread (*guids); }, error))
                return false;

            uint64_t hash = 1469598103934665603ULL; // FNV-1a, order-sensitive on purpose
            for (const std::string& guid : *guids) {
                for (const char character : guid) {
                    hash ^= static_cast<unsigned char> (character);
                    hash *= 1099511628211ULL;
                }
                hash ^= 0xFFULL;
                hash *= 1099511628211ULL;
            }
            value = hash;
            return true;
        }
    }

    error = "unknown generation domain";
    return false;
}

// --- references -------------------------------------------------------------

ReferenceResolution ArchicadReferenceResolver::Resolve (const Reference& reference) const
{
    return ResolveAll ({ reference }).front ();
}

std::vector<ReferenceResolution> ArchicadReferenceResolver::ResolveAll (const std::vector<Reference>& references) const
{
    std::vector<ReferenceResolution> resolutions (references.size ());

    if (references.empty ())
        return resolutions;

    if (!ProjectIsOpen ()) {
        for (ReferenceResolution& resolution : resolutions) {
            resolution.status = ResolutionStatus::Missing;
            resolution.detail = "no project is open";
        }
        return resolutions;
    }

    // ONE crossing for the whole batch. Per-reference would be one ~3ms round
    // trip per element.
    auto ids = std::make_shared<std::vector<std::string>> ();
    auto kinds = std::make_shared<std::vector<ReferenceKind>> ();
    for (const Reference& reference : references) {
        ids->push_back (reference.id);
        kinds->push_back (reference.kind);
    }
    auto results = std::make_shared<std::vector<ReferenceResolution>> (references.size ());

    std::string gateError;
    const bool delivered = OnHostThread (
        [ids, kinds, results] {
            for (size_t i = 0; i < ids->size (); ++i) {
                ReferenceResolution& resolution = (*results)[i];
                if ((*kinds)[i] != ReferenceKind::Element) {
                    resolution.status = ResolutionStatus::Incompatible;
                    resolution.detail = "only element references can be resolved by this build";
                    continue;
                }
                const API_Guid guid = APIGuidFromString ((*ids)[i].c_str ());
                if (guid == APINULLGuid) {
                    resolution.status = ResolutionStatus::Missing;
                    resolution.detail = "'" + (*ids)[i] + "' is not a valid element identifier";
                    continue;
                }
                API_Elem_Head head = {};
                head.guid = guid;
                if (ACAPI_Element_GetHeader (&head) != NoError) {
                    // Deleted, never existed, or outside this user's Teamwork
                    // workspace. All three are Missing from the graph's point of
                    // view, and the message says so without guessing which.
                    resolution.status = ResolutionStatus::Missing;
                    resolution.detail = "element " + (*ids)[i] +
                                        " is not in this project - it may have been deleted, or it may belong to "
                                        "another user's Teamwork workspace";
                    continue;
                }
                resolution.status = ResolutionStatus::Resolved;
            }
        },
        gateError);

    if (!delivered) {
        for (ReferenceResolution& resolution : resolutions) {
            resolution.status = ResolutionStatus::Missing;
            resolution.detail = gateError;
        }
        return resolutions;
    }
    return *results;
}

// --- host -------------------------------------------------------------------

bool ArchicadHostImpl::IsAvailable () const
{
    return ProjectIsOpen ();
}

const IProjectGenerationSource& ArchicadHostImpl::Generations () const
{
    return generations_;
}

const IReferenceResolver& ArchicadHostImpl::References () const
{
    return references_;
}

bool ArchicadHostImpl::GetSelection (std::vector<ArchicadElementRef>& elements, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    auto guids = std::make_shared<std::vector<std::string>> ();
    if (!OnHostThread ([guids] { ReadSelectionOnHostThread (*guids); }, error))
        return false;

    for (const std::string& guid : *guids)
        elements.push_back (ArchicadElementRef { guid });
    return true;
}

bool ArchicadHostImpl::SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error)
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    auto ids = std::make_shared<std::vector<std::string>> ();
    for (const ArchicadElementRef& element : elements)
        ids->push_back (element.guid);

    auto failure = std::make_shared<std::string> ();

    const bool delivered = OnHostThread (
        [ids, failure] {
            // Resolve every neig BEFORE deselecting. Deselecting first and then
            // discovering an unselectable element would leave the user with an
            // empty selection and an error - worse than the selection they had.
            GS::Array<API_Neig> neigs;
            for (const std::string& id : *ids) {
                const API_Guid picked = APIGuidFromString (id.c_str ());
                // Sub-parts of columns, railings, curtain walls and stairs are
                // not selectable; the owner is. See the file header.
                const API_Guid owner = geomsrv::ResolveSelectableOwner (picked);
                if (owner == APINULLGuid) {
                    *failure = "element " + id + " cannot be selected";
                    return;
                }
                API_Neig neig = {};
                if (ACAPI_Selection_SetSelectedElementNeig (&owner, &neig) != NoError) {
                    *failure = "element " + id + " cannot be selected";
                    return;
                }
                neigs.Push (neig);
            }

            ACAPI_Selection_DeselectAll ();
            if (neigs.IsEmpty ())
                return; // An empty result deselects, which is a legitimate answer.
            const GSErrCode err = ACAPI_Selection_Select (neigs, true);
            if (err != NoError)
                *failure = "Archicad refused the selection";
        },
        error);

    if (!delivered)
        return false;
    if (!failure->empty ()) {
        error = *failure;
        return false;
    }
    return true;
}

ArchicadHostImpl& ArchicadHostImpl::Get ()
{
    static ArchicadHostImpl host;
    return host;
}

} // namespace evp::nodegraph
