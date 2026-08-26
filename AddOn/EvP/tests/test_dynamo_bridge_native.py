from pathlib import Path


EVP_ROOT = Path(__file__).resolve().parents[1]
ADDON_ROOT = EVP_ROOT / "Sources" / "AddOn"


def test_pipe_bridge_dispatches_external_envelope_without_transport_json():
    source = (ADDON_ROOT / "Dynamo" / "DynamoBridge.cpp").read_text(encoding="utf-8")

    assert 'DispatchApiCall' in source
    assert '"external"' in source
    assert "EncodeResponse (envelopeBytes)" in source
    assert '!= "Tapioca.GetSelection"' in source
    assert "FILE_FLAG_OVERLAPPED" in source


def test_dynamo_menu_starts_bridge_and_teardown_quiesces_gate_first():
    host = (ADDON_ROOT / "Dynamo" / "DynamoHost.cpp").read_text(encoding="utf-8")
    main = (ADDON_ROOT / "AddOnMain.cpp").read_text(encoding="utf-8")

    assert "bridge.Start (error)" in host
    assert "DynamoBridge::Get ().Stop ();" in host
    assert main.count("BeginShutdown ();\n            evp::dynamo::Release ();") == 1
    assert main.count("BeginShutdown ();\n    evp::dynamo::Release ();") == 1


def test_zero_touch_package_exposes_only_read_only_selection_node():
    selection = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "Selection.cs").read_text(
        encoding="utf-8"
    )
    package = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "pkg.json").read_text(
        encoding="utf-8"
    )

    assert 'Call("Tapioca.GetSelection", "{}")' in selection
    assert "Tapioca.Dynamo" in package
    assert "SetSelection" not in selection
