#include "Geometry/GeometryRustTerrain.hpp"
#include "GeometryRustFfi.h"

namespace tapioca::geometry {
namespace {
std::string Failure (TapiocaStatus status, const std::array<char, 256>& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    return length != 0 ? std::string (buffer.data (), length)
                       : "Tapioca Rust grading call failed with status " + std::to_string (status);
}
} // namespace

bool SolveGrading (const GradingRequest& request, GradingResult& result, std::string& error)
{
    std::vector<TapiocaGradingTerm> terms;
    std::vector<TapiocaGradingRow> rows;
    rows.reserve (request.rows.size ());
    for (const auto& row : request.rows) {
        constexpr auto limit = std::numeric_limits<uint32_t>::max ();
        if (terms.size () > limit || row.terms.size () > limit - terms.size ()) {
            error = "GradingShape: term pool overflow";
            return false;
        }
        rows.push_back ({ row.target, row.weight, static_cast<uint32_t> (terms.size ()),
                          static_cast<uint32_t> (row.terms.size ()) });
        for (const auto& term : row.terms)
            terms.push_back ({ term.coefficient, term.unknown, 0 });
    }
    std::vector<TapiocaGradingFixed> fixed;
    for (const auto& item : request.fixed)
        fixed.push_back ({ item.value, item.unknown, 0 });
    std::vector<TapiocaGradingBound> bounds;
    for (const auto& item : request.bounds)
        bounds.push_back ({ item.lower, item.upper, item.unknown, 0 });
    std::vector<TapiocaGradingDifference> differences;
    for (const auto& item : request.differences)
        differences.push_back ({ item.min, item.max, item.a, item.b });

    TapiocaGradingRequest input {};
    input.terms = { terms.data (), terms.size () };
    input.rows = { rows.data (), rows.size () };
    input.fixed = { fixed.data (), fixed.size () };
    input.bounds = { bounds.data (), bounds.size () };
    input.differences = { differences.data (), differences.size () };
    input.start = { request.start.data (), request.start.size () };
    input.feasibility = request.limits.feasibility;
    input.residual = request.limits.residual;
    input.unknowns = request.unknowns;
    input.maxIterations = request.limits.maxIterations;
    input.constrained = !bounds.empty () || !differences.empty () || !request.start.empty () ? 1U : 0U;

    GradingResult candidate;
    candidate.values.resize (request.unknowns);
    TapiocaGradingSummary summary {};
    const TapiocaGradingOutput output { &summary, candidate.values.data (), candidate.values.size () };
    std::array<char, 256> buffer {};
    const auto status = tapioca_grading_solve (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    candidate.objective = summary.objective;
    candidate.iterations = summary.iterations;
    candidate.factorizations = summary.factorizations;
    candidate.active = summary.active;
    result = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
