"""Layer 2 — the independent DATABASES a drawing gets placed into.

    dbs  = evp.layouts.databases(["3dDocument"])              # read
    made = evp.layouts.create_3d_document("Option A")         # no other route exists
    evp.layouts.delete_databases("3dDocument", [made["guid"]], confirm=True)

⚠️ THIS MODULE IS DELIBERATELY NARROW — read this before adding to it.

Most of the layout cluster already has a working, live-verified route, and
duplicating one is the mistake this docstring exists to prevent. What is where:

  ALREADY COVERED ELSEWHERE — use these, do not re-add them here:
    worksheet create   Tapir.CreateWorksheets     -> navtree.ensure_worksheet
    detail create      Tapir.CreateDetails
    subset create      Tapir.CreateLayoutSubset   -> navtree.ensure_subset
    layout create      API.CreateLayout           -> navtree.create_layout
  (`navtree` is Commands/MassingFeasibility/navtree.py. `create_layout` there
  already reads the master's own LayoutParameters via `API.GetLayoutSettings`, so
  the "a JSON caller has to invent a page size" objection was already answered —
  native CreateLayout/CreateSubset commands were drafted and then REMOVED as
  duplication.)

  WHAT THIS MODULE IS FOR — the measured gaps:
    * **3D document create.** No route anywhere: the `archicad` package ships
      none, and Tapir registers CreateWorksheets / CreateDetails / CreateLayout /
      CreateLayoutSubset / CreateDrawings and nothing for 3D documents.
    * **Worksheet / detail / 3D-document DELETE.** The navigator route does not do
      it. Tapir's `DeleteNavigatorItems` branches on item type: Layout and
      MasterLayout go to `ACAPI_Database_DeleteDatabase`, SubSet returns
      APIERR_NOTSUPPORTED, and everything else goes to
      `ACAPI_Navigator_DeleteNavigatorView` — which deletes a VIEW, not the
      database. So `delete_databases` is the only way to delete a worksheet.
    * **Enumerating independent databases with their unique ids**, which is what
      the two above take as input and what the navigator tree does not give.

  AND ONE THING NOBODY CAN DO: **there is no subset delete, anywhere.** Grepped
  the AC29 headers (a default, a create, a read — no delete of any spelling), and
  Tapir answers APIERR_NOTSUPPORTED with exactly that explanation. Deleting a
  subset is a manual step in the Navigator.

⚠️⚠️ NOTHING HERE IS UNDOABLE, and nothing here can go inside `evp.transaction`.
Both calls are *structural*: Archicad documents them as "non-undoable data
structure modifier" functions that REFUSE to run while an undo scope is open
(`APIERR_REFUSEDCMD`). So:

  * **Ctrl+Z will not bring back a deleted worksheet or 3D document.** That is why
    `delete_databases` requires `confirm=True` — there is no rollback to fall back
    on if the guid list is wrong.
  * **`tx.call` on either is refused by name**, not silently broken: the native
    dispatcher checks the command's category before running the step and fails the
    batch with an explanation. Call them directly.
"""

from .api import call

# The database kinds the native commands accept. Anything else is refused with
# this list, rather than falling back to some default type.
DATABASE_TYPES = ("worksheet", "detail", "layout", "masterLayout", "3dDocument")


def _check_type(db_type):
    if db_type not in DATABASE_TYPES:
        raise ValueError("unknown database type %r (want one of %s)"
                         % (db_type, " | ".join(DATABASE_TYPES)))


def databases(types=None):
    """List the independent databases in the project. Returns a list of dicts.

    Each dict: {type, guid, name, ref, title} plus `master_layout_guid` for
    layouts and `error` when that one database's info would not read.

    `types` omitted means all of DATABASE_TYPES. This is the entry point for
    everything else in the module — every other call takes a database `guid`, and
    this is the only way to learn one. It is also the VERIFICATION surface: a
    structural create leaves no undo entry to inspect, so "did it work" means
    "does it show up here".
    """
    params = {}
    if types is not None:
        types = list(types)
        for db_type in types:
            _check_type(db_type)
        params["types"] = types

    data = call("EvP.ListDatabases", params).data or {}
    result = []
    for entry in data.get("databases", []):
        entry = entry or {}
        record = {
            "type": entry.get("type", ""),
            "guid": entry.get("guid", ""),
            "name": entry.get("name", ""),
            "ref": entry.get("ref", ""),
            "title": entry.get("title", ""),
            "error": entry.get("error", ""),
        }
        if "masterLayoutGuid" in entry:
            record["master_layout_guid"] = entry["masterLayoutGuid"]
        result.append(record)
    return result


def views(map_name="view", placeable_only=True):
    """List Navigator items BY NAME. Returns [{name, guid, path, item_type, placeable, map}].

    ⚠️⚠️ THE POINT OF THIS CALL, and a rule for every command you write: **a command
    that asks the user for a guid is unusable.** Archicad's UI shows a person NAMES
    — there is no way for them to read a navigator guid out of it. Learned the hard
    way: `LayoutApiProbe2`'s first version took `view_guid` as a text input and two
    of its seven questions could only be SKIPPED, because the input was
    unfillable. So this lists what exists by name, and
    `evp.drawings.place_from_view` takes `view_name`. Same policy the attribute
    pickers have always followed for layers and profiles: names, not indices.

    `map_name` is "view" (Public View Map, the default and where placeable saved
    views live), "myView", "project" (Project Map — its items are NOT placeable as
    drawings) or "layout".

    `placeable_only` (default True) drops folders and the project root, leaving
    exactly what a Drawing can source from. `path` is the slash-joined ancestor
    folders, which is how you tell two same-named views apart.

    ⚠️ `map_name="myView"` returns [] on a SOLO project rather than failing — the My
    View Map exists only in Teamwork mode. Do not read an empty result there as an
    error, and do not wrap this call in a try to survive it.
    """
    data = call("EvP.ListViews",
                {"map": map_name, "placeableOnly": bool(placeable_only)}).data or {}
    return [{"name": v.get("name", ""),
             "guid": v.get("guid", ""),
             "path": v.get("path", ""),
             "item_type": v.get("itemType", ""),
             "placeable": bool(v.get("placeable", False)),
             "map": v.get("map", map_name)}
            for v in (data.get("views") or [])]


def create_database(db_type, name=None, ref=None):
    """Create an independent database. Returns {ok, type, guid, name, ref, error}.

    `db_type` is one of DATABASE_TYPES except "layout" — a layout needs a master,
    which `ACAPI_Database_NewDatabase` cannot supply, so the native command refuses
    it rather than producing a broken one. Use `navtree.create_layout`.

    ⚠️ PREFER THE TAPIR ROUTE FOR "worksheet" AND "detail" (see the module
    docstring): `navtree.ensure_worksheet` gives you find-or-create semantics and a
    navigator id, where this gives you a bare database. They stay accepted here as
    a native fallback and as a DIAGNOSTIC CONTROL — "worksheet" is the type
    NewDatabase's docs agree is supported, so pairing it with "3dDocument" in a
    probe distinguishes "our call is wrong" from "this type is refused".

    ⚠️ Requires a floor-plan window to be open (Archicad answers APIERR_NOPLAN
    otherwise). ⚠️ Not undoable; not usable inside `evp.transaction`.

    A 3D DOCUMENT created here takes the 3D-document DEFAULTS, not the current 3D
    view. Pointing it at the current view is a separate step:
    `set_3d_document_from_view(guid)`, below.
    """
    _check_type(db_type)
    params = {"type": db_type}
    if name:
        params["name"] = name
    if ref:
        params["ref"] = ref

    data = call("EvP.CreateDatabase", params).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "type": data.get("type", db_type),
        "guid": data.get("guid", ""),
        "name": data.get("name", ""),
        "ref": data.get("ref", ""),
        "error": data.get("error", ""),
    }


def create_worksheet(name=None, ref=None):
    """Create an independent worksheet. See `create_database` for the caveats."""
    return create_database("worksheet", name, ref)


def create_3d_document(name=None, ref=None):
    """Create a 3D Document. See `create_database` for the caveats.

    ⚠️ THE ONE CALL IN THIS MODULE WHOSE SUPPORT RESTS ON AN EXAMPLE RATHER THAN
    THE DOCS. `ACAPI_Database_NewDatabase`'s own error list claims it refuses
    anything but Detail/Worksheet/Layout/MasterLayout — i.e. that this cannot
    work. The DevKit's shipped `Database_Control` example contradicts it: its
    `Do_Create3DDocument` passes exactly this type. Working example code beats a
    doc comment, but this is the row to check first if it fails.
    """
    return create_database("3dDocument", name, ref)


def set_3d_document_from_view(guid, from_current_3d_view=True,
                              transparency=None, cutaway3d=None, material_from_3d=None):
    """Point an existing 3D Document at the CURRENT 3D VIEW.

    Returns {ok, guid, name, is_persp, applied, verified, error}. `applied` names
    the field groups that were written ("projectionSetting", "window3DInfo",
    "transparency", …) so a log can say what was copied rather than just "done".

    The second half of `create_3d_document`: the create gives you a document built
    from the 3D-document DEFAULTS, and this copies the live 3D window's projection
    (camera, perspective-vs-axonometric, FOV) and appearance into it. The usual
    sequence is create → this, once per massing option.

    ⚠️⚠️ `verified` IS THE FIELD TO CHECK, not `ok`. `ok` means Archicad returned
    NoError; `verified` means the projection was read back and matches what was
    written. Those have already come apart once in this API family (a NoError
    that silently changed nothing). `ok=True, verified=False` means the call was
    accepted and the camera did not move — treat it as a failure.

    ⚠️ THE 3D WINDOW IS THE SOURCE, whatever window is frontmost. If a floor plan
    is in front, what gets copied is the 3D window's LAST state, not what you are
    looking at. Open the 3D view you want first.

    ⚠️ NOT UNDOABLE, and not usable inside `evp.transaction` —
    `ACAPI_View_ChangeDocumentFrom3DSettings` is documented as a non-undoable data
    structure modifier, so it is a structural command like create/delete. Ctrl+Z
    will not restore the document's previous settings. Re-running against the
    right 3D view is the way back.

    Cut planes are NOT copied — `cutaway3d` toggles the document's own existing
    cut planes; the 3D window's cutting planes are a separate API.
    """
    if not guid:
        raise ValueError("set_3d_document_from_view(guid=...) needs a 3D document guid "
                         "(layouts.databases(['3dDocument']) lists them)")

    params = {"guid": guid, "fromCurrent3DView": bool(from_current_3d_view)}
    if transparency is not None:
        params["transparency"] = bool(transparency)
    if cutaway3d is not None:
        params["cutaway3D"] = bool(cutaway3d)
    if material_from_3d is not None:
        params["materialFrom3D"] = bool(material_from_3d)

    data = call("EvP.SetDocumentFrom3DSettings", params).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "guid": data.get("guid", guid),
        "name": data.get("name", ""),
        "is_persp": bool(data.get("isPersp", False)),
        "applied": list(data.get("applied") or []),
        "verified": bool(data.get("verified", False)),
        "error": data.get("error", ""),
    }


def delete_databases(db_type, guids, confirm=False):
    """Delete independent databases by guid. Returns {ok, deleted, results, error}.

    THE ONLY WAY to delete a WORKSHEET, a DETAIL or a 3D DOCUMENT — the navigator
    route sends those to `ACAPI_Navigator_DeleteNavigatorView`, which deletes a
    view and not the database (see the module docstring). Layouts work either way.

    Each entry of `results` is {guid, ok, name, error} — and `name` is read BEFORE
    the delete, because afterwards there is nothing left to ask and "deleted 3
    databases" is not something a user can check.

    ⚠️⚠️ `confirm=True` IS MANDATORY, and not as a formality: this is NOT
    UNDOABLE. A structural change leaves no undo step, so a wrong guid destroys
    work permanently. Every other write in EvP is recoverable; this one is not.
    """
    guids = [g for g in guids]
    if not guids:
        return {"ok": True, "deleted": 0, "results": [], "error": ""}
    _check_type(db_type)
    if not confirm:
        raise ValueError(
            "delete_databases(confirm=True) is required: deleting a %s is NOT UNDOABLE "
            "(no undo step is created, so Ctrl+Z cannot bring it back)" % db_type)

    data = call("EvP.DeleteDatabase",
                {"type": db_type, "guids": guids, "confirm": True}).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "deleted": data.get("deleted", 0),
        "results": [{"guid": r.get("guid", ""),
                     "ok": bool(r.get("ok", False)),
                     "name": r.get("name", ""),
                     "error": r.get("error", "")}
                    for r in (data.get("results") or [])],
        "error": data.get("error", ""),
    }


def delete_navigator_items(navigator_item_ids):
    """Delete navigator items — LAYOUTS and VIEWS. Returns a list of {ok, error}.

    `API.DeleteNavigatorItems` is in the local `archicad` package's AC29 command set
    (the only trustworthy source for `API.*` names). Deleting a layout removes the
    Drawings placed on it.

    ⚠️ IT DOES NOT DELETE EVERY NAVIGATOR ITEM, despite the name. Read the impl
    (Tapir's `DeleteNavigatorItemsCommand::Execute` shows the branching): Layout and
    MasterLayout go to `ACAPI_Database_DeleteDatabase`; a **SUBSET returns
    APIERR_NOTSUPPORTED** ("Deleting layout subsets is not supported by the ArchiCAD
    API"); everything else goes to `ACAPI_Navigator_DeleteNavigatorView`, which
    deletes a VIEW and not the underlying database. So use `delete_databases` for a
    worksheet / detail / 3D document, and delete a subset by hand.

    ⚠️ Not part of `evp.transaction` — this goes out on the `API.*` route, which
    opens its own undo scope per call, so N items is N undo steps.
    """
    ids = [{"navigatorItemId": {"guid": nav_id}} if isinstance(nav_id, str) else nav_id
           for nav_id in navigator_item_ids]
    if not ids:
        return []

    result = call("API.DeleteNavigatorItems", {"navigatorItemIds": ids})
    executions = (result.data or {}).get("executionResults") or []
    return [{"ok": bool(r.get("success", False)),
             "error": (r.get("error") or {}).get("message", "")}
            for r in executions]
