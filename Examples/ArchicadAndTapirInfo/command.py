"""Call the official Archicad API and Tapir through Tapioca's command bus."""

import tapioca


@tapioca.command(
    title="Archicad and Tapir Info",
    category="Examples",
    description="Reads product information from Archicad and the active window from Tapir.",
    requires_tapir=">=1.5.6",
)
def run():
    # Graphisoft's archicad package uses this same API.GetProductInfo command.
    product = tapioca.call("API.GetProductInfo").data or {}
    window = tapioca.call("Tapir.GetCurrentWindowType").data or {}

    lines = [
        "Official Archicad Python API",
        "Version: %s" % product.get("version", "unknown"),
        "Build: %s" % product.get("buildNumber", "unknown"),
        "Language: %s" % product.get("languageCode", "unknown"),
        "",
        "Tapir API",
        "Current window: %s" % window.get("currentWindowType", "unknown"),
    ]
    tapioca.ui.text("\n".join(lines))
