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
        "Tapioca.SetSelection",
        "Tapir.MoveElements",
    ):
        assert f'"{command}"' in protocol
    # The allowlist is a FIXED SIZE on purpose: a command added without widening the
    # array would be silently dropped, and one added without a reason is exactly what
    # the allowlist exists to stop. Both numbers move together or this fails.
    assert "std::array<std::string_view, 7> allowed" in protocol
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


def test_selection_set_node_model_stays_free_of_wpf_and_out_of_the_zero_touch_assembly():
    """The two Dynamo rules that force the three-assembly split.

    1. DynamoModel.LoadNodeLibrary stops importing an assembly's ZeroTouch nodes as
       soon as that assembly holds one NodeModel. A NodeModel in Tapioca.Dynamo would
       therefore delete every existing Tapioca node from the library, silently.
    2. The headless runner is net10.0 with no Windows Desktop framework, so the model
       must not reference WPF or it could not be loaded there at all.

    Neither failure shows up as a build error, so they are asserted on the source.
    """
    sources = EVP_ROOT / "Sources"
    zero_touch = sources / "TapiocaDynamo"
    model = (sources / "TapiocaDynamoNodes" / "SelectionSetNode.cs").read_text(encoding="utf-8")
    model_project = (sources / "TapiocaDynamoNodes" / "Tapioca.DynamoNodes.csproj").read_text(encoding="utf-8")
    view_project = (sources / "TapiocaDynamoNodesUI" / "Tapioca.DynamoNodesUI.csproj").read_text(encoding="utf-8")

    # Rule 1: no NodeModel anywhere in the ZeroTouch assembly.
    for source in zero_touch.glob("*.cs"):
        assert ": NodeModel" not in source.read_text(encoding="utf-8"), source.name

    # Rule 2: the model assembly is WPF-free, and only the view assembly is not. The
    # REFERENCE is asserted, not the word - both files explain the rule in prose.
    assert 'Include="DynamoCoreWpf"' not in model_project
    assert "<UseWPF>" not in model_project
    assert "<TargetFramework>net10.0</TargetFramework>" in model_project
    assert 'Include="DynamoCoreWpf"' in view_project
    assert "<UseWPF>true</UseWPF>" in view_project
    assert "<TargetFramework>net10.0-windows</TargetFramework>" in view_project

    # The set is stored state, not a live read - the whole reason it is a NodeModel.
    assert "BuildOutputAst" in model
    assert "Selection.Current" in model


def test_selection_set_node_offers_the_palette_s_five_actions_in_the_palette_s_order():
    """Update/Add/Remove/Reselect/Clear, matching SelectionSetPanel.hpp and the node
    graph editor's Get Selection. A user who learned one must not have to learn
    another, so the vocabulary is asserted rather than left to drift."""
    sources = EVP_ROOT / "Sources"
    model = (sources / "TapiocaDynamoNodes" / "SelectionSetNode.cs").read_text(encoding="utf-8")
    view = (sources / "TapiocaDynamoNodesUI" / "SelectionSetNodeView.cs").read_text(encoding="utf-8")

    actions = ["Update", "Add", "Remove", "Reselect", "Clear"]
    for action in actions:
        assert f"public string {action}()" in model, action

    # In order, and every one wired to the model rather than reimplemented in the view.
    # Matched on the call site rather than the bare label, so a mention in a comment
    # cannot pass or fail this.
    positions = [view.index(f'AddButton(buttons, "{action}"') for action in actions]
    assert positions == sorted(positions)
    for action in actions:
        assert f"n.{action}()" in view, action

    manifest = json.loads((sources / "TapiocaDynamo" / "pkg.json").read_text(encoding="utf-8"))
    assert manifest["node_libraries"] == [
        "Tapioca.Dynamo",
        "Tapioca.DynamoNodes",
        "Tapioca.DynamoNodesUI",
    ]

    # A declared node library that the installer never copies is a node that exists in
    # the manifest and nowhere on disk - which Dynamo reports, at most, in a log.
    host = (ADDON_ROOT / "Dynamo" / "DynamoHost.cpp").read_text(encoding="utf-8")
    for library in manifest["node_libraries"]:
        assert f'L"{library}.dll"' in host, library


def test_runner_resolves_the_geometry_library_before_building_the_model():
    """A missing ASM must be one sentence, not a LibG assembly error.

    Dynamo swallows a failed ASM search and leaves the preloader location empty, so the
    geometry factory path collapses to the runtime root and the first geometry node
    dies naming a file that was never meant to be there. The probe has to run BEFORE
    MakeCLIModel, because that is the only moment Dynamo accepts a path for it.
    """
    runner = EVP_ROOT / "Sources" / "DynamoRunner"
    program = (runner / "Program.cs").read_text(encoding="utf-8")
    probe = (runner / "GeometryLibrary.cs").read_text(encoding="utf-8")

    assert program.index("GeometryLibrary.Find()") < program.index("MakeCLIModel")
    assert "--GeometryPath" in program
    assert "geometryMessage" in program

    # The supported majors are read from the runtime's own libg_* folders, never
    # hardcoded: pinning a different Dynamo must not need a code change here.
    assert "libg_" in probe
    assert "GetInstalledAsmVersion2" in probe
    # An override is CHECKED, not trusted - an ASM of the wrong major cannot bind.
    assert "GetVersionFromPath" in probe
    assert "version.Major == found.Major" in probe

    host = (ADDON_ROOT / "Dynamo" / "DynamoHost.cpp").read_text(encoding="utf-8")
    assert 'response.Get ("geometryMessage", geometryMessage)' in host
    assert "TAPIOCA_DYNAMO_ASM_DIR" in host
