"""Read and total a built-in area property for the current selection."""

import tapioca


@tapioca.command(
    title="Property Summary",
    category="Examples",
    description="Reads General Area in one batch and reports missing values and a total.",
    needs_selection=True,
)
def run():
    guids = tapioca.selection.get()
    if not guids:
        tapioca.ui.text("Nothing is selected.")
        return

    property_id = tapioca.properties.try_builtin_id("General_Area")
    if property_id is None:
        tapioca.ui.text("General_Area is not available in this project.")
        return

    values = tapioca.properties.values(guids, [property_id])
    identities = tapioca.elements.ids(guids)
    lines = []
    total = 0.0
    measured = 0
    for identity, value in zip(identities, values):
        amount = value[0] if value else None
        label = identity["element_id"] or identity["guid"]
        if isinstance(amount, (int, float)):
            total += float(amount)
            measured += 1
            lines.append("%s: %.2f m2" % (label, amount))
        else:
            lines.append("%s: no General Area value" % label)

    lines.extend([
        "",
        "Measured elements: %d of %d" % (measured, len(guids)),
        "Total General Area: %.2f m2" % total,
    ])
    tapioca.ui.text("\n".join(lines))
