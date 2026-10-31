"""Opt-in windowed rebuild test; temporarily edits one production panel TU.

Do not run while editing HudFloorProgramme.cpp. Only this owned process is closed,
and the original bytes are restored only if the source still matches our last edit.
No Archicad process is used or controlled. Run after Run-Playground.ps1 -BuildOnly.
"""

import argparse
import ctypes
import json
import re
import statistics
import subprocess
import time
from ctypes import wintypes
from pathlib import Path


def wait_for(predicate, process, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Playground exited early: {process.returncode}")
        answer = predicate()
        if answer:
            return answer
        time.sleep(0.1)
    raise TimeoutError("Timed out waiting for playground verification evidence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--bin", type=Path, default=root / "build/bin/RelWithDebInfo")
    args = parser.parse_args()
    binary = args.bin.resolve()
    source = root.parents[1] / "Sources/AddOn/ArchViz/HudFloorProgramme.cpp"
    original = source.read_bytes()
    marker = b'    ImGui::TextWrapped ("%s", "Min/Max: net m2 inside walls. Share: % of flat count.");'
    if original.count(marker) != 1:
        raise RuntimeError("Production panel changed; review the structural-test insertion point")
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM), wintypes.LPARAM]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    process = subprocess.Popen([str(binary / "TapiocaUIPlayground.exe"), "--fresh", "--verification"], cwd=binary)
    last_written = original
    log_path = binary / "playground-session.log"

    def log():
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
            return text if text.startswith(f"Session PID: {process.pid}\n") else ""
        except OSError:
            return ""

    def window():
        found = []

        @callback_type
        def visit(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process.pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user32.EnumWindows(visit, 0)
        return found[0] if found else None

    def write_source(content):
        nonlocal last_written
        if source.read_bytes() != last_written:
            raise RuntimeError("Concurrent source edit detected; refusing to overwrite it")
        source.write_bytes(content)
        last_written = content

    def snapshot(generation):
        path = binary / "verification" / str(process.pid) / f"generation-{generation}.json"
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

    hwnd = None
    report = {}
    try:
        hwnd = wait_for(window, process)
        wait_for(lambda: "Frames rendered: 60" in log(), process)
        start_log = log()
        failure_text = "#error TAPIOCA_PLAYGROUND_EXPECTED_COMPILE_FAILURE"
        write_source(original + b"\n" + failure_text.encode() + b"\n")
        wait_for(lambda: "Build FAILED" in log()[len(start_log):], process)
        failed_log = log()
        before_frames = [int(n) for n in re.findall(r"Frames rendered: (\d+)", failed_log)]
        wait_for(
            lambda: max([int(n) for n in re.findall(r"Frames rendered: (\d+)", log())], default=0) > max(before_frames),
            process,
        )
        if "Active UI generation" in log():
            raise AssertionError("A failed compilation replaced the module")
        if "error C" not in failed_log or failure_text.split()[-1] not in failed_log:
            raise AssertionError("Compiler diagnostics not surfaced")
        baseline = None
        for iteration in range(10):
            content = original + f"\n// Watched reload verification {iteration}\n".encode()
            if iteration % 2 == 1:
                content = content.replace(
                    marker, marker + b'\n    ImGui::TextUnformatted ("Watched structural reload control");'
                )
            write_source(content)
            generation = iteration + 2
            wait_for(lambda generation=generation: f"Active UI generation {generation};" in log(), process)
            state = wait_for(lambda generation=generation: snapshot(generation), process)
            if baseline is None:
                baseline = state
            elif state != baseline:
                raise AssertionError("Authoritative state changed during structural reload")
            print(f"Verified generation {generation}", flush=True)
        write_source(original)
        wait_for(lambda: "Active UI generation 12;" in log(), process)
        text = log()
        latencies = [float(n) for n in re.findall(r"Watcher-to-reload ([0-9.]+) ms", text)][:10]
        swaps = [float(n) for n in re.findall(r"; swap ([0-9.]+) ms", text)][:10]
        builds = [float(n) for n in re.findall(r"Build succeeded in ([0-9.]+) ms", text)][:10]
        if len(latencies) != 10 or len(builds) != 10:
            raise AssertionError("Missing timing samples")
        report = {"reloads": 10, "compile_failure_retained_ui": True,
                  "structural_controls_added_and_removed": True, "state_preserved": True,
                  "watcher_to_reload_ms": latencies, "incremental_build_ms": builds,
                  "swap_ms": swaps, "median_watcher_ms": statistics.median(latencies),
                  "worst_watcher_ms": max(latencies), "median_incremental_ms": statistics.median(builds)}
    finally:
        if source.read_bytes() == last_written:
            source.write_bytes(original)
        else:
            print("Concurrent source edits preserved; original bytes were NOT restored")
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE, only the verified owned PID
        process.wait(timeout=180)
    if process.returncode != 0:
        raise RuntimeError(f"Playground shutdown failed: {process.returncode}")
    (binary / "watched-reload-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
