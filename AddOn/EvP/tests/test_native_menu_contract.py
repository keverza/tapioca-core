"""Offline resource/registration contract for the native Tapioca menu (#172)."""

from pathlib import Path
import re


SOURCES = Path(__file__).resolve().parents[1] / "Sources"
ADDON = SOURCES / "AddOn"
EXPECTED = [
    ("GeometryServerMenuResId", "Tapioca Panel", "MenuFlag_Default"),
    ("GraphEditorMenuResId", "Node Graph", "MenuFlag_Default"),
    ("ArchVizMenuResId", "Vision Window", "MenuFlag_SeparatorBefore"),
    ("Overlay2DMenuResId", "2D Vision Overlay", "MenuFlag_Default"),
    ("ArchVizOverlayMenuResId", "3D Vision Overlay", "MenuFlag_Default"),
    ("GrasshopperMenuResId", "Close Grasshopper", "MenuFlag_SeparatorBefore"),
    ("GrasshopperEditorMenuResId", "Grasshopper Editor", "MenuFlag_Default"),
    ("RhinoComputeStartMenuResId", "Start rhino.compute", "MenuFlag_Default"),
    ("RhinoComputeStopMenuResId", "Stop rhino.compute", "MenuFlag_Default"),
    ("GrasshopperRunMenuResId", "Run Grasshopper Definition", "MenuFlag_Default"),
    ("AboutMenuResId", "About Tapioca...", "MenuFlag_SeparatorBefore"),
]


def test_registration_order_and_group_separators():
    main = (ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    registrations = re.findall(
        r"ACAPI_MenuItem_RegisterMenu\s*\(\s*(\w+),\s*0,\s*MenuCode_UserDef,\s*(\w+)\s*\)",
        main,
    )
    assert registrations == [(resource, flags) for resource, _, flags in EXPECTED]
    handlers = re.findall(
        r"ACAPI_MenuItem_InstallMenuHandler\s*\(\s*(\w+),\s*MenuCommandHandler\s*\)",
        main,
    )
    assert handlers == [resource for resource, _, _ in EXPECTED]


def test_each_menu_resource_has_one_item_and_keeps_view_enable_codes():
    ids = dict(re.findall(
        r"constexpr short (\w+) = (\d+);",
        (ADDON / "ResourceIds.hpp").read_text(encoding="utf-8"),
    ))
    grc = (SOURCES / "AddOnResources/RINT/AddOn.grc").read_text(encoding="utf-8")
    grc = re.sub(r"/\*.*?\*/", "", grc, flags=re.DOTALL)
    resources = dict(re.findall(r"'STR#'\s+(\d+)\s+\"[^\"]*\"\s*\{(.*?)\}", grc, re.DOTALL))
    for resource, label, _ in EXPECTED:
        assert re.findall(r'"([^\"]*)"', resources[ids[resource]]) == [
            "Tapioca", label + "^E3^ES^EE^EI^ED^EW^EL"
        ]
    assert re.search(r"'GDLG'\s+32540[^\n]*\"Vision Window\"", grc)
