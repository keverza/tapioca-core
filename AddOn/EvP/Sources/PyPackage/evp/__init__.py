"""EvP — the Python surface of the Archicad add-on.

This package ships WITH the add-on and is placed at the front of sys.path, so
nothing pip installs can shadow it. `_evp` underneath is a builtin module baked
into EvPPy.dll, not a file on disk, so it cannot be shadowed at all.

Layer 1 (here) is the explicit command bus: every operation, whatever the
backend, is one envelope through one funnel. Layer 2 (evp.elements,
evp.geometry, ...) is built strictly on top of it — wrappers add types and
convenience, never private channels, so a Layer 1 trace always tells the whole
truth and any wrapper bug reduces to a single reproducible `evp.api.call` line.
"""

from . import changes
from . import context
from . import drafting
from . import drawings
from . import elements
from . import errors
from . import geometry
from . import issues
from . import layouts
from . import model
from . import outputs
from . import paths
from . import plan
from . import preview
from . import properties
from . import runtime
from . import selection
from . import topology
from . import ui
from . import webui
# NOT imported here: evp.schema. It needs pydantic, and `import evp` must keep
# working in the scanner's transport-less process and on a machine whose runtime
# baseline has not been provisioned yet. A command imports tapioca.schema itself
# and gets a clear ImportError if it is missing.
from .api import Result, EvpError, Cancelled, call, debug, API_VERSION
from .command import (
    command, action, Float, Int, Enum, Bool, Text, Action,
    Layer, Pen, Fill, LineType, Surface, Story, FilePath, ProjectField, View, Database,
    LibraryPart, Favourite,
    BuildingMaterial, WallComposite, SlabComposite, RoofComposite, ShellComposite,
    WallProfile, BeamProfile, ColumnProfile, HandrailProfile, AllProfile,
)
from .context import Context
from .plan import ElementSpec, FromStep, Plan, PlanDiff, PlanError
from .preview import PreviewBudgetError, PreviewScene
from .transaction import Transaction, TransactionError, Handle, transaction

__all__ = [
    "Result", "EvpError", "Cancelled", "call", "debug", "API_VERSION", "api",
    "Transaction", "TransactionError", "Handle", "transaction",
    "Context", "ElementSpec", "FromStep", "Plan", "PlanDiff", "PlanError",
    "PreviewScene", "PreviewBudgetError",
    "changes", "context", "drafting", "drawings", "elements", "errors", "geometry",
    "issues",
    "layouts", "outputs", "paths", "plan", "preview", "properties", "runtime", "selection",
    "model", "topology", "ui", "webui",
    "command", "action", "Float", "Int", "Enum", "Bool", "Text", "Action",
    "Layer", "Pen", "Fill", "LineType", "Surface", "Story", "FilePath", "ProjectField", "View", "Database",
    "LibraryPart", "Favourite",
    "BuildingMaterial", "WallComposite", "SlabComposite", "RoofComposite", "ShellComposite",
    "WallProfile", "BeamProfile", "ColumnProfile", "HandrailProfile", "AllProfile",
]
