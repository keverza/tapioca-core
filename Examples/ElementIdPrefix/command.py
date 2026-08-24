"""Preview or apply an Element ID prefix to a mixed selection."""

import tapioca


@tapioca.command(
    title="Element ID Prefix",
    category="Examples",
    description="Previews new Element IDs and applies them only when requested.",
    needs_selection=True,
)
def run(
    prefix: tapioca.Text = "A-",
    apply_changes: bool = False,
):
    guids = tapioca.selection.get()
    if not guids:
        tapioca.ui.text("Nothing is selected.")
        return
    if not prefix:
        tapioca.ui.text("Enter a non-empty prefix.")
        return

    identities = tapioca.elements.ids(guids)
    changes = []
    lines = []
    for identity in identities:
        if not identity["found"]:
            lines.append(
                "Skipped %s: %s"
                % (identity["guid"], identity["reason"] or "ID is unavailable")
            )
            continue
        old_id = identity["element_id"]
        new_id = old_id if old_id.startswith(prefix) else prefix + old_id
        changes.append((identity["guid"], new_id))
        lines.append("%s -> %s" % (old_id or "(empty)", new_id))

    if not changes:
        tapioca.ui.text("\n".join(lines) or "No editable Element IDs were found.")
        return

    if not apply_changes:
        lines.extend(["", "Preview only. Enable Apply changes to write these IDs."])
        tapioca.ui.text("\n".join(lines))
        return

    results = tapioca.elements.set_ids(changes)
    changed = sum(1 for result in results if result["ok"])
    failures = [result for result in results if not result["ok"]]
    lines.extend(["", "Updated Element IDs: %d of %d" % (changed, len(changes))])
    for failure in failures:
        lines.append(
            "Failed %s: %s"
            % (failure["guid"], failure["error"] or "unknown error")
        )
    tapioca.ui.text("\n".join(lines))
