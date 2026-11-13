#ifndef TAPIOCA_EDIT_LANDSCAPEDOMAIN_HPP
#define TAPIOCA_EDIT_LANDSCAPEDOMAIN_HPP

// #112 P1: one ordered domain-edit entry point for every Boolean authoring path
// (canvas command, Keep/Remove, face-set Boolean). The operation DAG stays the
// single planar authority: this command composes only the existing node kinds
// (RegionSelect + Union/Difference/Intersection) and binds the result root to one
// owning HardscapeObject. It is a pipeline correction, not a second domain model;
// evaluated polygons, island indices and face ranks are never persisted here.
#include "Edit/EditLandscape.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {
struct Document;
}

namespace tapioca::geometry {

// One Boolean operand. Exactly one form is used: `input` names an existing live
// DAG node (a source or an operation id, as the saved-set and cutter paths do),
// or `faces` authors a fresh RegionSelect node (the canvas/Keep picker paths).
struct DomainOperand {
    Id input = kNoId;
    std::vector<RegionDescriptor> faces;
};

struct DomainCommand {
    // Union | Difference | Intersection, ordered: the result is subject op operand.
    OperationKind kind = OperationKind::Union;
    // Existing object whose domain root the result replaces in place: its identity
    // and metadata stay stable and it stays one undo step. Zero authors a new
    // output object derived from the subject.
    Id owner = kNoId;
    DomainOperand subject, operand;
    // Output object name when `owner` is zero; never a silently bare object.
    std::string name;
};

struct DomainEditResult {
    bool ok = false;
    Id objectId = kNoId;    // The owner, or the authored output object.
    Id operationId = kNoId; // The resulting domain root (Boolean or adopted node).
    std::string error;
    std::string note; // What the user should know about an applied result; empty when nothing.
};

// One atomic document edit: all operand nodes, the Boolean (when an operand is
// present) and the output owner publish together or not at all. When no `owner`
// is given, the output inherits the subject owner's kind/surface/terrain/material/
// thickness; the operand never contributes metadata. #188: a subject with Boundary
// curve Z gives a derived output Constant elevation instead (a derived domain owns no
// boundary line): the line's height when it is level, else its mean height, in `note`. A fresh face subject that
// cites more than one configured region refuses rather than picking one.
DomainEditResult ApplyDomainCommand (geomsrv::edit::Document& doc, const DomainCommand& command);

} // namespace tapioca::geometry
#endif
