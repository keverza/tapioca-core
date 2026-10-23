#include "Edit/EditSparseSolve.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include <array>
#include <exception>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {
bool Solve (const geomsrv::edit::Document& doc, const EditSparseSolveRequest& request, EditSparseSolveResult& candidate,
            std::string& error)
{
    if (!terrain_detail::ValidateInput (doc, request.input, error) ||
        !terrain_detail::ValidateOperations (doc, request.input.provenance,
                                             std::array<OperationId, 1> { request.operation }, error) ||
        !terrain_detail::ValidateUnconstrainedTerrainTier (doc, error))
        return false;
    if (request.vertices.size () != request.system.unknowns) {
        error = "SolveShape: every unknown requires one baseline vertex locator";
        return false;
    }
    std::set<uint32_t> vertices;
    for (uint32_t vertex : request.vertices)
        if (vertex >= request.input.mesh.points.size () / 3 || !vertices.insert (vertex).second) {
            error = "SolveShape: unknown vertex locators must be unique and in range";
            return false;
        }
    std::map<uint32_t, double> fixed;
    for (const auto& value : request.system.fixed) {
        const auto [found, inserted] = fixed.emplace (value.unknown, value.value);
        if (!inserted && found->second != value.value) {
            error = "SolveInconsistent: contradictory fixed heights for one unknown";
            return false;
        }
    }
    if (!SolveGrading (request.system, candidate.solution, error))
        return false;
    if (candidate.solution.values.size () != request.vertices.size ()) {
        error = "SolveOutputShape: incomplete certified solution";
        return false;
    }
    candidate.revision = request.input.revision;
    candidate.sourceTerrainGuid = request.input.sourceTerrainGuid;
    candidate.operation = request.operation;
    candidate.vertices.assign (request.vertices.begin (), request.vertices.end ());
    candidate.inputProvenance = request.input.provenance;
    return true;
}
} // namespace

bool SolveEditTerrain (const geomsrv::edit::Document& doc, const EditSparseSolveRequest& request,
                       EditSparseSolveResult& result, std::string& error)
{
    try {
        EditSparseSolveResult candidate;
        if (!Solve (doc, request, candidate, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("SparseSolveRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
