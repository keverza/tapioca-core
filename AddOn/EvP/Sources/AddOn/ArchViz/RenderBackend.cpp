#include "ArchViz/RenderBackend.hpp"

#include <cstdio>

namespace geomsrv::archviz {
namespace {

std::string Hex2 (uint32_t value)
{
    char text[16] = {};
    std::snprintf (text, sizeof (text), "0x%02X", static_cast<unsigned> (value));
    return text;
}

} // namespace

const char* RenderAdapterRefusal (const RenderAdapterFacts& adapter)
{
    if (adapter.kind == RenderAdapterKind::Software)
        return "a software adapter";
    if (adapter.vendorId == kMicrosoftAdapterVendorId)
        return "a Microsoft software or basic adapter";
    // ⚠️ UNKNOWN IS REFUSED, NOT GIVEN THE BENEFIT OF THE DOUBT. The engine
    // reports Unknown when it could not classify the adapter, which is exactly
    // the case where nothing else said here can be trusted either.
    if (adapter.kind == RenderAdapterKind::Unknown)
        return "an adapter of unknown type";
    if (!adapter.rayTracingEnabled)
        return "no hardware ray tracing";
    if ((adapter.rayTracingCaps & kRequiredRayTracingCaps) != kRequiredRayTracingCaps)
        return "incomplete ray-tracing capabilities";
    return nullptr;
}

RenderBackendChoice ChooseRenderBackend (RenderBackendPreference preference,
                                         const std::vector<RenderAdapterFacts>& adapters,
                                         const RenderBackendConstraints& constraints)
{
    RenderBackendChoice choice;
    if (preference == RenderBackendPreference::D3D11) {
        choice.reason = "preference d3d11";
        return choice;
    }
    if (constraints.requiresD3D11Interop) {
        choice.reason =
            "auto: D3D11 required by " +
            (constraints.interopReason.empty () ? std::string ("a D3D11 interop") : constraints.interopReason);
        return choice;
    }
    if (constraints.d3d12Unavailable) {
        choice.reason = "auto: the D3D12 runtime could not be loaded";
        return choice;
    }

    std::string seen;
    for (size_t index = 0; index < adapters.size (); ++index) {
        const RenderAdapterFacts& adapter = adapters[index];
        const char* refusal = RenderAdapterRefusal (adapter);
        if (refusal == nullptr) {
            choice.backend = RenderBackend::D3D12;
            choice.adapterIndex = static_cast<int> (index);
            choice.adapterName = adapter.name;
            choice.reason = "auto: D3D12 on '" + adapter.name + "' (rt caps " + Hex2 (adapter.rayTracingCaps) + ")";
            return choice;
        }
        seen += (seen.empty () ? "" : "; ") + ("'" + adapter.name + "' refused, ") + refusal;
        if (adapter.rayTracingEnabled && refusal == std::string ("incomplete ray-tracing capabilities"))
            seen += " " + Hex2 (adapter.rayTracingCaps) + ", needs " + Hex2 (kRequiredRayTracingCaps);
    }
    choice.reason =
        "auto: no qualifying D3D12 adapter (" + (seen.empty () ? std::string ("none enumerated") : seen) + ")";
    return choice;
}

std::string RefuseCreatedD3D12Device (const RenderBackendChoice& choice, const CreatedDeviceFacts& created)
{
    if (const char* refusal = RenderAdapterRefusal (created.adapter))
        return "the created D3D12 device is " + std::string (refusal) + " ('" + created.adapter.name + "')";
    // The engine was asked for one adapter by index; an index that moved
    // between enumeration and creation would hand back a different one.
    if (created.adapter.name != choice.adapterName)
        return "the created D3D12 device is '" + created.adapter.name + "', not the chosen '" + choice.adapterName +
               "'";
    if (created.ndcMinZ != 0.0f)
        return "the created D3D12 device does not use the 0..1 depth range Camera.cpp assumes";
    return {};
}

bool ParseRenderBackendPreference (const std::string& text, RenderBackendPreference& out)
{
    if (text == "d3d11") {
        out = RenderBackendPreference::D3D11;
        return true;
    }
    if (text == "auto") {
        out = RenderBackendPreference::Auto;
        return true;
    }
    return false;
}

const char* RenderBackendPreferenceName (RenderBackendPreference preference)
{
    return preference == RenderBackendPreference::Auto ? "auto" : "d3d11";
}

const char* RenderBackendName (RenderBackend backend)
{
    return backend == RenderBackend::D3D12 ? "d3d12" : "d3d11";
}

} // namespace geomsrv::archviz
