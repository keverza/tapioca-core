# Tapioca UI Playground (Windows)

Standalone, DevKit-free Win32 + Diligent D3D11 host for the **real** Stats,
Define Programme and TapiocaUI gallery. No Archicad installation, process or API
connection is used. ImRAD is deliberately not integrated yet.

From the core checkout:

```powershell
powershell -File tools/build/provision-reference.ps1 -FromUpstream
powershell -File AddOn/EvP/tools/UIPlayground/Run-Playground.ps1
```

Requires Visual Studio C++ x64 tools, ATL and the Windows SDK. The script locates
CMake (PATH or Visual Studio), configures its own project, builds, runs smoke tests,
and launches the host. `-BuildOnly` skips launching; `-Fresh` discards the saved
workspace for this launch. Default config is `RelWithDebInfo`. No add-on build runs.

## Use

- Left: search panels/variants or open the existing component gallery.
- Centre: real production controls in a draggable-size preview child.
- Right: light/dark, four densities, window-DPI override, production font steps,
  compact spacing, mock recompute, events, live fixture JSON, Demo/Style Editor.
- Define Programme sliders/add/remove/default/save use production helpers.
  Double-click cells/colours or use Edit table to open playground-only ImGui
  read-back dialogs. Nothing is written to a project.
- Save a watched C++ file: the host debounces changes and builds only the UI DLL.
  Rendering continues while the build runs. Build errors appear in the Inspector.
  “Build UI DLL now” also requests a build.
- Host/shared-ImGui/ABI edits require restarting and rerunning the entry script;
  only the UI module is reloadable. Do not run the full entry script's build while
  an instance holds the shared ImGui DLL open.
- Fixture JSON files are read on startup. Use `-Fresh` after editing them; ordinary
  launches restore edited fixtures from the saved workspace.

`build/bin/<Config>/workspace.json` stores edits/selection/theme/density and the
bounded event log on clean shutdown. Fonts, ImGui context, GPU objects and input
belong to the host. No files are written into Archicad or Program Files.

## Architecture and ABI

- `TapiocaImGui.dll`: one shared Dear ImGui (Diligent's pinned 1.92.1), including
  Win32 backend and Demo. `imgui` is defined before DiligentTools; imports/exports
  and user config match in host, renderer and UI module. Do not link static ImGui.
- `TapiocaUIPlayground.exe`: Win32 input/window, bundled production Noto Sans,
  Diligent device/swapchain/renderer, authoritative UTF-8 JSON snapshot,
  source watcher, asynchronous build runner and transactional DLL loader.
- `TapiocaUIPlaygroundUI.dll`: production TUs, workspace and transient typed
  fixtures. No GPU/font ownership, threads or native host calls.
- `UiAbi.h`: versioned tool-only extern-C POD ABI, cdecl. No STL crosses the
  exports. Create copies host callbacks and reconstructs from JSON. Draw borrows
  frame data only for the call; changed JSON is allocated with the host allocator
  and freed by the host. Destroy runs inside the creating DLL. Exceptions never
  cross the ABI. Shared ImGui remains a pinned toolchain/config dependency.

After the previous frame's RenderDrawData/Present, successful builds are copied
to `modules/<pid>/TapiocaUIPlaygroundUI-<attempt>.dll`. The candidate must load,
export every entry point, match ABI and successfully Create from the latest state
**before** the previous module is destroyed/unloaded. Failed candidates leave it
intact. Keep five copies per process; prune only after successful replacement.

The watcher polls source timestamps every 250 ms, debounces 400 ms, allows one
build, and coalesces edits during a build into one follow-up. It watches local
C++/CMake/layout sources, TapiocaUI, HudShell and selected panel/domain sources,
metadata/JSON dependencies. Build output is `build/playground-build.log`.
Closing while a build runs waits for that owned build to finish, rather than
abandoning a child build process. Another host instance should use a separate
build directory; concurrent writers to the same CMake output are unsupported.

## Verification

The entry script runs CTest. The headless smoke executable uses the actual shared
ImGui and reloadable DLL, renders every fixture and gallery at both themes/four
densities, checks round trips, failed candidate rollback and ten consecutive swaps.

```powershell
# Windowed device/input/render/shutdown path (auto-closes)
& AddOn/EvP/tools/UIPlayground/build/bin/RelWithDebInfo/TapiocaUIPlayground.exe `
  --fresh --smoke-frames 30 --smoke-reloads 10
```

For interactive acceptance: type in the browser, drag Programme shares, add/remove
types, open/read back the table and colour picker, edit coefficients, resize/move
between DPI monitors, then edit a panel TU. Confirm the generation increases,
selection/drafts/events remain, and an intentional compiler error keeps the old
controls responding. Restore the source, save again, and confirm recovery.

Full theme-file/token work, ImRAD and extra panels are not delivered by this slice.
