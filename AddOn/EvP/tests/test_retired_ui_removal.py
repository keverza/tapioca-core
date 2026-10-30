"""Keep retired palettes out of the add-on without removing shared services."""

from pathlib import Path

import pytest

_COMPONENT = Path(__file__).resolve().parents[1]
_SOURCES = _COMPONENT / "Sources"


@pytest.mark.parametrize(
    "relative",
    [
        "NotebookUI/index.html",
        "NotebookUI/package.json",
        "NotebookUI/package-lock.json",
        "NotebookUI/src/main.ts",
        "WebUI/index.html",
        "Sources/AddOn/Notebook/NotebookPalette.cpp",
        "Sources/AddOn/Notebook/NotebookPalette.hpp",
        "Sources/AddOn/Palette/WebUIPalette.cpp",
        "Sources/AddOn/Palette/WebUIPalette.hpp",
        "Sources/AddOnResources/RFIX.win/NotebookUI.grc.in",
        "Sources/AddOnResources/RFIX.win/WebUIPanel.grc.in",
    ],
)
def test_retired_ui_sources_are_removed(relative):
    assert not (_COMPONENT / relative).exists()


def test_build_keeps_only_the_supported_embedded_editor():
    cmake = (_COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")
    rc = (_SOURCES / "AddOnResources/RFIX.win/AddOnMain.rc2").read_text(encoding="utf-8")

    assert "NotebookUI" not in cmake
    assert "WebUI" not in cmake
    assert "NotebookUI" not in rc
    assert "WebUIPanel" not in rc
    assert 'TapiocaAddEmbeddedUI (GraphUI "${GraphUIFolder}" ${GraphUIFiles})' in cmake
    assert "add_dependencies (${ADDON_RES} EvPGraphUI)" in cmake
    assert '#include "GraphUI.grc.rc2"' in rc


def test_menu_resources_and_lifecycle_do_not_register_retired_palettes():
    main = (_SOURCES / "AddOn/AddOnMain.cpp").read_text(encoding="utf-8")
    ids = (_SOURCES / "AddOn/ResourceIds.hpp").read_text(encoding="utf-8")
    grc = (_SOURCES / "AddOnResources/RINT/AddOn.grc").read_text(encoding="utf-8")

    for text in (main, ids, grc):
        assert "Notebook" not in text
        assert "WebUI" not in text
    for resource_id in (32504, 32505, 32550, 32551, 32560, 32561):
        assert str(resource_id) not in ids
        assert str(resource_id) not in grc
    assert "GraphEditorPalette::RegisterPaletteControlCallBack ()" in main
    assert "ControlPalette::RegisterPaletteControlCallBack ()" in main


def test_http_server_keeps_data_plane_but_not_the_retired_panel_page():
    server = (_SOURCES / "AddOn/Server/HttpServer.cpp").read_text(encoding="utf-8")
    header = (_SOURCES / "AddOn/Server/HttpServer.hpp").read_text(encoding="utf-8")

    for text in (server, header):
        assert "SetWebUIPage" not in text
        assert "webUiPage" not in text
        assert "webUiMutex" not in text
    for route in ("/ui", "/ui/", "/ui/index.html"):
        assert f'Get ("{route}"' not in server
    for route in ("/health", "/evp/version", "/snapshot", "/meshes"):
        assert f'Get ("{route}"' in server
    assert 'Post ("/evp/call"' in server
    assert "HttpServer& SharedHttpServer ()" in header


def test_command_control_api_is_independent_of_the_retired_panel():
    python = _SOURCES / "AddOn/Python"
    control = (python / "CommandControlDispatch.cpp").read_text(encoding="utf-8")
    dispatcher = (python / "ApiDispatcher.cpp").read_text(encoding="utf-8")
    state = (python / "CommandRunState.cpp").read_text(encoding="utf-8")

    for verb in (
        "GetCommands",
        "RunCommand",
        "GetRunState",
        "CancelRun",
        "GetServerState",
        "StartServer",
        "StopServer",
    ):
        assert f'name == "{verb}"' in control
    assert "DispatchCommandControlVerb (backend, name, command, params, data, error, ok)" in dispatcher
    for call in ("SetStatus (message)", "SetResults (headers, rows)", "SetResultText (resultText)"):
        assert f"CommandRunState::Get ().{call}" in dispatcher
    assert "LaunchCommand (request" in control
    assert "RunCancel::Get ().EndRun (generation)" in state
    assert "state.generation != generation" in state
    assert (_SOURCES / "PyPackage/evp/webui.py").is_file()
