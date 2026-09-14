"""Session-only perspective camera sets declared by the active command."""

from .api import call


class _CameraSets:
    """Ordered camera lists declared with ``camera_sets=(...)``."""

    def get(self, name):
        """Return captured cameras ready for ``outputs.diligent_capture_batch``."""
        return list((call("Tapioca.GetCameraSet", {"name": str(name)}).data or {}).get("cameras", []))


sets = _CameraSets()
