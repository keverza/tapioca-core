"""Layer 2 — zone/room topology: collisions, connected elements, zone boundaries.

    pairs = evp.topology.collisions(group1_guids, group2_guids)   # (guid1, guid2) pairs
    per   = evp.topology.connected_elements(guids, "Door")   # per-input guid lists
    bnds  = evp.topology.zone_boundaries(zone_guid)          # one zone's boundaries

The relationships the apartment grouping depends on. Backends differ by command —
`collisions` and `connected_elements` are ABSORBED NATIVELY (`EvP.GetCollisions` /
`EvP.GetConnectedElements`, ported from Tapir's ElementCommands.cpp onto the real AC29
symbols), so they no longer need Tapir installed. `zone_boundaries` still rides the E1
`Tapir.*` proxy — the apartment grouping does not use it (Apartment Graph works off
collisions + connected elements precisely because those are robust to stale /
manually-drawn zones, unlike a boundary query), so porting a ModelerAPI boundary walk
would be untested dead code. A Layer-1 trace still shows `backend:"tapir"` for it, which
is exactly what keeps "not yet absorbed" visible.

Speaks plain guids like evp.selection / evp.properties. Request/response shapes for the
native commands are EvP's flat parallel arrays (unflattened below); the shapes mirror
`02 Tapir/apt_lib.py` (get_collisions / get_connected_elements / get_zone_boundaries) so
the apartment port logic carries over verbatim. Tolerances/flags are coerced to
float/bool before the bus — ObjectState::Get drops a value on a JSON type mismatch (the
same reason evp.selection._rgba and geometry._vec exist).
"""

from .api import call


# --------------------------------------------------------------------------- #
#  Collisions (element SOLIDS -> robust to zone recalculation state)  — NATIVE #
# --------------------------------------------------------------------------- #

def collisions(group1_guids, group2_guids,
               volume_tol=0.001, surface_check=True, surface_tol=0.001, raw=False):
    """Which elements in group 1 collide with elements in group 2.

    Default (raw=False): a list of (guid1, guid2) tuples for every pair with a body
    collision OR — when surface_check is on — a clearance/surface contact. Order within
    a pair follows group1/group2. This is the flattened form apt_lib.get_collisions
    used, so port logic carries over.

    raw=True returns a list of dicts {elementId1, elementId2, hasBodyCollision,
    hasClearenceCollision} — the same shape (and Tapir's "Clearence" spelling) the raw
    Tapir payload had, for callers that must distinguish body from surface contact.

    Defaults match apt_lib's CONFIG (1 mm tolerances, surface check on) so the apartment
    ports behave identically; pass explicit values to override. Returns [] if either
    group is empty (nothing to test).
    """
    group1_guids = list(group1_guids)
    group2_guids = list(group2_guids)
    if not group1_guids or not group2_guids:
        return []
    data = call("EvP.GetCollisions", {
        "guids1": group1_guids,
        "guids2": group2_guids,
        "volumeTolerance": float(volume_tol),
        "performSurfaceCheck": bool(surface_check),
        "surfaceTolerance": float(surface_tol),
    }).data or {}

    g1 = data.get("elemGuids1", [])
    g2 = data.get("elemGuids2", [])
    body = data.get("hasBodyCollision", [])
    clearance = data.get("hasClearenceCollision", [])

    if raw:
        return [
            {"elementId1": {"guid": g1[i]}, "elementId2": {"guid": g2[i]},
             "hasBodyCollision": body[i], "hasClearenceCollision": clearance[i]}
            for i in range(len(g1))
        ]

    return [(g1[i], g2[i]) for i in range(len(g1)) if body[i] or clearance[i]]


# --------------------------------------------------------------------------- #
#  Connected elements  — NATIVE                                                #
# --------------------------------------------------------------------------- #

def connected_elements(guids, connected_type):
    """For each input element, the connected elements of `connected_type`.

    `connected_type` is an element type name (e.g. "Door", "Window", "Zone", "Wall").
    Returns a list parallel to `guids`: entry i is the list of connected element guids
    for input i (empty list when none). Mirrors apt_lib.get_connected_elements.

    Best effort: returns [] (no rows) if the call fails outright, matching apt_lib — the
    apartment graph corroborates this path with the host-wall path rather than depending
    on it alone. (An unknown `connected_type` is a hard error, not an empty result.)
    """
    guids = list(guids)
    if not guids:
        return []
    res = call("EvP.GetConnectedElements",
               {"guids": guids, "connectedElementType": connected_type},
               raise_on_error=False)
    if not res.ok:
        return []
    data = res.data or {}
    flat = data.get("connectedGuids", [])
    counts = data.get("counts", [])

    out = []
    pos = 0
    for n in counts:
        out.append(flat[pos:pos + n])
        pos += n
    return out


# --------------------------------------------------------------------------- #
#  Zone boundaries  — still the Tapir.* proxy (not used by apartment grouping) #
# --------------------------------------------------------------------------- #

def zone_boundaries(zone_guid):
    """The boundary segments of ONE zone (via the Tapir.* proxy — see module docstring).

    Returns the raw list of zoneBoundary dicts, each with keys: connectedElementId,
    isExternal, neighbouringZoneElementId, area, polygonOutline. Empty list when the
    zone has no computed boundaries. Mirrors apt_lib.get_zone_boundaries — one zone per
    call is Tapir's shape; batch by looping. NOTE: unlike collisions/connected_elements,
    this needs the Tapir add-on installed until it is absorbed natively.
    """
    data = call("Tapir.GetZoneBoundaries",
                {"zoneElementId": {"guid": zone_guid}}).data or {}
    return data.get("zoneBoundaries", [])
