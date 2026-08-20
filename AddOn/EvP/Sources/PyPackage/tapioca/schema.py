"""Public spelling of :mod:`evp.schema` — the models a command's ports are
written as.

A separate module rather than a re-export from ``tapioca/__init__.py``, because
``evp.schema`` needs pydantic and ``import tapioca`` must keep working without
it. Importing THIS module is the command's explicit statement that it wants the
schema surface, and the ImportError it gets when the runtime baseline has not
been provisioned names the missing package instead of failing somewhere else.
"""

from evp.schema import *  # noqa: F403
from evp.schema import __all__  # noqa: F401
