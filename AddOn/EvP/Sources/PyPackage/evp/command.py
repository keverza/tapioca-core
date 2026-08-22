"""The @evp.command decorator and the parameter types the dialog generator reads.

A command folder is one command:

    scripts/PlaceSlopeSymbols/
        command.py      # @evp.command(...) + def run(...)
        helpers.py      # anything else — unrestricted
        icon.svg        # optional

    import evp

    @evp.command(title="Place Slope Symbols", category="Annotation", requires_api=">=1.0")
    def run(
        angle_text: bool = True,
        min_area: evp.Float(unit="m2") = 2.0,
        layer: evp.Layer = "Annotation",
        scope: evp.Enum("selection", "all") = "selection",
    ):
        ...

CANONICAL FORM — the entry file only (see evp._scanner):
  * `import evp` (no alias) in command.py
  * `@evp.command(...)` with LITERAL arguments
  * on a top-level `def run(...)`
Helpers and imports elsewhere in the folder are unrestricted. The contract binds
only where the tooling reads, the way pyproject.toml is TOML but your code is not.

The decorator itself is what runs at execution time; the scanner reads the same
information from the AST WITHOUT executing anything. Any drift between the two is
a bug, and the runtime cross-check reports it.
"""

#
# show_when — one input follows another
# ------------------------------------
# Every annotation class below accepts show_when={"<other param>": value} (or a
# LIST of values), and the palette hides the row whenever that other parameter
# does not currently hold one of them. The usual controller is the command's
# evp.Action, but any Enum or Bool parameter works, so show_when={"dry_run":
# False} is equally valid.
#
# Three things the scanner enforces, because each fails silently otherwise:
#   * exactly ONE controlling parameter per show_when. Two conditions would need
#     a nested shape the C++ side cannot read back, and "A and B" has never yet
#     been the thing anybody wanted;
#   * the named parameter must EXIST, and (when it is an Action, an Enum or a
#     bool) every listed value must be one of its choices. A show_when nobody can
#     satisfy is a control that never appears — a scan diagnostic, not a mystery;
#   * a hidden row is still PASSED to run(). The palette collects every parameter,
#     visible or not, so a command's signature never has to know what the palette
#     is showing; the action branch simply ignores what it does not use.
#


class Float:
    """A float parameter, optionally carrying a unit for the generated dialog.

    unit="m"/"m2"/"m3"/"rad" declares the physical dimension. Defaults, bounds,
    and values received by run() always use Archicad API base units: metres, square
    metres, cubic metres, or radians. The native DG edit independently reads the
    open project's Working Units, so users see and type cm/mm/feet/degrees/etc.

    readonly=True shows the field but locks it, so the value can come only from its
    default. default_from="project:<field name>" prefills it at dialog-build from a
    project-info field (case-insensitive substring match on the field's UI name, the
    same source File > Info > Project Info edits) — e.g. default_from=
    "project:Sklypo plotas" for a plot area. The two are usually paired: a value the
    project owns, shown but not hand-edited. These are read by the C++ dialog
    generator; the scanner folds every annotation kwarg into the param metadata, so
    they must be accepted here or evaluating the annotation would raise at import.

    show_when — see the note above.
    """

    __slots__ = ("unit", "minimum", "maximum", "readonly", "default_from", "show_when")

    def __init__(self, unit=None, minimum=None, maximum=None, readonly=False, default_from=None,
                 show_when=None):
        self.unit = unit
        self.minimum = minimum
        self.maximum = maximum
        self.readonly = readonly
        self.default_from = default_from
        self.show_when = show_when


class Int:
    __slots__ = ("minimum", "maximum", "readonly", "default_from", "show_when")

    def __init__(self, minimum=None, maximum=None, readonly=False, default_from=None,
                 show_when=None):
        self.minimum = minimum
        self.maximum = maximum
        self.readonly = readonly
        self.default_from = default_from
        self.show_when = show_when


class Enum:
    """A fixed choice — becomes a DG::PopUp."""

    __slots__ = ("choices", "show_when")

    def __init__(self, *choices, show_when=None):
        self.choices = list(choices)
        self.show_when = show_when


class Bool:
    """A checkbox. Identical to a bare `bool` annotation, except that it can carry
    kwargs — which is the whole reason it exists: `dry_run: bool = True` has
    nowhere to hang show_when or readonly, and adding them to the language is not
    an option. Bare `bool` keeps working exactly as before."""

    __slots__ = ("readonly", "show_when")

    def __init__(self, readonly=False, show_when=None):
        self.readonly = readonly
        self.show_when = show_when


class Text:
    """A text field — bare `str` with room for kwargs. See Bool."""

    __slots__ = ("readonly", "default_from", "show_when")

    def __init__(self, readonly=False, default_from=None, show_when=None):
        self.readonly = readonly
        self.default_from = default_from
        self.show_when = show_when


class Action:
    """The command's MODE, pinned to the top of its parameter block.

        action: evp.Action("Place", "Update", "Remove") = "Place"

    It renders like an Enum, but the palette places it above everything else with
    its own rule under it, and the other parameters follow it through show_when —
    so one command can present three coherent little dialogs instead of one form
    whose fields half apply.

    At most ONE per command; a second is a scan diagnostic. Two selectors that both
    reflow the block would leave the user reading a form that rearranges itself for
    reasons they cannot see — and "which mode am I in" has to have one answer.

    Sub-commands were the alternative and were rejected: the scanner's whole
    contract is one top-level `run` per folder, and everything downstream (the
    dispatcher, the transcript, the cancel token, the external runner) is built on
    exactly one entry point."""

    __slots__ = ("choices",)

    def __init__(self, *choices):
        self.choices = list(choices)


class _ProjectList:
    """A value chosen from the live project, not typed.

    Layer/Pen/Fill/LineType become Archicad's OWN attribute pickers (the
    arrow-in-a-box control), listing exactly what the open project contains.

    That the user cannot type into them is the point: a typo in a layer field
    could otherwise create a new layer, which would be a bug, not a choice. A
    picker cannot invent an attribute.

    Each name below is an INSTANCE, so `layer: evp.Layer = "Annotation"` needs no
    call. Calling one — `evp.Layer(show_when={"action": "Place"})` — returns a new
    instance carrying the kwargs, which is what lets a picker take show_when
    without every use site growing a pair of brackets.

    `FilePath(extensions=("e57", "las"))` adds arbitrary suffix filters to its
    Browse dialog. The suffixes are picker metadata; the command still receives a
    normal path string and performs its own runtime validation.
    """

    __slots__ = ("kind", "readonly", "show_when", "extensions")

    def __init__(self, kind, readonly=False, show_when=None, extensions=None):
        self.kind = kind
        self.readonly = readonly
        self.show_when = show_when
        self.extensions = tuple(extensions or ())

    def __call__(self, readonly=False, show_when=None, extensions=None):
        return _ProjectList(self.kind, readonly=readonly, show_when=show_when,
                            extensions=extensions)

    def __repr__(self):
        return "evp.%s" % self.kind


Layer = _ProjectList("Layer")        # -> APIUserControlType_Layer,      value: layer name
Pen = _ProjectList("Pen")            # -> APIUserControlType_Pen,        value: pen number (int)
Fill = _ProjectList("Fill")          # -> APIUserControlType_AllFill,    value: fill name
LineType = _ProjectList("LineType")  # -> APIUserControlType_SymbolLine, value: line type name
Surface = _ProjectList("Surface")    # -> APIUserControlType_Material,   value: surface name
Story = _ProjectList("Story")
# `extensions` is optional picker metadata, not a runtime validation rule. The
# palette uses it to register a temporary filter for arbitrary filesystem types
# that Archicad's own File Type Manager does not know about (for example E57).
FilePath = _ProjectList("FilePath")

class _ProjectField:
    """A field chosen from the open project's Project Info (File > Info > Project Info).

        plot_field: evp.ProjectField = "Sklypo plotas"
        plot_field: evp.ProjectField(numeric=True) = "Sklypo plotas"

    It answers "which field does this number come from", which a
    `default_from="project:<field>"` prefill cannot: that one names the field in
    the command's SOURCE, so a project labelling it differently needs the command
    edited. The two are complementary and usually paired — the picker chooses the
    source, default_from fills the value in at dialog-build.

    ⚠️ THE ROW SHOWS THE DESCRIPTION; THE VALUE IS THE DATABASE KEY. Same split as
    evp.View, and for the same reason: a Project Info description is user-authored
    display data and need not be unique, while the key is the identity Archicad
    itself writes by (ACAPI_AutoText_SetAnAutoText takes an `autotextDbKey`). So a
    command receives e.g. "SKLYPOPLOTAS", and looks it up in Tapioca.GetProjectInfo's
    `fieldKeys` — not `fieldNames`.

    `numeric=True` lists only the fields a number can actually be read out of.
    Stock Project Info is mostly prose — company, client, addresses, dates — so a
    picker feeding a Float parameter otherwise offers dozens of rows of which two
    are usable, and choosing a wrong one fails much later, as a missing area rather
    than as a bad choice. The test is the same one default_from resolves through,
    so "offered by the picker" and "prefills as a number" cannot disagree.

    The DEFAULT is matched the way the resolver matches: an exact key first, then a
    case-insensitive substring of the description. That is what lets a command spell
    a readable `= "Sklypo plotas"` and still land on a field labelled "Sklypo
    plotas, m2", while a source that names the key hits it precisely.

    Its own class rather than another _ProjectList entry, for the same reason
    _LibraryPart and _Favourite are: `numeric` is meaningless on a Layer or a Pen,
    and every picker would have had to accept it.
    """

    __slots__ = ("kind", "numeric", "readonly", "show_when")

    def __init__(self, numeric=False, readonly=False, show_when=None):
        self.kind = "ProjectField"
        self.numeric = numeric
        self.readonly = readonly
        self.show_when = show_when

    def __call__(self, numeric=False, readonly=False, show_when=None):
        return _ProjectField(numeric=numeric, readonly=readonly, show_when=show_when)

    def __repr__(self):
        return "evp.ProjectField"


ProjectField = _ProjectField()

# Navigator pickers. ⚠️ THESE TWO ARE THE ONE PLACE A PICKER'S VALUE IS NOT A NAME,
# and the reason is measured, not stylistic: navigator names are NOT UNIQUE (a stock
# project holds 12 views called "Story"), so a name identifies nothing. A guid does —
# but a guid is invisible in Archicad's UI, so a user cannot supply one either. The
# picker is the only shape that closes the gap: the ROW shows `name — folder/path`,
# the VALUE handed to run() is the guid. Never ask for either half as text.
View = _ProjectList("View")          # -> placeable View Map items,     value: guid
Database = _ProjectList("Database")  # -> independent databases,        value: guid


class _LibraryPart:
    """A library OBJECT chosen from the project's libraries, not typed.

        chair: evp.LibraryPart = None
        lamp:  evp.LibraryPart(subtype="Lamp") = None

    A BUTTON that opens the same thing Archicad's Object Settings browser shows:
    the placeable objects of the embedded and loaded libraries, in the Library
    Manager's own folder tree.

    ⚠️ IT LISTS WHAT CAN BE PLACED, AND ONLY THAT. Macros, textures, list
    schemes and templates are all registered library parts and none of them can
    be placed, so none of them appear (`isPlaceable` / `isTemplate`). The first
    version listed every registered part and the verdict was "data is all over
    the place" — surfaces, images, lights and section markers in a picker whose
    question is "which object do I place".

    `subtype` — "Object" (the default), "Lamp", "ZoneStamp", "Label", or "all"
    for the unfiltered catalogue.

    ⚠️ DOORS, WINDOWS AND SKYLIGHTS ARE OUT OF SCOPE and are REFUSED, not
    silently served. An opening is cut into a host wall or roof, so choosing one
    needs that host's context; a list that merely named doors would be a control
    that looks finished and cannot do the job. Ask for one and the browser opens
    saying so.

    ⚠️ THE VALUE IS A DICT, NOT A STRING — the one shape difference from every
    other picker, and it is forced by the data rather than chosen:

        {"name": "Slope Symbol 29", "unID": "{...}-...", "type": "Object",
         "file": "Slope Symbol 29.gsm", "location": "C:\\\\...\\\\Slope Symbol 29.gsm"}

    A library part's DOCUMENT NAME IS NOT UNIQUE (Archicad registers only the
    newest of a duplicate name, and two loaded libraries routinely carry the same
    part), so a name identifies nothing on its own — the same reason evp.View
    sends a guid. What IS stable across sessions is `unID`, the part's own
    unique-ID string, and that is what a placement command should carry. But a
    unID is unreadable and never appears in Archicad's UI, so a log or an alert
    that quotes it says nothing. Sending both closes the gap: act on `unID`,
    print `name`.

    readonly / show_when — as on every other picker; see _ProjectList.
    """

    __slots__ = ("kind", "subtype", "readonly", "show_when")

    def __init__(self, subtype=None, readonly=False, show_when=None):
        self.kind = "LibraryPart"
        self.subtype = subtype
        self.readonly = readonly
        self.show_when = show_when

    def __call__(self, subtype=None, readonly=False, show_when=None):
        return _LibraryPart(subtype=subtype, readonly=readonly, show_when=show_when)

    def __repr__(self):
        return "evp.LibraryPart"


class _Favourite:
    """A favourite chosen from the project's Favourites, not typed.

        template: evp.Favourite = None
        wall:     evp.Favourite(element_type="Wall") = None

    A BUTTON that opens a browsable modal tree of the project's favourites, in
    their real folder hierarchy. `element_type` narrows it to one element type —
    "Wall", "Slab", "Roof", "Shell", "Mesh", "Column", "Beam", "Window", "Door",
    "Object", "Lamp", "Stair", "Railing", "CurtainWall", "Morph", "Zone" and the
    rest of the toolbox; omitted means every type.

    The value is a dict, for the same reason evp.LibraryPart's is:

        {"name": "Exterior 300", "elementType": "Wall", "folder": ["Walls", "Ext"]}

    ⚠️ A FAVOURITE'S IDENTITY IS ITS NAME. Unlike a library part or a navigator
    item it has no guid and no unID anywhere in the API — ACAPI_Favorite_Get,
    _Change, _Rename and _Delete all key on the name. So `name` here is not a
    label beside the real value, it IS the value; `elementType` and `folder` are
    what make it readable and what let a command refuse a favourite of the wrong
    type before it acts.

    British spelling deliberately: it matches the Archicad UI's own "Favourites".
    The ACAPI symbols are American ("ACAPI_Favorite_*") and stay that way.

    readonly / show_when — as on every other picker; see _ProjectList.
    """

    __slots__ = ("kind", "element_type", "readonly", "show_when")

    def __init__(self, element_type=None, readonly=False, show_when=None):
        self.kind = "Favourite"
        self.element_type = element_type
        self.readonly = readonly
        self.show_when = show_when

    def __call__(self, element_type=None, readonly=False, show_when=None):
        return _Favourite(element_type=element_type, readonly=readonly, show_when=show_when)

    def __repr__(self):
        return "evp.Favourite"


LibraryPart = _LibraryPart()
Favourite = _Favourite()

# Structure attributes. The value handed to run() is the attribute NAME (a string);
# a create command resolves name -> API_AttributeIndex server-side (never guess).
#
# The picker per element family is the "native toggle": each control type shows only
# the composites/profiles that family can use. Composites exist for Wall/Slab/Roof/
# Shell (beams and railings have no composites — they use PROFILES); profiles exist
# per family too. Pick the one that matches the element you are about to create.
BuildingMaterial = _ProjectList("BuildingMaterial")  # -> basic-structure element material
WallComposite = _ProjectList("WallComposite")        # -> composites usable on walls
SlabComposite = _ProjectList("SlabComposite")        # -> composites usable on slabs
RoofComposite = _ProjectList("RoofComposite")        # -> composites usable on roofs
ShellComposite = _ProjectList("ShellComposite")      # -> composites usable on shells
WallProfile = _ProjectList("WallProfile")            # -> complex profiles usable on walls
BeamProfile = _ProjectList("BeamProfile")            # -> complex profiles usable on beams
ColumnProfile = _ProjectList("ColumnProfile")        # -> complex profiles usable on columns
HandrailProfile = _ProjectList("HandrailProfile")    # -> complex profiles usable on railings
AllProfile = _ProjectList("AllProfile")              # -> every complex profile


#: The band types the palette can size. "text" is the fallback every planning
#: command gets for free — the Plan.diff() rendered into the results panel.
PREVIEW_KINDS = ("text", "3d", "plan2d")


def command(title=None, category="General", requires_api=None, requires_tapir=None, runtime="embedded",
             description=None, requires=None, needs_selection=False, labels=None,
             timeout_s=0, tags=None, selection_sets=None,
             inputs=None, outputs=None, plan=None, needs_preview=False,
             preview=None, preview_kind=None, actions=None):
    """Mark `run` as an EvP command.

    title        shown in the palette (defaults to the folder name)
    category     palette grouping
    tags         related terms the palette's search box also matches, e.g.
                 ["roof", "slope", "pitch", "kaltas"]. They are NOT shown anywhere —
                 they exist so a command can be found by a word its title does not
                 contain: the term the user thinks in, the synonym, the other
                 language, the element type it works on. A title match always ranks
                 above a tag match, so tags widen the net without burying the
                 obvious answer. Cheap to add and worth adding: a command nobody can
                 find is a command nobody runs.
    requires_api semver range checked against evp.API_VERSION, e.g. ">=1.1"
    requires_tapir semver range checked against the installed Tapir add-on, e.g.
                 ">=1.5.2". A command with this declaration never starts when
                 Tapir is absent or below the requested version.
    runtime      "embedded" (in-process) or "external" (subprocess) — the plan's
                 escape hatch for ctypes/C-extension-heavy or uninterruptible work
    requires     pinned packages the palette installs, e.g. ["numpy==2.0.2"]
    labels       optional {param_name: "shown text"} — the display label for a
                 parameter's input row, replacing the raw parameter name. The name
                 is an ASCII Python identifier (no spaces, no diacritics); a label
                 can be any text, so a command can present its inputs in the user's
                 language. A param with no entry keeps its name. The scanner folds
                 each entry into that parameter's metadata; see evp._scanner.
    timeout_s    E9 — seconds after which the run cancels itself. 0 (the default)
                 means no timeout: a command that legitimately runs for an hour must
                 not be guessed at. Set it where a runaway is plausible and nobody
                 may be watching the palette to press Stop. The deadline is enforced
                 wherever the run is polled — every bus call, every
                 evp.runtime.check_cancel(), and the external subprocess's drain
                 loop — so a pure-compute loop that does none of those is NOT timed
                 out. Cancelling is cooperative; see evp.runtime.
    needs_selection
                 the palette keeps Run DISABLED until something is selected, and
                 says so on the status line. A precondition the palette can check
                 is worth far more than the same check inside run(), because the
                  user finds out before pressing the button rather than after.
    selection_sets
                  ordered role names for the optional selection-manager panel,
                  e.g. ("Targets", "Operators"). The command reads those saved
                  sets through evp.selection.sets; the panel is absent when omitted.

    inputs       a tapioca.schema.NodeModel subclass declaring the command's
                 ports. Declaring one moves the UI contract out of run()'s
                 annotations and into a model that also VALIDATES what arrives:
                 `Field(ge=0)` becomes both a spin-box minimum and a rejection.
                 The palette gets its controls from the model's JSON Schema
                 (evp._ports), not from the signature, and run() is then called
                 as run(ctx, inputs) with one validated object.

                 Omitting it keeps the original form — annotated parameters on
                 run() — which every existing command uses and which stays
                 supported. The two are alternatives, never mixed: a command
                 that declares inputs= and also annotates run()'s parameters
                 would have two sources for the same control, and nothing could
                 say which one the palette rendered.
    outputs      a NodeModel describing what run() returns. Validated on the way
                 out for the same reason inputs are validated on the way in —
                 a consumer (the results table, WebUI, a notebook cell) binds to
                 the declared shape, and an undeclared return is a shape that
                 drifts silently.
    plan         a callable (inputs, ctx) -> tapioca.Plan that computes the
                 intended writes WITHOUT performing them. It is what the preview
                 band renders and what Run commits, so the picture the user
                 approved and the change that lands come from one computation
                 rather than two.
    needs_preview
                 the palette shows its preview band for this command. A command
                 that declares plan= usually wants this; one that only reads
                 usually does not.
    preview      a callable (inputs, ctx) -> None that fills `ctx.scene` with a
                 REPRESENTATIVE FRAGMENT — one wall of a stack, one label
                 placement — not the model and not the whole plan. Omitted, a
                 command with plan= still previews: the band falls back to the
                 text diff, which needs no command code at all.
    preview_kind "3d" | "plan2d" | "text". DECLARED, never inferred from what the
                 scene happens to hold, because the palette has to size the band
                 before any command code runs. Defaults to "text" when preview= is
                 absent and "3d" when it is present.
    actions      names from evp.outputs.STANDARD_ACTIONS ("csv", "pdf", "bake", …)
                 the palette offers as buttons under the results. A named action
                 needs NO export code: the framework builds it from the Outputs
                 field marked role="table". Anything the standard set cannot
                 express is the command's own function instead.

    A parameter with NO default in run()'s signature is likewise treated as
    required: Run stays disabled until it has a usable value.
    """
    if inputs is None and (outputs is not None or plan is not None):
        raise TypeError(
            "outputs=/plan= need inputs=: they describe a command whose ports "
            "are a model, and without one the palette still reads run()'s "
            "annotations, so the declared shapes would never be applied."
        )

    def decorate(fn):
        fn.__evp_command__ = {
            "title": title,
            "category": category,
            "requires_api": requires_api,
            "requires_tapir": requires_tapir,
            "runtime": runtime,
            "description": description,
            "requires": list(requires) if requires else [],
            "tags": [str(t) for t in tags] if tags else [],
            "needs_selection": bool(needs_selection),
            "timeout_s": float(timeout_s or 0),
            "labels": dict(labels) if labels else {},
            "selection_sets": list(selection_sets) if selection_sets else [],
            "needs_preview": bool(needs_preview) or preview is not None,
            "preview_kind": _preview_kind(preview_kind, preview),
            "actions": [str(name) for name in actions] if actions else [],
            # The models themselves, for the RUNTIME. The palette never sees
            # these -- it reads the JSON Schema the scanner cached, because it
            # must not import a command module to draw its controls.
            "inputs": inputs,
            "outputs": outputs,
            "plan": plan,
            "preview": preview,
        }
        return fn

    return decorate


def _preview_kind(declared, preview_fn):
    """The band type. Declared wins; otherwise a command with a preview() gets 3d
    and one without gets the text diff — which is free and always available."""
    if declared is not None:
        if declared not in PREVIEW_KINDS:
            raise ValueError(
                "unknown preview_kind %r. Known: %s"
                % (declared, ", ".join(PREVIEW_KINDS)))
        return declared
    return "3d" if preview_fn is not None else "text"


def action(label, name=None):
    """Mark a module-level function as one of this command's OWN actions.

    The flexible half of the output API. The standard set
    (`evp.outputs.STANDARD_ACTIONS`, named on `@command(actions=[...])`) covers
    what every command needs; this covers what one command needs, so that the
    standard set never has to grow to accommodate a single caller.

        @tapioca.action("Export DXF")
        def export_dxf(ctx, outputs):
            ...            # `outputs` is the LAST run's result, from the store

    ⚠️ THE COMMAND IS NOT RE-RUN to serve a button. `outputs` is the plain dict
    the run store kept, not a live model — the action runs in a fresh process and
    importing the command to rebuild the class would be exactly the re-run the
    store exists to avoid.

    `label` is what the button says and MUST be a literal: the palette reads it
    with the AST scanner, which never executes the file.
    """
    def decorate(fn):
        fn.__evp_action__ = {"name": name or fn.__name__, "label": str(label)}
        return fn

    return decorate


def menu(label, region="panel", name=None):
    """Put one of this command's OWN actions in the palette's RIGHT-CLICK menu.

    The context-sensitive half of the output API. `@tapioca.action` puts an entry
    in the action bar, where every command's buttons sit in the same place; this
    puts one under the pointer, where WHAT WAS CLICKED is part of the question.

        @tapioca.menu("Reset offsets", region="params")
        def reset_offsets(ctx, outputs):
            ...            # `outputs` is the LAST run's result, from the store

    A MENU ENTRY IS AN ACTION. Same `(ctx, outputs)` contract, same run store,
    same worker, same Cancel — the palette dispatches it down the identical path
    an action button uses, so nothing about running it is a special case. The only
    thing `menu` adds is WHERE it appears.

    `region` says where that is:

        "panel"         anywhere on the palette — the default
        "params"        over any generated parameter control
        "param:<name>"  over ONE named parameter's control, e.g. "param:offset"
        "commands"      over the command list and its search field
        "results"       over the results table

    A "param:<name>" region naming a parameter the command does not declare is a
    scan error, not a menu entry that never appears: the palette reads these
    statically, so a typo can be caught at Rescan instead of by a user wondering
    why right-clicking does nothing.

    ⚠️ THE COMMAND IS NOT RE-RUN to serve an entry, exactly as for `action` — and
    `outputs` is None when nothing has run yet, so an entry that acts on a result
    must say so rather than assume one.

    `label` and `region` MUST be literals: the palette reads them with the AST
    scanner, which never executes the file.
    """
    def decorate(fn):
        fn.__evp_action__ = {"name": name or fn.__name__, "label": str(label)}
        fn.__evp_menu__ = {"region": str(region)}
        return fn

    return decorate
