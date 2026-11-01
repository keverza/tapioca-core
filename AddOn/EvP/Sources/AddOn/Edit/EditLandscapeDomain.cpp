#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditDocument.hpp"
#include <algorithm>
#include <set>

namespace tapioca::geometry {
namespace {

// The subject's configured owner, if there is exactly one. Direct hit: the subject
// node is some object's current root (saved-set and cutter operands). Fresh face
// subject: the unique region object the subject faces cite through their sources.
// Two configured owners in one subject operand is ambiguous: the caller must narrow
// the selection, so that refuses instead of silently picking a winner.
bool SubjectOwner (const geomsrv::edit::Document& doc, Id subjectNode, const DomainOperand& subject,
                   const HardscapeObject*& owner, std::string& error)
{
    owner = nullptr;
    if (!doc.landscape)
        return true;
    const auto& objects = doc.landscape->objects;
    const auto byRoot =
        std::find_if (objects.begin (), objects.end (),
                      [=] (const auto& o) { return o.footprintOperation == subjectNode; });
    if (byRoot != objects.end ()) {
        owner = &*byRoot;
        return true;
    }
    if (subject.faces.empty ())
        return true;
    std::set<Id> owners;
    for (const auto& face : subject.faces)
        for (Id source : face.sources)
            for (const auto& object : objects)
                if (object.boundarySource == source)
                    owners.insert (object.id);
    if (owners.size () > 1) {
        error = "Domain subject: the selected faces cite more than one configured region; "
                "select faces of one subject.";
        return false;
    }
    if (owners.size () == 1)
        owner = &*std::find_if (objects.begin (), objects.end (),
                                [&] (const auto& o) { return o.id == *owners.begin (); });
    return true;
}

} // namespace

DomainEditResult ApplyDomainCommand (geomsrv::edit::Document& doc, const DomainCommand& command)
{
    DomainEditResult result;
    const bool boolean = command.operand.input != kNoId || !command.operand.faces.empty ();
    const bool booleanKind = command.kind == OperationKind::Union || command.kind == OperationKind::Difference ||
                             command.kind == OperationKind::Intersection;
    if (!doc.landscape || !booleanKind) {
        result.error = "Domain command: a landscape session and Union/Difference/Intersection are required";
        return result;
    }
    // Stage on a candidate so a refused command leaves the caller's document untouched;
    // the individual Add* calls still publish one validated revision each on success.
    geomsrv::edit::Document candidate = doc;
    const auto nodeFor = [&] (const DomainOperand& operand, Id& node) {
        if (operand.input != kNoId) {
            const bool source =
                std::any_of (candidate.landscape->sources.begin (), candidate.landscape->sources.end (),
                             [=] (const auto& s) { return s.id == operand.input && !s.deleted; });
            const bool operation =
                std::any_of (candidate.landscape->operations.begin (), candidate.landscape->operations.end (),
                             [=] (const auto& o) { return o.id == operand.input; });
            if (!source && !operation) {
                result.error = "Domain operand: the input node does not exist";
                return false;
            }
            node = operand.input;
            return true;
        }
        if (operand.faces.empty ()) {
            result.error = "Domain operand: faces or an existing input node required";
            return false;
        }
        LandscapeOperation select;
        select.kind = OperationKind::RegionSelect;
        select.faces = operand.faces;
        return AddLandscapeOperation (candidate, select, node, result.error);
    };
    Id subjectNode = kNoId, root = kNoId;
    if (!nodeFor (command.subject, subjectNode))
        return result;
    if (boolean) {
        Id operandNode = kNoId;
        if (!nodeFor (command.operand, operandNode))
            return result;
        LandscapeOperation booleanOp;
        booleanOp.kind = command.kind;
        booleanOp.inputs = { subjectNode, operandNode };
        if (!AddLandscapeOperation (candidate, booleanOp, root, result.error))
            return result;
    }
    else
        root = subjectNode;
    if (command.owner != kNoId) {
        for (auto& object : candidate.landscape->objects)
            if (object.id == command.owner) {
                object.footprintOperation = root;
                doc = std::move (candidate);
                result.ok = true;
                result.objectId = command.owner;
                result.operationId = root;
                return result;
            }
        result.error = "Domain owner: the chosen object no longer exists";
        return result;
    }
    const HardscapeObject* subject = nullptr;
    if (!SubjectOwner (candidate, subjectNode, command.subject, subject, result.error))
        return result;
    HardscapeObject object;
    if (subject) {
        // A's metadata wins deterministically; B contributes geometry only (#112
        // acceptance: inheritance is explicit, never copied from the operand).
        object.kind = subject->kind;
        object.surface = subject->surface;
        object.terrain = subject->terrain;
        object.baseMaterial = subject->baseMaterial;
        object.thickness = subject->thickness;
    }
    object.name = command.name;
    object.footprintOperation = root;
    if (!AddHardscapeObject (candidate, object, result.objectId, result.error))
        return result;
    doc = std::move (candidate);
    result.operationId = root;
    result.ok = true;
    return result;
}

} // namespace tapioca::geometry
