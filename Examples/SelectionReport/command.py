"""Inspect selected elements and optionally export their identities to CSV."""

import tapioca


@tapioca.command(
    title="Selection Report",
    category="Examples",
    description="Lists selected element types and IDs, with an optional CSV export.",
    needs_selection=True,
)
def run(export_csv: bool = False):
    guids = tapioca.selection.get()
    if not guids:
        tapioca.ui.text("Nothing is selected.")
        return

    identities = tapioca.elements.ids(guids)
    rows = []
    lines = ["Selected elements: %d" % len(guids)]
    for index, identity in enumerate(identities, start=1):
        element_id = identity["element_id"] or "(no ID)"
        if identity["found"]:
            type_name = identity["type_name"] or "Unknown type"
            lines.append(
                "%d. %s | ID: %s | GUID: %s"
                % (index, type_name, element_id, identity["guid"])
            )
        else:
            type_name = "Unavailable: %s" % (identity["reason"] or "unknown reason")
            lines.append("%d. %s | GUID: %s" % (index, type_name, identity["guid"]))
        rows.append([index, type_name, element_id, identity["guid"]])

    if export_csv:
        artifact = tapioca.outputs.csv_file(
            "selection-report",
            ["Number", "Element type", "Element ID", "GUID"],
            rows,
        )
        lines.append("CSV: %s" % artifact.path)

    tapioca.ui.text("\n".join(lines))
