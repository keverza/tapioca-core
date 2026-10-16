"""Live tests are never activated by the repository's ordinary offline pytest run."""

import json

import pytest


@pytest.fixture(scope="session")
def live_runner(request):
    runner = getattr(request.config, "_tapioca_live_runner", None)
    if runner is None:
        pytest.skip("Opt-in only: python tools/live_test.py --suite smoke")
    return runner


@pytest.fixture(scope="session")
def live_client(live_runner):
    client = live_runner.client.connect()
    try:
        client.discover()
        print(json.dumps(client.health, indent=2))
        yield client
    finally:
        if client.health.get("tapiocaCapabilities", {}).get("GetDiagnostics"):
            try:
                client.health["diagnostics"] = client.addon("TapiocaTest", "GetDiagnostics")
            except Exception as exc:
                live_runner.errors.append(str(exc))
