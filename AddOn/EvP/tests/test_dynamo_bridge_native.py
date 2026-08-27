import json
from pathlib import Path


EVP_ROOT = Path(__file__).resolve().parents[1]
ADDON_ROOT = EVP_ROOT / "Sources" / "AddOn"


def test_pipe_bridge_dispatches_external_envelope_without_transport_json():
    bridge = (ADDON_ROOT / "Dynamo" / "DynamoBridge.cpp").read_text(encoding="utf-8")
    protocol = (ADDON_ROOT / "Dynamo" / "ApiPipeProtocol.cpp").read_text(encoding="utf-8")

    assert 'DispatchApiCall' in bridge
    assert '"external"' in bridge
    assert "EncodeResponse (envelopeBytes)" in bridge
    for command in (
        "Tapioca.GetSelection",
        "Tapioca.GetModelElements",
        "Tapioca.GetBodyGeometry",
        "Tapioca.GetElementDetails",
        "Tapioca.SetElementDetails",
        "Tapir.MoveElements",
    ):
        assert f'"{command}"' in protocol
    assert "protocol::IsAllowedCommand" in bridge
    assert "GetNamedPipeClientProcessId" in bridge
    assert "FILE_FLAG_OVERLAPPED" in bridge
    assert "ReadExact (pipe, &acknowledgment, 1, stopping)" in bridge


def test_dynamo_menu_starts_bridge_and_teardown_quiesces_gate_first():
    host = (ADDON_ROOT / "Dynamo" / "DynamoHost.cpp").read_text(encoding="utf-8")
    main = (ADDON_ROOT / "AddOnMain.cpp").read_text(encoding="utf-8")

    assert "bridge.Start (error)" in host
    assert "DynamoBridge::Get ().Stop ();" in host
    assert main.count("BeginShutdown ();\n            evp::dynamo::Release ();") == 1
    assert main.count("BeginShutdown ();\n    evp::dynamo::Release ();") == 1


def test_zero_touch_package_exposes_selection_geometry_and_explicit_apply_nodes():
    selection = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "Selection.cs").read_text(
        encoding="utf-8"
    )
    package = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "pkg.json").read_text(
        encoding="utf-8"
    )
    geometry = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "Geometry.cs").read_text(
        encoding="utf-8"
    )
    elements = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "Elements.cs").read_text(
        encoding="utf-8"
    )
    project = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "Tapioca.Dynamo.csproj").read_text(
        encoding="utf-8"
    )
    customization = (
        EVP_ROOT / "Sources" / "TapiocaDynamo" / "Tapioca.Dynamo_DynamoCustomization.xml"
    ).read_text(encoding="utf-8")

    assert "[CanUpdatePeriodically(true)]" in selection
    assert "Current(bool refresh = false)" in selection
    assert 'CallData("Tapioca.GetSelection", new { })' in selection
    assert "Tapioca.Dynamo" in package
    assert '"version": "0.3.0"' in package
    assert '"Tapioca.GetBodyGeometry"' in geometry
    assert "ArchicadMesh" in geometry
    assert "CurrentSelectionBody(int index = 0, bool refresh = false)" in geometry
    assert "IGraphicItem" in (EVP_ROOT / "Sources" / "TapiocaDynamo" / "ArchicadMesh.cs").read_text(encoding="utf-8")
    assert "not a rigid translation" in geometry
    assert '"Tapioca.SetElementDetails"' in elements
    assert '"Tapir.MoveElements"' in elements
    assert "if (!apply)" in elements
    assert "ApplyStates.TryUpdate" in elements
    assert "[IsLacingDisabled]" in elements
    assert "Geometry.ReadBody" in elements
    assert "already at the staged target" in elements
    assert "ProtoGeometry.dll" not in project
    assert '<namespace name="Tapioca">' in customization
    assert "<category>Tapioca</category>" in customization


def test_round_trip_template_uses_scalar_meshes_and_connects_every_apply_input():
    graph = json.loads(
        (EVP_ROOT / "Sources" / "TapiocaDynamo" / "TapiocaRoundTripTest.dyn").read_text(
            encoding="utf-8"
        )
    )
    signatures = {node.get("FunctionSignature"): node for node in graph["Nodes"]}
    current = signatures["Tapioca.Geometry.CurrentSelectionBody@int,bool"]
    translate = signatures[
        "Tapioca.Geometry.Translate@Tapioca.ArchicadMesh,double,double,double"
    ]
    apply = signatures[
        "Tapioca.Elements.ApplyTranslation@Tapioca.ArchicadMesh,Tapioca.ArchicadMesh,string,bool"
    ]
    connected_inputs = {connector["End"] for connector in graph["Connectors"]}

    assert graph["View"]["Dynamo"]["RunType"] == "Manual"
    assert all(port["Id"] in connected_inputs for port in current["Inputs"])
    assert all(port["Id"] in connected_inputs for port in translate["Inputs"])
    assert all(port["Id"] in connected_inputs for port in apply["Inputs"])

    all_ports = {
        port["Id"]
        for node in graph["Nodes"]
        for port in node.get("Inputs", []) + node.get("Outputs", [])
    }
    assert all(connector["Start"] in all_ports for connector in graph["Connectors"])
    assert all(connector["End"] in all_ports for connector in graph["Connectors"])

    host = (ADDON_ROOT / "Dynamo" / "DynamoHost.cpp").read_text(encoding="utf-8")
    cmake = (EVP_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "TapiocaRoundTripTest.dyn" in host
    assert "TapiocaDynamoTemplate" in cmake


def test_zero_touch_client_acknowledges_the_complete_response_before_disconnect():
    bridge = (EVP_ROOT / "Sources" / "TapiocaDynamo" / "NamedPipeBridge.cs").read_text(
        encoding="utf-8"
    )

    assert bridge.index("pipe.ReadExactly(response);") < bridge.index(
        "pipe.WriteByte(ResponseAck);"
    )
