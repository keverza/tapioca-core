"""Layer 2 — Archicad Issues (the API's "Mark-Ups").

    issue_id = evp.issues.create("Wall gaps < 5 mm (12)")
    evp.issues.attach(issue_id, faulty_guids)                 # Highlight by default
    # or, both in ONE undo step:
    issue_id = evp.issues.create_for("Wall gaps (12)", faulty_guids)

Absorbs the two Tapir commands MultimodalCheck depended on — CreateIssue and
AttachElementsToIssue — onto EvP's own native `EvP.CreateIssue` /
`EvP.AttachElementsToIssue` (ACAPI_Markup_Create / ACAPI_Markup_AttachElements),
so no Tapir add-on is needed. An "Issue" in the Archicad UI is a Mark-Up entry in
the API; the wrapper keeps the user-facing "issue" wording.

Both native commands are WRITES (they modify the project, so each is one undo
step on its own). `create_for` batches create+attach into a single transaction —
one undo step, and a rollback if the attach fails — using a deferred Handle so
the new issue's id flows into the attach step server-side. Speaks plain guids
like evp.selection / evp.topology.

`kind` is the component role: "Highlight" (default — a non-destructive overlay,
what the checks use), "Creation", "Deletion", or "Modification".
"""

from .api import call
from .transaction import transaction


def create(name, tag_text=None):
    """Create one Issue named `name`; return its id (a guid string).

    `tag_text`, if given, sets the Issue's tag text. Immediate write = one undo
    step; use create_for() to create-and-attach atomically instead.
    """
    params = {"name": name}
    if tag_text is not None:
        params["tagText"] = tag_text
    return (call("EvP.CreateIssue", params).data or {}).get("issueId")


def attach(issue_id, guids, kind="Highlight"):
    """Attach `guids` to the Issue `issue_id` with role `kind`.

    Returns {attached} — the number of elements submitted. Immediate write = one
    undo step. Attaching an empty list is a no-op that still succeeds.
    """
    return call("EvP.AttachElementsToIssue", {
        "issueId": issue_id,
        "guids": list(guids),
        "componentType": kind,
    }).data or {}


def create_for(name, guids, kind="Highlight", tag_text=None):
    """Create an Issue and attach `guids` to it in ONE undo step; return its id.

    Mirrors MultimodalCheck's create_issue_for: one issue per finding, elements
    attached as `kind` (Highlight by default). The create and the attach run in a
    single transaction — so a failed attach rolls the whole thing back and the
    user never sees an empty orphan Issue — with the new issue's id flowing from
    the create step into the attach step server-side via a deferred handle.
    """
    create_params = {"name": name}
    if tag_text is not None:
        create_params["tagText"] = tag_text

    with transaction("Create issue: %s" % name) as tx:
        h = tx.call("EvP.CreateIssue", create_params)
        tx.call("EvP.AttachElementsToIssue", {
            "issueId": h.issueId,   # Ref -> resolved mid-batch, after CreateIssue runs
            "guids": list(guids),
            "componentType": kind,
        })
    return tx.results[0].get("issueId")
