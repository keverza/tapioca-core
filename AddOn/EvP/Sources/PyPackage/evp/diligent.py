"""Diligent camera snapshots, named presets, and fixed-size captures."""

import json
import math
import os
import re

from . import paths

_CAMERA_FIELDS = (
    "valid", "source", "orthographic", "viewMoving",
    "eyeX", "eyeY", "eyeZ", "targetX", "targetY", "targetZ",
    "viewConeDegreesHorizontal",
)
_NUMBER_FIELDS = _CAMERA_FIELDS[4:]
_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")


def get_camera():
    """Return the current visible Diligent perspective camera."""
    from .api import call

    result = call("Tapioca.GetDiligentCamera", {}, raise_on_error=False)
    if not result.ok:
        raise RuntimeError("GetDiligentCamera failed: %s" % _error(result))
    return _validate_camera(result.data or {})


def save_camera(name, camera=None, replace=False):
    """Save a camera under ``presets/diligent-cameras`` using atomic replacement."""
    name = _validate_name(name)
    camera = _validate_camera(get_camera() if camera is None else camera)
    folder = paths.presets_dir("diligent-cameras")
    path = _preset_path(name, folder)
    existing = {item.casefold(): item for item in list_cameras()}
    previous = existing.get(name.casefold())
    if previous and not replace:
        raise FileExistsError("Diligent camera preset %r already exists" % previous)
    if previous:
        path = _preset_path(previous, folder)

    temporary = path + ".tmp-%d" % os.getpid()
    try:
        with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"version": 1, "name": previous or name, "camera": camera}, handle,
                      ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return path


def list_cameras():
    """List valid preset names in stable case-insensitive order."""
    folder = paths.presets_dir("diligent-cameras")
    names = []
    for filename in os.listdir(folder):
        if filename.endswith(".json") and _NAME.fullmatch(filename[:-5]):
            names.append(filename[:-5])
    return sorted(names, key=str.casefold)


def load_camera(name):
    """Load and validate one named perspective camera."""
    name = _resolve_name(_validate_name(name))
    with open(_preset_path(name, paths.presets_dir("diligent-cameras")),
              encoding="utf-8") as handle:
        record = json.load(handle)
    if record.get("version") != 1 or record.get("name") != name:
        raise ValueError("Diligent camera preset %r has an invalid record header" % name)
    return _validate_camera(record.get("camera") or {})


def delete_camera(name):
    """Delete a preset, returning false when it did not exist."""
    name = _validate_name(name)
    existing = {item.casefold(): item for item in list_cameras()}
    resolved = existing.get(name.casefold(), name)
    path = _preset_path(resolved, paths.presets_dir("diligent-cameras"))
    try:
        os.remove(path)
        return True
    except FileNotFoundError:
        return False


def apply_camera(name):
    """Apply a preset to the running visible Diligent viewer."""
    from .api import call

    camera = load_camera(name)
    params = {key: camera[key] for key in _NUMBER_FIELDS}
    result = call("Tapioca.SetDiligentCamera", params, raise_on_error=False)
    if not result.ok:
        raise RuntimeError("SetDiligentCamera failed: %s" % _error(result))
    return result.data or {}


def capture(name, preset, width, height, render_quality="realistic", **kwargs):
    """Capture through :mod:`tapioca.outputs` using a named camera preset."""
    from . import outputs

    return outputs.diligent_capture(name, load_camera(preset), width, height,
                                    render_quality=render_quality, **kwargs)


def _validate_name(name):
    if not isinstance(name, str) or not _NAME.fullmatch(name):
        raise ValueError("preset name must match [A-Za-z0-9][A-Za-z0-9_-]{0,63}")
    return name


def _validate_camera(camera):
    if set(camera) != set(_CAMERA_FIELDS):
        raise ValueError("camera must contain exactly: %s" % ", ".join(_CAMERA_FIELDS))
    if camera["valid"] is not True or camera["orthographic"] is not False:
        raise ValueError("only valid perspective Diligent cameras can be saved")
    if not isinstance(camera["source"], str) or not isinstance(camera["viewMoving"], bool):
        raise ValueError("camera source and viewMoving have invalid types")
    for key in _NUMBER_FIELDS:
        value = camera[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError("camera field %s must be a finite number" % key)
        if key != "viewConeDegreesHorizontal" and abs(value) > 1e15:
            raise ValueError("camera field %s exceeds the supported coordinate range" % key)
    cone = camera["viewConeDegreesHorizontal"]
    if cone <= 1 or cone >= 179:
        raise ValueError("viewConeDegreesHorizontal must be in (1, 179)")
    dx = camera["eyeX"] - camera["targetX"]
    dy = camera["eyeY"] - camera["targetY"]
    dz = camera["eyeZ"] - camera["targetZ"]
    if dx * dx + dy * dy + dz * dz <= 1e-8:
        raise ValueError("camera eye and target must be distinct")
    return {key: camera[key] for key in _CAMERA_FIELDS}


def _preset_path(name, folder):
    return os.path.join(folder, name + ".json")


def _resolve_name(name):
    existing = {item.casefold(): item for item in list_cameras()}
    return existing.get(name.casefold(), name)


def _error(result):
    return (result.error or {}).get("message") or "no reason given"
