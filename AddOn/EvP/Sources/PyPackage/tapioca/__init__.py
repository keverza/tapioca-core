"""Public Tapioca command-authoring namespace.

This is a compatibility shell over the internal :mod:`evp` package.  New
commands may use ``import tapioca``; existing ``import evp`` commands continue
to run unchanged.
"""

from evp import *
from evp import __all__
