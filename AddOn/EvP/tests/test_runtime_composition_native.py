"""Source contracts for the palette and release runtime composition."""

import re
from pathlib import Path

EVP_ROOT = Path(__file__).resolve().parents[1]
CORE_ROOT = EVP_ROOT.parents[1]
ADDON = EVP_ROOT / "Sources" / "AddOn"


def test_command_rescan_uses_only_the_scanned_catalog():
    source = (ADDON / "Palette" / "CommandListPanel.cpp").read_text(encoding="utf-8")
    rescan = source.split("GS::UniString CommandListPanel::Rescan ()", 1)[1].split(
        "GS::UniString CommandListPanel::Query ()", 1
    )[0]
    assert "commands = outcome.commands;" in rescan
    assert "commands.push_back" not in rescan


def test_retired_menu_resource_is_not_registered_or_reused():
    resources = (EVP_ROOT / "Sources" / "AddOnResources" / "RINT" / "AddOn.grc").read_text(encoding="utf-8")
    ids = (ADDON / "ResourceIds.hpp").read_text(encoding="utf-8")
    assert "32508" not in resources
    assert "32508" not in ids


def test_reserved_status_row_is_hidden_without_changing_positional_ids():
    source = (ADDON / "Palette" / "ControlPalette.cpp").read_text(encoding="utf-8")
    ids = (ADDON / "ResourceIds.hpp").read_text(encoding="utf-8")
    assert "statusText.Hide ();" in source
    assert "PaletteStatusTextId = 3;" in ids
    assert "PaletteCommandsLabelId = 4;" in ids


def test_hosted_workflows_bypass_the_python_worker_and_server_gate():
    source = (ADDON / "Palette" / "ControlPaletteRun.cpp").read_text(encoding="utf-8")
    assert "if (!IsWorkflowCommand (info) && !serverBand.IsRunning ())" in source
    run = source.split("void ControlPalette::RunSelected (", 1)[1]
    assert run.index("SolveWorkflowNow ()") < run.index("EnsureInitialized (error)")
    assert run.index("EnsureInitialized (error)") < run.index("BeginRun (info->timeoutSeconds)")


def test_release_stages_only_supported_optional_workers():
    source = (CORE_ROOT / "tools" / "build" / "Stage-Release.ps1").read_text(encoding="utf-8")
    optional = re.search(r"\$optionalDirectories\s*=\s*@\(([^)]*)\)", source)
    assert optional is not None
    assert re.findall(r'"([^"]+)"', optional.group(1)) == ["GhWorker", "Gh2Worker"]
    assert "Remove-Item -LiteralPath $stage -Recurse -Force" in source
