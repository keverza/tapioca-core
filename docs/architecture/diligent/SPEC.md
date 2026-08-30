# Diligent Viewer Specification

Status: canonical specification for `internal work item`. The live task registry is
`private task registry`; delivered evidence is in
`AddOn/EvP/Sources/AddOn/ArchViz/private history ` and the history summary in this directory.

The former bgfx plan is archaeology only:
`historical source docs/bgfx-archviz-plan.md`. It records measurements and lessons that survived the
renderer change, but its bgfx API, phase order, and implementation instructions are obsolete.

The cross-project renderer decision and alternatives are recorded in
`private development decisions/ADR-003-diligent-renderer.md`; this specification defines the resulting
Diligent contract rather than reopening the engine comparison.

## Scope

`internal work item` owns the native GPU viewer, scene cache, progressive population, navigation
synchronization, overlays, and screenshots. It does not own BIM authority, persistent project
storage, or the semantics of analysis results.

The current renderer is Diligent Engine. It is built into the add-on with Diligent Core,
Diligent Tools, and Diligent FX. The viewport and overlays are built and running in Archicad;
bgfx is not a fallback renderer and no bgfx implementation work remains on the live path.

## Entry Points

The current diagnostics and probes are:

- `AddOn/EvP/Diagnostics/Commands/DiligentViewportSmoke/`
- `AddOn/EvP/Diagnostics/Commands/DiligentOverlayProbe/`
- `AddOn/EvP/Diagnostics/Commands/DiligentPlanOverlayProbe/`
- `AddOn/EvP/Diagnostics/Probes/DiligentShadowProbe/`
- `AddOn/EvP/Diagnostics/Probes/DiligentPickProbe/`
- `AddOn/EvP/Commands/DiligentOverlaySync/`

The native implementation is under `AddOn/EvP/Sources/AddOn/ArchViz/`. The CMake dependency
definition is `AddOn/EvP/cmake/Diligent.cmake`. Native command registration is in
`AddOn/EvP/Sources/AddOn/NativeCommands/ArchVizCommands.cpp`.

## Runtime Architecture

```text
Archicad main thread
  DG palette / overlay control
  ACAPI reads: model, camera, materials, sun, selection
  ChangeTracker and model-diff publishers
          |
          v
bounded extraction work through MainThreadGate
          |
          v
SceneCmdQueue: material table, element upserts, removals, camera/environment updates
          |
          v
Diligent viewport loop
  device, swap chain, scene cache, shadow map, pick buffer, HUD, plan-anchor layer
          |
          +--> opaque palette child HWND
          +--> premultiplied DirectComposition overlay
```

`DiligentViewportControl.cpp` owns the main-thread lifecycle and publishers. The Diligent
viewport owns the rendering loop, device, swap chain, and GPU scene. Extraction work is
bounded and reaches ACAPI only through `MainThreadGate`; gate lambdas capture by value. The
rendering side never calls ACAPI, touches DG, or waits on the gate.

The palette surface and overlay surface share scene and camera code but not presentation
policy. The palette uses an opaque Diligent HWND swap chain. The overlay uses a premultiplied
DirectComposition swap chain and is transparent where it draws nothing.

## Progressive Population And Cache Policy

Opening a large scene makes the viewport interactive before extraction is complete. The scheduler
provides the renderer with the first useful objects, the scene cache patches GPU resources in
batches, and remaining requested objects continue to arrive while navigation remains responsive.
The viewer never waits for full population before accepting orbit, pan, or zoom input.

Priority follows user relevance rather than element order: actively edited or selected objects come
first, then visible dirty objects, analysis-critical or nearby context, the remaining requested
scene, and stable background context. These are policy categories, not fixed public numeric values.
Early versus late project stage changes scheduling and cache lifetime only; it never changes element
identity, geometry meaning, property meaning, or analysis semantics.

Viewer caches are derived and disposable. Volatile proposal or temporary analysis data may be
replaced cheaply; stable session context may be retained and patched while useful; deliberate
artifacts such as screenshots record their source state but never become BIM authority.

## Scene Contract

- `BuildSnapshot` and the existing geometry extraction path remain the source of model
  geometry; ArchViz does not duplicate ACAPI tessellation.
- The scene cache owns one vertex/index buffer pair per element GUID and submits one draw per
  contiguous material range. It may retain compact derived GPU metadata beside that pair for
  render-only passes.
- Indices are 32-bit. Opaque ranges draw before transparent ranges.
- Transparent ranges do not write depth and are submitted after opaque geometry.
- The material table arrives before element upserts that refer to it.
- Dirty model changes are consumed as batches. A viewer does not attach per-element observers or
  write to the project just to watch it.
- Selection maps modeler sub-parts back through the selectable-owner chain before applying the
  Archicad selection bridge.
- Wireframe uses Modeler polygon boundaries rather than every extracted triangle edge. Internal
  convex-decomposition and fan-triangulation edges remain hidden; hardware tessellation may add
  display-only edges within flat or smooth source faces without moving the extracted surface or
  changing shaded, shadow, picking, G-buffer, bounds, or query geometry. Devices without hull,
  domain, and geometry shader support fall back to rasterizer wireframe.

The plan surface is an analysis layer, not a top-down copy of the 3D model. Wall outlines are
registration anchors. The anchor pipeline is separate from the lit scene pipeline, unlit, with
depth and culling disabled, and uses pixel-width ribbons so line weight stays constant through
zoom.

## Coordinate And Camera Contract

Archicad is Z-up, right-handed, and model coordinates are metres. ArchViz converts neither the
world handedness nor the vertical axis.

- `distance` is the 2D plan distance.
- `viewCone` is the horizontal field of view in degrees.
- Azimuth values exposed by the viewer are degrees unless a command explicitly documents a
  different wire representation.
- `CULL_CW` pairs only with `Handedness::Right`.
- The fitted plan camera is orthographic and top-down. Orthographic projection alone does not
  mean that the plan surface is active; projection and pose are separate camera properties.
- The panel viewport adopts Archicad's camera once when opened and then navigates independently.
  The overlay is the surface that follows Archicad navigation.

The camera matrix implementation is `ArchViz/Camera.cpp` and `ArchViz/MatrixMath.cpp`. The
camera-sync diagnostics and reporting tools are the contract for measuring visible trailing;
do not infer a pixel result from a poll interval alone.

When progressive loading is measured, record the request and consumer, element/triangle counts,
requested profile, extraction and processing time, GPU upload time, time to first visible object,
time to a usable viewport, completion time, and frame responsiveness while loading. A successful
link or a fast extraction is not evidence that the viewport stayed usable.

## Input And Synchronization

DG owns palette lifecycle, docking and layout only. The DG user item's native child HWND is
subclassed per window and supplies `WM_MOUSEMOVE`, left/right transitions, capture and wheel deltas
directly to the renderer; DG callbacks do not feed interactive input. Cursor position, modifier and
navigation-button state are independently polled in physical client pixels on the render thread.
Archicad camera reads use a main-thread timer because `MainThreadCommand` dispatch is not reliable
during a 3D-window drag.

The palette keeps delivering polled client coordinates while an ImGui button is captured beyond the
viewport edge. Its dedicated D3D11 device is limited to one queued frame. The ImGui graph
interaction lab compares native mouse-message coordinates and cadence, `GetCursorPos` screen and
client samples, the coordinates ImGui consumed, frame cadence, event/sample age and the configured
DXGI maximum frame latency. Buffered CSV telemetry is written only when logging is switched off or
the viewer closes, so file I/O cannot perturb the run. It includes node/wire drag, pan, zoom and
scroll labels plus viewport size, DPI, monitor, host-window styles and geometry, visibility and
minimize state. Those host fields are evidence for dock/float transitions rather than an inferred
dock boolean. The optional UI-isolation arm skips scene work only during an interaction, making
scene cost versus HWND/input cost an explicit A/B. The lab is an interaction test surface, not a
second graph runtime, and it does not intercept Archicad's swap chain.

Camera synchronization is a measured ladder, not a single magic interval. Sampling, prediction,
composition, frame queuing, and hide-on-navigation are independent concerns. A proposed mode
must be measured with the shared navigation log and must not be promoted from an eye-only result.

## Picking

Picking is an ID G-buffer, not a second cursor-aim camera. For each pick, IDs are rendered at
the viewport's own resolution with the displayed image's nominal, unjittered camera. A temporal
effect may jitter an individual geometry sample, but its accumulated output and the pick buffer
share the nominal pixel grid. Pixel `(x, y)` in the ID buffer therefore corresponds to pixel
`(x, y)` in the displayed image at every projection and DPI.
Only a small readback box is copied around the cursor. A click arriving while a readback is in
flight is retained, the box is clamped at frame edges, and `PickVote` resolves the cursor's
actual position inside the box.

The pick target is `RGBA8_UNORM`, never an sRGB format. The ID maps to the snapshot mesh and then
to the selectable owner before the selection bridge applies it.

## Environment And Sun

The 3D window shades with the view sun from 3D Projection Settings, not necessarily the project
place sun. `ExtractionEnvironment::ReadEnvironment` reads the view sun and retains the place sun
as a fallback and diagnostic value. Stored Archicad angles are authoritative; ArchViz does not
reimplement NOAA or silently replace the stored values with a recomputation.

The custom-mode `sunAzimuth` convention still requires verification at a project with a
different north value. DiligentFX is linked and reachable, but link reachability is not proof
that a DiligentFX cascade or post-effect is correct on the active device.

## Temporal Post-Processing

Temporal anti-aliasing consumes linear pre-tone-map HDR colour, current and previous depth,
genuine motion vectors, and current and previous camera attributes. Projection jitter is applied
to visible geometry and reported in every `PostFXContext` camera, but it is removed from the motion
vectors by deriving those vectors from separate unjittered current and previous matrices.

DiligentFX TAA output alpha is history weight, not viewport coverage. The HDR resolve therefore
uses accumulated RGB and reads alpha separately from the current unaccumulated HDR target. This is
required for the DirectComposition overlay to remain transparent where the current frame drew no
content. Resize, geometry replacement, projection changes, effect-mode changes, and skipped effect
frames invalidate temporal history. Ordinary camera publications retain history and rely on genuine
motion, depth rejection, and TAA's motion factor; resetting every camera poll would disable TAA on
the continuously synchronized overlay. Explicit one-shot camera adoption is marked as a
discontinuity and resets history.

## API And Command Boundary

Native commands own Archicad reads, writes, lifecycle verbs, and structured state. Python probes
compose those commands and write durable evidence through `evp.paths`. The viewer does not become
a second command runtime, and analysis consumers do not duplicate scene extraction or selection
semantics.

The current command names are generated from the native registry. The generic legacy `Viewer*`
names and the Diligent-specific `OpenDiligent*` names must be reconciled by the registered
ArchViz command task before a public compatibility surface is declared complete.

## Verification

Offline checks for changes to this area include the ArchViz C++ tests, the architecture and
palette seam checks, command scanning/dry runs for touched probes, and the native build. A live
renderer claim requires a real Archicad run and a recorded probe result; a successful compile or
link does not prove presentation, camera registration, or visual correctness.
