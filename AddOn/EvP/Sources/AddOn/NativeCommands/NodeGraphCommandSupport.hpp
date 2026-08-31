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
#include "NodeGraph/GraphRuntimeState.hpp"

#include <exception>
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

// Every verb takes an optional graphId. Omitting it means the default graph, so
// a single-graph client needs to know nothing about the addressing.
inline graph::GraphId ReadGraphIdParam (const GS::ObjectState& params)
{
    GS::UniString graphId;
    if (!params.Get ("graphId", graphId) || graphId.IsEmpty ())
        return graph::kDefaultGraphId;
    return GraphUtf8 (graphId);
}

// Containment rule 1: no graph operation propagates an exception into
// Archicad's call stack. Every verb runs through this one wrapper rather than
// relying on each handler to remember a try/catch.
class GateFreeGraphCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& control) const final
    {
        try {
            return ExecuteGraph (params, control);
        }
        catch (const std::exception& exception) {
            return NativeCommandResult::Failure (GS::UniString ("the graph runtime raised an error: ", CC_UTF8) +
                                                 GS::UniString (exception.what (), CC_UTF8));
        }
        catch (...) {
            return NativeCommandResult::Failure (
                GS::UniString ("the graph runtime raised an unknown error", CC_UTF8));
        }
    }

  protected:
    virtual NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl& control) const = 0;
};

} // namespace geomsrv

#endif
