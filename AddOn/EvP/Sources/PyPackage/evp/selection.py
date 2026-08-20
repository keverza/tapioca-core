"""Layer 2 — the current selection, element highlighting, and view framing.

    guids = evp.selection.get()                 # what the user has selected now
    evp.selection.set(guids)                    # replace the selection
    evp.selection.highlight(guids, (1, 0, 0))   # paint them, all windows
    evp.selection.clear_highlight()
    evp.selection.zoom_to(guids)                # frame them in the active view

get()/set() return data; highlight/clear/zoom are FIRE-AND-FORGET like ui.status
— they post to the main thread and return immediately, so they are cheap to call
in a visual-QA loop and never block on human time. For the interactive rung —
"ask the user to select, then read it back" — see evp.ui.request_selection(),
which lives in ui.py because it is an interaction, not a data read.

Colours are (r, g, b) or (r, g, b, a) with components in 0..1 (Archicad's own
API_RGBAColor range), NOT 0..255. They are coerced to float before crossing the
bus for the same reason geometry._vec exists: ObjectState::Get returns FALSE on a
type mismatch, so an int like 0 or 1 arrives as a JSON integer and is dropped,
which would silently blank the colour.
"""

from .api import call


def get():
    """The GUIDs of the currently selected elements, in selection order.

    Returns a list (empty when nothing is selected — that is not an error).
    """
    elements = (call("Tapioca.GetSelection").data or {}).get("elements", [])
    return [item.get("elementId", {}).get("guid", "") for item in elements
            if item.get("elementId", {}).get("guid")]


def _elements(guids):
    return [{"elementId": {"guid": str(guid)}} for guid in guids]


def _selection_result(result):
    data = result.data or {}
    data["missing"] = [item.get("elementId", {}).get("guid", "")
                       for item in data.get("missing", [])
                       if item.get("elementId", {}).get("guid")]
    return data


def set(guids, add=False):
    """Select `guids`. Replaces the current selection unless add=True.

    Returns {selected, missing, count}: `missing` lists any guid that no longer
    resolves to a selectable element, so a partial hit is visible rather than
    silent.
    """
    return _selection_result(call("Tapioca.SetSelection", {
        "elements": _elements(guids), "add": bool(add)
    }))


def replace(guids):
    """Replace the active Archicad selection and report its final count."""
    return _selection_result(call("Tapioca.ModifySelection", {
        "op": "replace", "elements": _elements(guids)
    }))


def add(guids):
    """Add `guids` to the active Archicad selection in one native operation."""
    return _selection_result(call("Tapioca.ModifySelection", {
        "op": "add", "elements": _elements(guids)
    }))


def remove(guids):
    """Remove `guids` from the active Archicad selection in one native operation."""
    return _selection_result(call("Tapioca.ModifySelection", {
        "op": "remove", "elements": _elements(guids)
    }))


def clear():
    """Deselect everything and return the resulting active-selection count."""
    return _selection_result(call("Tapioca.ModifySelection", {"op": "clear"}))


class _SelectionSets:
    """Saved element-role sets declared by the active command.

    A command opts in with ``selection_sets=("Targets", "Operators")``. The
    palette owns their session lifetime; these methods only operate on declared
    names, so one command cannot consume another command's captured elements.
    """

    @staticmethod
    def _mutate(name, op, guids):
        payload = {"name": str(name), "op": op}
        if guids is None:
            payload["current"] = True
        else:
            payload["elements"] = _elements(guids)
        data = call("Tapioca.ModifySelectionSet", payload).data or {}
        data["guids"] = [item.get("elementId", {}).get("guid", "")
                         for item in data.pop("elements", [])
                         if item.get("elementId", {}).get("guid")]
        return data

    def names(self):
        """Declared role names in command-declaration order."""
        return (call("Tapioca.ListSelectionSets").data or {}).get("names", [])

    def get(self, name):
        """GUIDs stored for `name`, in first-capture order."""
        return self.info(name).get("guids", [])

    def info(self, name):
        """Return `{guids, count}` for a saved role."""
        data = call("Tapioca.GetSelectionSet", {"name": str(name)}).data or {}
        data["guids"] = [item.get("elementId", {}).get("guid", "")
                         for item in data.pop("elements", [])
                         if item.get("elementId", {}).get("guid")]
        return data

    def update(self, name, guids=None):
        """Replace a role with `guids`, or with the active selection when omitted."""
        return self._mutate(name, "update", guids)

    def add(self, name, guids=None):
        """Union `guids`, or the active selection, into a role."""
        return self._mutate(name, "add", guids)

    def remove(self, name, guids=None):
        """Subtract `guids`, or the active selection, from a role."""
        return self._mutate(name, "remove", guids)

    def clear(self, name):
        """Empty a saved role without changing Archicad's active selection."""
        return self._mutate(name, "clear", [])

    def reselect(self, name):
        """Replace Archicad's active selection with the saved role."""
        return _selection_result(call("Tapioca.ReselectSelectionSet", {"name": str(name)}))


sets = _SelectionSets()


def _rgba(color, default_alpha=1.0):
    """(r,g,b[,a]) -> [r,g,b,a] as floats. See the module docstring on why float."""
    values = list(color)
    if len(values) == 3:
        values = values + [default_alpha]
    return [float(v) for v in values]


def highlight(guids, color=(1.0, 0.2, 0.2), wireframe3D=None, dim_others=None,
              colors=None):
    """Paint `guids` with `color` in every window. Fire-and-forget.

    `color`/`dim_others` are (r,g,b) or (r,g,b,a), components 0..1. `dim_others`,
    when given, is the colour+alpha applied to every NON-highlighted element (a
    low alpha dims the rest of the model). `wireframe3D` switches non-highlighted
    3D elements to wireframe.

    `colors`, when given, is a sequence PARALLEL to `guids` of per-element
    colours, and replaces `color` for every guid it covers (guids past its end
    keep `color`). Use it whenever one run has more than one severity —
    ⚠️ a highlight call REPLACES the whole highlight set, so two calls do NOT
    layer: the second wipes the first. Red errors + orange warnings have to go
    out as ONE call carrying both.
    """
    params = {"elements": _elements(guids), "color": _rgba(color)}
    if colors is not None:
        for element, entry in zip(params["elements"], colors):
            element["color"] = _rgba(entry)
    if wireframe3D is not None:
        params["wireframe3D"] = bool(wireframe3D)
    if dim_others is not None:
        params["dimOthers"] = _rgba(dim_others)
    call("Tapioca.HighlightElements", params, raise_on_error=False)


def clear_highlight():
    """Remove highlights set by highlight(). Fire-and-forget."""
    call("Tapioca.ClearHighlights", raise_on_error=False)


def zoom_to(guids):
    """Frame `guids` in the active view. Fire-and-forget."""
    call("Tapioca.ZoomTo", {"elements": _elements(guids)}, raise_on_error=False)
