#ifndef EVP_NATIVECOMMANDS_NODEGRAPHCOMMANDSUPPORT_HPP
#define EVP_NATIVECOMMANDS_NODEGRAPHCOMMANDSUPPORT_HPP

// What every Tapioca.Graph* verb needs, wherever its translation unit lives.
//
// The graph verbs outgrew one file, so they are split by subject - the runtime
// verbs in NodeGraphCommands.cpp, the workflow library in
// NodeGraphLibraryCommands.cpp. Splitting them must not fork the two things they
// genuinely share: the UTF-8 conversions, and the exception wrapper that keeps
// containment rule 1 a property of the boundary rather than of each author's
// memory.

#include "NativeCommands/CommandBase.hpp"
#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/Value.hpp"

#include <string>

namespace geomsrv {

namespace graph = evp::nodegraph;

inline std::string GraphUtf8 (const GS::UniString& value)
{
    return value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

inline GS::UniString GraphText (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}

// The wire spelling of a ValueType, shared rather than repeated.
//
// ⚠️ IT LIVES HERE BECAUSE A SECOND COPY IS A SECOND CONTRACT. This was local to
// NodeGraphCommands.cpp until a second command TU needed to name a value type;
// spelling it again there is how "archicadElementRef" and "elementRef" end up
// being the same type on two verbs, and a client cannot tell which is the truth.
//
// Indexed, not switched, so a new ValueType inserted in the middle of the enum
// shifts every name after it - which the offline suite catches, unlike a switch
// that silently falls through to its default.
inline const char* GraphValueTypeName (graph::ValueType valueType)
{
    constexpr const char* names[] = { "absent", "bool",     "integer", "double", "string",
                                      "point3", "polyline", "polygon", "mesh",   "archicadElementRef",
                                      "list" };
    return names[static_cast<size_t> (valueType)];
}

// Every verb takes an optional graphId. Omitting it means the default graph, so
// a single-graph client needs to know nothing about the addressing.
inline graph::GraphId ReadGraphIdParam (const GS::ObjectState& params)
{
    GS::UniString graphId;
    if (!params.Get ("graphId", graphId) || graphId.IsEmpty ())
        return graph::kDefaultGraphId;
    return GraphUtf8 (graphId);
}

// Containment rule 1: NO GRAPH OPERATION TAKES ARCHICAD DOWN. Every verb runs
// through this one wrapper rather than relying on each handler to remember a
// try/catch.
//
// ⚠️ IT IS THE FAULT BARRIER, NOT A try/catch, AND THE DIFFERENCE IS THE WHOLE
// POINT. This wrapper used to be `try { ... } catch (const std::exception&)
// catch (...)`, which looks total and is not: under /EHsc an access violation is
// a STRUCTURED exception, and `catch (...)` is explicitly not required to catch
// one. So a null dereference or a bad index anywhere in the runtime - the plan
// builder, the serializer, the store, the value encoder, the catalog projection
// - went straight past this handler and terminated Archicad with the user's
// unsaved project in it.
//
// The barrier was already applied to NODE BODIES, on the assumption that node
// code is the untrustworthy part. That assumption was wrong in scope: the
// runtime around the node is this add-on's code too, and a defect in it is just
// as fatal and considerably more likely, because every verb runs it and only an
// evaluation runs a node. RunGuarded translates both C++ and structured
// exceptions into a returned failure, so the whole verb is contained.
//
// The honest caveat is FaultBarrier.hpp's and it applies here unchanged:
// continuing after an access violation means continuing in a process whose state
// was, for one instruction, wrong. That is accepted deliberately, because the
// alternative is not a clean process - it is Archicad terminating. The failure
// is REPORTED, so a repeatedly faulting verb shows up rather than being retried
// in silence.
class GateFreeGraphCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& control) const final
    {
        NativeCommandResult result;
        const graph::GuardOutcome guarded = graph::RunGuarded ([this, &params, &control, &result] () {
            result = ExecuteGraph (params, control);
            return true;
        });
        if (guarded.completed)
            return result;
        // A structured fault reaches here as its name and code; a thrown
        // exception as its what(). Both are the same thing to a client: the
        // graph runtime failed and Archicad is still running.
        return NativeCommandResult::Failure (GS::UniString ("the graph runtime raised an error: ", CC_UTF8) +
                                             GS::UniString (guarded.fault.c_str (), CC_UTF8));
    }

  protected:
    virtual NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl& control) const = 0;
};

} // namespace geomsrv

#endif
