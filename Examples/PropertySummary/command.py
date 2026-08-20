"""Read one built-in quantity from the current selection."""

import tapioca


@tapioca.command(
    title="Property Summary",
    category="Examples",
    description="Demonstrates resolving and reading a built-in property in one batch.",
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
    rows = []
    for guid, value in zip(guids, values):
        amount = value[0] if value else None
        rows.append([guid, "" if amount is None else amount])
    tapioca.ui.table(["Element GUID", "General Area"], rows, row_ids=guids)
