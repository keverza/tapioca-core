"""Offline unit tests for evp._env's pure logic (spec parsing + version satisfaction).

Kept OUTSIDE PyPackage so it never ships with the add-on. The install/reset paths need a
real runtime + pip and are exercised by the integration run in the E7 verification, not
here. Run:  python -m pytest AddOn/EvP/tests/test_env.py
"""
import importlib.util
import os

_ENV_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp", "_env.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_env_undertest", _ENV_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


env = _load()


def test_split_spec_forms():
    assert env._split_spec("ezdxf==1.3.0") == ("ezdxf", "==", "1.3.0")
    assert env._split_spec("pillow") == ("pillow", None, None)
    assert env._split_spec("numpy>=2.0.2") == ("numpy", ">=", "2.0.2")
    assert env._split_spec("foo[extra]>=1.2") == ("foo", ">=", "1.2")
    assert env._split_spec("  bar ~= 3.4 ") == ("bar", "~=", "3.4")


def test_name():
    assert env._name("ezdxf==1.3.0") == "ezdxf"
    assert env._name("foo[a,b]<2") == "foo"


def test_version_tuple():
    assert env._version_tuple("1.3.0") == (1, 3, 0)
    assert env._version_tuple("2.0.2") == (2, 0, 2)
    assert env._version_tuple("1.4.4rc1") == (1, 4, 4)  # non-numeric suffix ignored


def test_satisfies_equality():
    assert env._satisfies("1.3.0", "==", "1.3.0")
    assert not env._satisfies("1.4.0", "==", "1.3.0")
    assert env._satisfies("1.3.0", "==", "1.3")  # tuple-normalized equality


def test_satisfies_ordering():
    assert env._satisfies("2.0.2", ">=", "2.0.2")
    assert env._satisfies("2.5.1", ">=", "2.0.2")
    assert not env._satisfies("1.9.9", ">=", "2.0.2")
    assert env._satisfies("1.0.0", "<", "2.0.0")
    assert not env._satisfies("2.0.0", "<", "2.0.0")


def test_satisfies_compatible_release():
    assert env._satisfies("1.4.9", "~=", "1.4.0")
    assert not env._satisfies("1.5.0", "~=", "1.4.0")


def test_satisfies_no_op_is_name_only():
    assert env._satisfies("anything", None, None)


def test_looks_locked_detects_windows_access_denied():
    assert env._looks_locked("ERROR: [WinError 5] Access is denied: 'numpy\\...pyd'")
    assert env._looks_locked("Access is denied")
    assert env._looks_locked("[Errno 13] Permission denied")
    assert not env._looks_locked("Successfully installed ezdxf-1.4.4")
    assert not env._looks_locked("")


def test_ensure_empty_is_noop_without_lock():
    # Empty requires must short-circuit BEFORE taking the file lock (which needs a
    # writable runtime); this is the common case for commands with no requires.
    result = env.ensure([])
    assert result == {"ok": True, "action": "noop", "requires": []}
    result = env.ensure(["  ", None])
    assert result["action"] == "noop"
