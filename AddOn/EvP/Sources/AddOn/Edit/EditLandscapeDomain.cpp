#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditDocument.hpp"
#include <algorithm>
#include <cstdio>
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
    const auto byRoot = std::find_if (objects.begin (), objects.end (),
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
        owner =
            &*std::find_if (objects.begin (), objects.end (), [&] (const auto& o) { return o.id == *owners.begin (); });
    return true;
}

// #188: Boundary curve Z resolves through an owned boundary line only (SetRegionSurface), and a
// derived domain owns none, so its build could never make the surface target. It takes Constant
// elevation instead: the subject's boundary height when that line is level (exactly what Boundary
// gave there), else its mean height, said in `note`.
bool DerivedElevation (const geomsrv::edit::Document& doc, const HardscapeObject& subject, RegionSurface& surface,
                       std::string& note, std::string& error)
{
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [&] (const auto& s) { return s.id == subject.boundarySource && !s.deleted; });
    std::vector<Vec3> boundary;
    bool closed = false;
    if (source == doc.landscape->sources.end () || !LandscapeSourcePreview (doc, *source, boundary, closed, error) ||
        boundary.empty ()) {
        if (error.empty ())
            error = "Domain subject: its Boundary curve Z has no live boundary line; choose Constant elevation first.";
        return false;
    }
    double low = boundary.front ().z, high = low, sum = 0;
    for (const auto& point : boundary) {
        low = (std::min) (low, point.z), high = (std::max) (high, point.z);
        sum += point.z;
    }
    const bool level = high - low <= 1e-3;
    surface.elevation = RegionElevationMode::Constant;
    surface.slope = RegionSlopeMode::None;
    surface.first.z = level ? boundary.front ().z : sum / double (boundary.size ());
    if (!level) {
        char text[160];
        std::snprintf (text, sizeof text,
                       "The result is level at %.2f m, the mean of its boundary heights (%.2f to %.2f m).",
                       surface.first.z, low, high);
        note = text;
    }
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
            const bool source = std::any_of (candidate.landscape->sources.begin (), candidate.landscape->sources.end (),
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
        if (object.surface && object.surface->elevation == RegionElevationMode::Boundary &&
            !DerivedElevation (candidate, *subject, *object.surface, result.note, result.error))
            return result;
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

DomainEditResult DissolveBoolean (geomsrv::edit::Document& doc, Id objectId)
{
    DomainEditResult result;
    if (!doc.landscape) {
        result.error = "Dissolve: the landscape session is not started";
        return result;
    }
    geomsrv::edit::Document candidate = doc;
    auto& s = *candidate.landscape;
    const auto object =
        std::find_if (s.objects.begin (), s.objects.end (), [&] (const auto& o) { return o.id == objectId; });
    if (object == s.objects.end ()) {
        result.error = "Dissolve: the region no longer exists";
        return result;
    }
    const auto boolean = [] (OperationKind kind) {
        return kind == OperationKind::Union || kind == OperationKind::Difference ||
               kind == OperationKind::Intersection || kind == OperationKind::Xor;
    };
    const auto find = [&] (Id id) {
        const auto op =
            std::find_if (s.operations.begin (), s.operations.end (), [&] (const auto& o) { return o.id == id; });
        return op == s.operations.end () ? nullptr : &*op;
    };
    const auto* root = find (object->footprintOperation);
    if (!root || !boolean (root->kind)) {
        result.error = "Dissolve: this region is not a Boolean result";
        return result;
    }
    // The Boolean steps: the footprint and every Boolean node under it, every input followed
    // (a bypassed step still holds its other inputs), down to another region's footprint: that
    // region is an input, a result of its own, and stays.
    const auto rootOfAnother = [&] (Id id) {
        return std::any_of (s.objects.begin (), s.objects.end (),
                            [&] (const auto& o) { return o.id != objectId && o.footprintOperation == id; });
    };
    std::set<Id> steps;
    std::vector<Id> open { root->id };
    while (!open.empty ()) {
        const auto id = open.back ();
        open.pop_back ();
        const auto* op = find (id);
        if (!op || !boolean (op->kind) || rootOfAnother (id) || !steps.insert (id).second)
            continue;
        open.insert (open.end (), op->inputs.begin (), op->inputs.end ());
    }
    // Nothing else may be built from them: another step, another region's footprint or mask, this
    // region's mask, or a material assignment's mask.
    const auto inSteps = [&] (Id id) { return steps.count (id) > 0; };
    for (const auto& op : s.operations)
        if (!inSteps (op.id) && std::any_of (op.inputs.begin (), op.inputs.end (), inSteps)) {
            result.error = "Dissolve: another Boolean is built from this result; dissolve that one first";
            return result;
        }
    for (const auto& other : s.objects)
        if ((other.id != objectId && inSteps (other.footprintOperation)) ||
            std::any_of (other.maskOperations.begin (), other.maskOperations.end (), inSteps)) {
            result.error = "Dissolve: another region uses this result as its shape or mask";
            return result;
        }
    for (const auto& assignment : s.assignments)
        if (inSteps (assignment.mask)) {
            result.error = "Dissolve: a material assignment is masked by this result";
            return result;
        }
    std::set<Id> removedObjects;
    if (object->boundarySource != kNoId) {
        // Cut in place: the region's own footprint is the RegionSelect of its boundary line.
        const auto own = std::find_if (s.operations.begin (), s.operations.end (), [&] (const auto& op) {
            return op.kind == OperationKind::RegionSelect && op.inputs == std::vector<Id> { object->boundarySource } &&
                   op.faces.empty ();
        });
        if (own == s.operations.end ()) {
            result.error = "Dissolve: the region's own boundary footprint is missing";
            return result;
        }
        object->footprintOperation = own->id;
        result.objectId = objectId;
    }
    else
        removedObjects.insert (objectId);
    // What only the removed object and steps carried, as when a region line stops being one.
    std::set<Id> regions, assignments, patches;
    for (const auto& region : s.regions)
        if (removedObjects.count (region.descriptor.object) ||
            std::any_of (region.descriptor.operations.begin (), region.descriptor.operations.end (), inSteps))
            regions.insert (region.id);
    if (!removedObjects.empty ()) {
        for (const auto& assignment : s.assignments)
            if (regions.count (assignment.region))
                assignments.insert (assignment.id);
        for (const auto& patch : s.patches)
            if (removedObjects.count (patch.object) || assignments.count (patch.assignment))
                patches.insert (patch.key);
    }
    std::erase_if (s.objects, [&] (const auto& v) { return removedObjects.count (v.id) > 0; });
    std::erase_if (s.operations, [&] (const auto& v) { return inSteps (v.id); });
    std::erase_if (s.regions, [&] (const auto& v) { return regions.count (v.id) > 0; });
    std::erase_if (s.assignments, [&] (const auto& v) { return assignments.count (v.id) > 0; });
    std::erase_if (s.patches, [&] (const auto& v) { return patches.count (v.key) > 0; });
    for (auto& region : s.regions)
        std::erase_if (region.predecessors, [&] (Id id) { return regions.count (id) > 0; });
    for (auto& patch : s.patches) {
        std::erase_if (patch.predecessorRegions, [&] (Id id) { return regions.count (id) > 0; });
        std::erase_if (patch.predecessorPatches, [&] (Id id) { return patches.count (id) > 0; });
    }
    std::erase_if (s.bindings, [&] (const auto& v) {
        return (v.kind == BindingKind::Object && removedObjects.count (v.logicalId)) ||
               (v.kind == BindingKind::Operation && inSteps (v.logicalId));
    });
    LandscapeResult rebuilt;
    if (!RebuildLandscape (candidate, rebuilt, result.error, &doc) ||
        !geomsrv::edit::Validate (candidate, result.error))
        return result;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    result.ok = true;
    return result;
}

} // namespace tapioca::geometry
