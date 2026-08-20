"""Layer 2 — Archicad property definitions and their values on elements.

    pid   = evp.properties.builtin_id("Zone_ZoneName")        # resolve by name
    pid2  = evp.properties.userdefined_id("Apartment", "Number")
    rows  = evp.properties.values(guids, [pid, pid2])          # read, per element
    evp.properties.set_values([(guid, pid2, "A-101")])         # write, one batch

E5. This is a PURE wrapper on Layer 1: every call routes through
`evp.api.call("API.*", ...)`, which the dispatcher runs in-process through the gate
(ApiDispatcher.cpp:465). There is NO new native code — the transport already exists.
Names, request shapes and result keys are taken from the local `archicad` pypi package
(reference/python-api-archicad-29.3000/.../ac29/b3000commands.py + b3000types.py), the
only trustworthy source for API.* shapes — never from Tapir docs.

Absorbing Tapir: `02 Tapir/apt_lib.py` used Tapir's convenience `GetAllProperties` to
resolve a user-defined id by group+name. There is no single official equivalent, but
`API.GetPropertyIds` accepts a UserDefined PropertyUserId ({"type":"UserDefined",
"localizedName":[group, name]}) and resolves it directly — so `userdefined_id()` below
needs no Tapir. Only property *creation* has no JSON-API command at all; that stays out
of scope here (the ports never create at runtime — see the handoff).

What is NOT covered: creating property groups/definitions (no JSON API command exists;
needs native ACAPI or a one-time Tapir script), and component-level reads
(GetPropertyValuesOfElementComponents) — add when a port needs them.
"""

from .api import call


# --------------------------------------------------------------------------- #
#  Element / property id wrappers — the two array-item shapes the API wants    #
# --------------------------------------------------------------------------- #

def _element_items(guids):
    """[guid, ...] -> [{"elementId": {"guid": guid}}, ...]  (ElementIdArrayItem)."""
    return [{"elementId": {"guid": g}} for g in guids]


def _property_items(property_ids):
    """[pid, ...] -> [{"propertyId": pid}, ...]  (PropertyIdArrayItem).

    `pid` is a PropertyId dict, i.e. {"guid": "..."} — exactly what the resolver
    functions below return.
    """
    return [{"propertyId": pid} for pid in property_ids]


# --------------------------------------------------------------------------- #
#  Definitions — groups, names, details, availability                          #
# --------------------------------------------------------------------------- #

def group_ids(property_type=None):
    """Every property group id. `property_type` filters: "BuiltIn"/"UserDefined".

    Returns a list of PropertyGroupIdArrayItem dicts ({"propertyGroupId": {...}}).
    """
    params = {}
    if property_type is not None:
        params["propertyType"] = property_type
    return (call("API.GetAllPropertyGroupIds", params).data or {}).get("propertyGroupIds", [])


def groups(group_id_items):
    """Details for the given group ids. `group_id_items` are PropertyGroupIdArrayItem
    dicts (as returned by group_ids()). Returns a list of PropertyGroupOrError."""
    params = {"propertyGroupIds": list(group_id_items)}
    return (call("API.GetPropertyGroups", params).data or {}).get("propertyGroups", [])


def all_names():
    """Every property's human-readable PropertyUserId — for debug/discovery.

    Each item is {"type", "localizedName":[group, name]} for user-defined, or
    {"type":"BuiltIn", "nonLocalizedName": ...} for built-in.
    """
    return (call("API.GetAllPropertyNames").data or {}).get("properties", [])


def details(property_ids):
    """Definition details (name, group, type, ...) for property ids. Returns a list
    of PropertyDefinitionOrError, in request order."""
    params = {"properties": _property_items(property_ids)}
    return (call("API.GetDetailsOfProperties", params).data or {}).get("propertyDefinitions", [])


def availability(property_ids):
    """Which element classifications each property id applies to. Returns a list of
    PropertyDefinitionAvailabilityOrError, in request order."""
    params = {"propertyIds": _property_items(property_ids)}
    data = call("API.GetPropertyDefinitionAvailability", params).data or {}
    return data.get("availabilities", [])


# --------------------------------------------------------------------------- #
#  Id resolution by name                                                       #
# --------------------------------------------------------------------------- #

def ids(user_ids):
    """Resolve PropertyUserId dicts to PropertyIdOrError, in request order.

    `user_ids` are the name-based ids: {"type":"BuiltIn","nonLocalizedName": ...} or
    {"type":"UserDefined","localizedName":[group, name]}. The lower-level primitive;
    builtin_id()/userdefined_id() wrap the common single-lookup cases.
    """
    params = {"properties": list(user_ids)}
    return (call("API.GetPropertyIds", params).data or {}).get("properties", [])


def builtin_id(non_localized_name):
    """The PropertyId of a built-in property (e.g. "Zone_ZoneName").

    Returns the PropertyId dict ({"guid": ...}) or raises if it does not resolve.
    """
    entry = ids([{"type": "BuiltIn", "nonLocalizedName": non_localized_name}])[0]
    if "propertyId" not in entry:
        raise KeyError("built-in property not found: %s" % non_localized_name)
    return entry["propertyId"]


def userdefined_id(group_name, property_name):
    """The PropertyId of a user-defined property, by group + name.

    Resolves via API.GetPropertyIds with a UserDefined PropertyUserId — the official
    replacement for Tapir's GetAllProperties. Raises if it does not resolve (which,
    as in apt_lib, usually means the property has not been created yet).
    """
    entry = ids([{"type": "UserDefined", "localizedName": [group_name, property_name]}])[0]
    if "propertyId" not in entry:
        raise KeyError('user-defined property not found: "%s" / "%s"' % (group_name, property_name))
    return entry["propertyId"]


def try_builtin_id(non_localized_name):
    """builtin_id() but returns None instead of raising."""
    try:
        return builtin_id(non_localized_name)
    except Exception:
        return None


# --------------------------------------------------------------------------- #
#  Values — read and write                                                     #
# --------------------------------------------------------------------------- #

def values(guids, property_ids, only_supported_types=None, raw=False):
    """Property values for `guids` × `property_ids`.

    Default (raw=False): a list (per element, in `guids` order) of lists (per
    property, in `property_ids` order), each entry the plain value or None when the
    value is not "normal" (missing / userUndefined / notAvailable / notEvaluated) —
    the same flattening apt_lib.get_property_values used, so port logic carries over.

    raw=True returns the untouched PropertyValuesOrError list, for callers that need
    the status/type or must see per-element errors.
    """
    params = {"elements": _element_items(guids), "properties": _property_items(property_ids)}
    if only_supported_types is not None:
        params["onlySupportedTypes"] = bool(only_supported_types)
    per_element = (call("API.GetPropertyValuesOfElements", params).data or {}).get(
        "propertyValuesForElements", [])
    if raw:
        return per_element

    out = []
    for pv_for_elem in per_element:
        row = []
        for pv in pv_for_elem.get("propertyValues", []):
            value_obj = pv.get("propertyValue", {})
            if value_obj.get("status", "normal") == "normal" and "value" in value_obj:
                row.append(value_obj["value"])
            else:
                row.append(None)
        out.append(row)
    return out


# The property kind tags the API's NormalOrUserUndefinedPropertyValue union accepts.
# Used by set_values when a caller cannot rely on Python-value inference (e.g. an
# integer property whose value arrives as a display string, or clearing a non-string
# property with None/"").
_PROPERTY_KINDS = (
    "number", "integer", "string", "boolean",
    "length", "area", "volume", "angle",
    "numberList", "integerList", "stringList", "booleanList",
    "lengthList", "areaList", "volumeList", "angleList",
    "singleEnum", "multiEnum",
)


def _infer_property_kind(value):
    """Best-effort NormalOrUserUndefinedPropertyValue `type` tag for a Python value.

    bool is checked BEFORE int (bool subclasses int). A list infers its *element*
    kind; mixed-type lists fall back to stringList (the only kind Tapir/Archicad
    display values can round-trip without a hint). Anything unknown also falls
    back to "string" — the most permissive scalar kind on the write path.
    """
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        if value and all(isinstance(x, bool) for x in value):
            return "booleanList"
        if value and all(isinstance(x, int) for x in value):
            return "integerList"
        if value and all(isinstance(x, (int, float)) for x in value):
            return "numberList"
        return "stringList"
    return "string"


def _make_property_value(value, kind=None):
    """Build the typed union member: {"type","status","value"} for a normal value,
    or {"type","status":"userUndefined"} (NO `value` field) to clear it.

    `kind` overrides the inferred type tag — pass it when the property's stored
    type does not match the Python value's (e.g. an integer property whose value
    arrives as a display string, or clearing a non-string property with None/"").
    Automated inference picks the right kind for str/int/float/bool/list values.
    """
    if kind is None:
        kind = _infer_property_kind(value)
    # An empty string or None means "clear". The UserUndefinedPropertyValue union
    # member has type + status only — NO `value` field — so omitting it is what
    # makes the API mark the property as userUndefined instead of storing "".
    if value is None or (isinstance(value, str) and value == ""):
        return {"type": kind, "status": "userUndefined"}
    return {"type": kind, "status": "normal", "value": value}


def set_values(entries):
    """Write property values. `entries` is a list of tuples:

        (guid, property_id, value)              # 3-tuple — type inferred from value
        (guid, property_id, value, kind)        # 4-tuple — explicit kind tag

    All entries go in ONE API.SetPropertyValuesOfElements call — a single JSON-API
    round trip, hence a single undo step. `value` is a scalar (str/int/float/bool)
    or a list for list-typed properties.

    An empty string or None CLEARS a property (sent as `userUndefined` status —
    the API union member with NO `value` field). To clear a NON-string property,
    pass the 4-tuple form with the property's real kind, e.g.
        (guid, pid, None, "integer")

    `kind` is one of the NormalOrUserUndefinedPropertyValue type tags
    ("string", "integer", "number", "boolean", "length", "area", "volume",
    "angle", and their *List variants, "singleEnum", "multiEnum"). Auto-inference
    picks the right one for str/int/float/bool/list Python values; pass the 4-tuple
    form when the property's stored type doesn't match the Python value's.

    Returns the API's per-entry executionResults (each {"success"} or an error), in
    entry order.

    ⚠️ Value shape: the official API.SetPropertyValuesOfElements takes the
    `NormalOrUserUndefinedPropertyValue` UNION, whose JSON schema requires the
    `type` discriminator (and `status`) on every entry. The bare `{"value": v}`
    shape apt_lib used against Tapir's proxy is rejected by the official command
    ("Validation failed on schema rule 'oneOf' on propertyValue") — confirmed live
    on the ApartmentGraph port. This sends the full union member, so str→string,
    int→integer, float→number, bool→boolean, list→<elem>List, and "" or None
    → userUndefined.
    """
    payload = []
    for entry in entries:
        if len(entry) == 4:
            guid, pid, value, kind = entry
        else:
            guid, pid, value = entry
            kind = None
        payload.append({
            "elementId": {"guid": guid},
            "propertyId": pid,
            "propertyValue": _make_property_value(value, kind),
        })
    if not payload:
        return []
    params = {"elementPropertyValues": payload}
    return (call("API.SetPropertyValuesOfElements", params).data or {}).get("executionResults", [])
