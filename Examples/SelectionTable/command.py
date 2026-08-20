"""Display the current Archicad selection as a clickable result table."""

import tapioca


@tapioca.command(
    title="Selection Table",
    category="Examples",
    description="Reads the active selection and displays its GUIDs in a table.",
    needs_selection=True,
)
def run():
    guids = tapioca.selection.get()
    if not guids:
        tapioca.ui.text("Nothing is selected.")
        return

    rows = [[index, guid] for index, guid in enumerate(guids, start=1)]
    tapioca.ui.table(["#", "Element GUID"], rows, row_ids=guids)
