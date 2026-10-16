def test_connection_and_capabilities(live_client, live_runner):
    health = live_client.health
    assert health["archicadVersion"] == 29
    assert health["tapir"], "Tapir returned no version information"
    project = health["project"]
    assert "isUntitled" in project and "isTeamwork" in project, "Missing project identity fields"
    if live_runner.options.expected_project:
        live_client.verify_project(live_runner.options.expected_project)
    if live_runner.options.connect_only:
        return
    missing = [name for name, present in health["tapiocaCapabilities"].items() if not present]
    assert not missing, (
        f"Tapioca test interface not registered: {missing}. Build/install and restart the test instance."
    )
    ping = health["tapioca"]
    assert ping["protocolVersion"] == 1
    assert ping["ready"] is True
    assert ping["version"]
    assert ping["buildHash"] and ping["buildHash"] != "unknown", "Build provenance unavailable"
    assert "massing_feasibility" in ping["workflows"]
