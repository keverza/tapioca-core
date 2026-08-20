# EvP Web UI Plan — HTML/JS command UI, and later a browser palette + three.js viewer

Status: rev 7 (2026-07-30). **Phase 0 BUILT and user-ran**; **Phase 1 BUILT and
user-ran**; **Phase 2 BUILT and user-ran** — the viewer renders with HDRI IBL,
PBR textures, tessellation, and controls, but shading (drop shadows, shadow
stability) needs further work (see §3 Phase 2 Known issues).
**Phase 2b (Showcase viewer + probes) BUILT** — MassingDataset showcase viewer is the
strongest candidate (HDRI + PBR + scene centring + 10 shapes, only 3 small issues remain
per HANDOFF); three-gpu-pathtracer works as a useful probe; threepipe has broken lighting/
shadows with a HANDOFF for fixes.
**Phase 3 unchanged.**
**Phase 4 — Magnum native render test BUILT (offline compile)** — compiles but
outputs black window/PPM; needs validation with proper geometry + shaders (see §3 Phase 4).

Decided with the user: the **native palette stays the standard command UI** (params, results
table, pickers — E12 continues to own that). This plan adds the browser layer for what DG
cannot do, in four phases, cheapest first.

## 0. What is verified vs open (do not blur this line)

**Verified in the vendored Tapir source** (`AddOn/reference/tapir-archicad-automation-main/
archicad-addon/Sources/ScriptUIPalette.{cpp,hpp}`, `ScriptUICommands.{cpp,hpp}` — currently
vendored at 1.5.6):

- `Tapir.ShowScriptUI {htmlContent, width, height, title, resizable, zoomEnabled, zoomLevel,
  scrollBarsVisible, contextMenuEnabled, navigationDisabled, allowSelfSignedCertificates,
  clearCookies, autoHeight}` — loads a **full HTML document** into a `DG::Browser` (CEF) in a
  native modeless palette. One palette instance; a second call replaces the content.
- Page → Archicad: `window.ACAPI.SubmitResult(string)` stores ONE pending string;
  `window.ACAPI.ClosePalette()` hides. Archicad → page: nothing after load (no push) —
  the script side **polls** `Tapir.GetScriptUIResult` → `{hasResult, result}` (consume-once).
- `autoHeight` injects a debounced ResizeObserver that calls `ACAPI.ReportContentHeight` —
  the palette grows/shrinks with content; no JS authoring needed.
- The palette registers its JS object via `RegisterAsynchJSObject`; per Graphisoft's browser
  docs, such callbacks run on the **main thread** (safe for ACAPI on the C++ side).
- `ShowScriptUI` resets `hasPendingResult` on every call — a stale submit from a previous
  page is dropped, but a nonce stamped in `SubmitResult` and re-checked by the command is
  still the safer pattern (the plan's pressure-test caveat).

**Verified in the AC29 devkit / EvP source:**

- `DG::Browser` exists (`Support/Modules/DGLib/DGBrowser.hpp`) — the same control Tapir uses;
  EvP can host its own (Phase 3).
- EvP's data plane (`Sources/AddOn/Server/HttpServer.cpp`) already serves `GET /meshes`,
  `/mesh`, `/render`, `/screenshot/current|top`, `/stories`, `/slice`, … and **`POST
  /evp/call`** — the whole command surface over localhost HTTP.

**Was open; answered by the Phase 0 probe (2026-07-29, in Archicad):**

1. ~~Can the page `fetch()` the data plane?~~ — **No.** Page origin is `about:blank`/null; both
   `http://127.0.0.1:19191/health` and `https://example.com/` fail CORS in the embedded CEF.
   Bake everything in. Phase 3's same-origin `/ui` route is the only path to page-initiated
   fetches.
2. ~~Does the embedded browser load remote `https://` resources?~~ — **No** for `fetch()`
   (same CORS answer). Not tested with a `<script src=…>` because the showcase bakes
   everything; the plan pressure-test rule "bake everything in" stands.
3. ~~Polling cost / cadence.~~ — **300 ms is sane.** Steady-state ~50–75 ms per
   `GetScriptUIResult`; the first poll after `ShowScriptUI` spikes — **~4.7 s when CEF
   is cold** (first `ShowScriptUI` in an Archicad session), **~1.0 s when CEF is warm**
   (same session, after one prior `ShowScriptUI`). Polling keeps Archicad responsive.
   The Phase 1 helper returns per-poll timings so spikes are visible, not silent.
4. ~~Palette lifetime vs command lifetime.~~ — A submit sent after the command returned sits
   pending until the next `GetScriptUIResult`. Not exercised in Phase 0 (the showcase
   always polls while alive) but the C++-side `hasPendingResult` reset on every
   `ShowScriptUI` covers the common case.

## 1. Rules that bind this plan

- **"Never build UI in Python" is satisfied, not bent:** the HTML is *content* handed to a
  C++-rendered palette (Tapir's, later EvP's) — same shape as `labels={}` crossing the bus.
  No tkinter, no Qt, ever.
- **The native palette is the default.** A command whose UI is inputs + a results table uses
  the existing palette. The web layer is for: rich formatted reports, docs/READMEs, images
  inline with text, multi-step forms with preview, and (later) 3D views.
- **Data plane stays localhost-only.** "Share" means exporting a self-contained HTML file
  (data + images baked in as data URIs) via `evp.paths.output_path(...)` — openable and
  sendable anywhere without EvP. Never expose the HTTP server beyond 127.0.0.1.
- **Submit is a string.** Convention: pages submit `JSON.stringify({...})`; the Python side
  `json.loads`es and validates. One pending result at a time — design forms to submit once.

## 2. The two-way loop (the pattern every phase reuses)

```
command gathers data (evp.api / data plane / massingcalc)
  → renders HTML with the data BAKED IN (a <script>const DATA = {...}</script> tag)
  → Tapir.ShowScriptUI (autoHeight for forms; fixed size for viewers)
  → user previews / edits in the palette
  → page: ACAPI.SubmitResult(JSON.stringify(edits))
  → command polls Tapir.GetScriptUIResult (evp.runtime.check_cancel-aware loop, ~300 ms)
  → command applies edits: writes in ONE evp.transaction / renders PDF / places worksheet
  → optionally re-ShowScriptUI a confirmation view
```

Receiving from Archicad is the command's job (Python side), not the page's — that sidesteps
the CORS unknown entirely for Phases 0–2: the page never needs the network.

## 3. Phases

### Phase 0 — `WebUIShowcase` ✅ BUILT (probe: 2026-07-29, in Archicad)

`Commands/WebUIShowcase/` — `command.py` (the @evp.command), `page.py` (HTML/CSS/JS
template + ~40-line markdown converter), `test_page.py` (31 offline tests). One
`category="Samples"` command, three parameters: `poll_ms` (50–5000, default 300),
`show_remote_probe` (bool), `include_screenshot` (bool). Zero C++.

Five sections on one page, baked in by the command before `ShowScriptUI`:
**typography** (massing metrics table, Lithuanian strings), **form round-trip**
(text/number/select/checkbox + Submit), **image inline** (3D screenshot baked as
data URI via `urllib` from the data plane), **docs rendering** (the markdown
converter on a sample README), **the probe** (page-side `fetch()` against data
plane + a remote URL, both display the CORS outcome).

Two-way loop: `Tapir.ShowScriptUI` (autoHeight) → poll `Tapir.GetScriptUIResult`
(evp.runtime.check_cancel-aware, ~300 ms) → nonce-checked `SubmitResult` parse →
re-show confirmation → close. The page is also written to
`evp.paths.output_path("webui_showcase.html")` so the same artifact opens in a
desktop browser — that IS the share/export mechanism, demonstrated for free.

API surface (proved by the probe):
- `Tapir.ShowScriptUI {htmlContent, …, autoHeight}` — only `htmlContent` required.
- `Tapir.GetScriptUIResult {} → {hasResult, result?}` — consume-once.
- `window.ACAPI.SubmitResult(string)` / `ACAPI.ClosePalette()` / `ACAPI.ReportContentHeight(int)`.
- Archicad → page: nothing after load (no push) — script polls.

**Lessons (each one cost a real cycle — keep, do not blur):**
- ⚠️ **CORS**: page origin is `about:blank`/null; `fetch()` to **any** http(s) URL is
  blocked (data plane AND remote). Everything the page shows must be baked in by the
  command before `ShowScriptUI`. This is a feature — it keeps the share artifact
  self-contained — but it is the only path until Phase 3's same-origin `/ui`.
- ⚠️ **First poll after `ShowScriptUI` is ~4.7 s** (CEF warm-up); steady state ~75 ms
  per `GetScriptUIResult`. 300 ms cadence is fine, but surface the first-poll cost in
  the Phase 1 helper so it is not a surprise.
- ⚠️ **One ShowScriptUI palette** — title must name the current owner; a nonce stamped
  in `SubmitResult` and re-checked by the command is the safer pattern (the C++ side
  resets `hasPendingResult` on every call, but the belt-and-braces check catches a
  different class of mistake).
- ⚠️ **Import trap**: the runner uses `importlib.util.spec_from_file_location` (a
  MODULE spec, not a PACKAGE spec), so `from . import page` in `command.py` **fails**
  — use `import page` (the command's folder is appended to `sys.path`). Other
  commands follow this pattern (e.g. `MassingProbe` vendors its `massingcalc.py`
  into its own folder for the same reason).

### Phase 1 — `evp.webui` ✅ BUILT and user-ran (2026-07-29, in Archicad)

`evp/webui.py` (PyPackage) + `evp/__init__.py` (re-exports it) +
`AddOn/EvP/Sources/PyPackage/test_webui.py` (47 offline tests) +
`Commands/WebUIShowcase/{page,command}.py` (refactored to use the helper;
15 new showcase tests).

**Public surface (the "~5 lines" promise):**

| Helper | What it wraps |
|---|---|
| `webui.show(html, title=…, …)` | `Tapir.ShowScriptUI` with sensible defaults for the eight schema options |
| `webui.await_result(nonce, poll=0.3, timeout=None)` | Cancel-aware poll loop; returns `(parsed_dict, polls_count, timings_ms_list)`; drops stale nonces; raises `evp.Cancelled` on close |
| `webui.show_and_await(html, …)` | Show + await; one call |
| `webui.write_share(html, filename)` | Saves to `evp.paths.output_path(filename)` — the share/export mechanism |
| `webui.section(num, title, body, note=…)` | One numbered section |
| `webui.metrics_table(rows)` | `(label, value, unit)` triples → styled table |
| `webui.image(data_uri, alt, caption=…)` | Inline image (or placeholder if `data_uri is None`) |
| `webui.markdown(text)` | ~40-line converter (headings, bold, code, lists, blockquote, code fence) |
| `webui.note(text)` | Highlighted callout |
| `webui.html(raw)` | Raw-HTML passthrough |
| `webui.form_html(fields, submit_label, action)` | text / int / float / select / bool / textarea → `<form>`; auto-submits via `ACAPI.SubmitResult` |
| `webui.report(blocks, title, data=, extra_js=)` | Multi-section page; `data` merges into the page's data block; `extra_js` is a second `<script>` block for showcase-specific behaviour |
| `webui.page(body, title, data=, extra_js=)` | One-body page |
| `webui.confirm(submitted, summary, title, extra_js=)` | Post-submit confirmation with a Close button |

Commands then do:

```python
from evp import webui
html = webui.report([
    webui.section("1", "Inputs", webui.form_html([...fields...])),
], title="My Command")
webui.write_share(html, "my_command.html")
submitted = webui.show_and_await(html, title="My Command")
```

**Probe (2026-07-29, in Archicad, after `.\tools\build\Build-AddOn29.ps1`):**

```
data plane: http://127.0.0.1:19191
screenshot: no                                  (3D window not front)
nonce: 627ba519967e4789
html -> <repo> (10120 chars)
submit after 46 polls (avg 48.1 ms, max 1061.2 ms): action=form_submit
payload: {"nonce": "627ba519967e4789", "action": "form_submit", "name": "phase-zero", "count": 3, "scope": "all", "enabled": true}
palette closed (close button submitted)
```

Same protocol as Phase 0; the refactor only changed the source shape, not the
wire format. Compared with the earlier cold-CEF run (117 polls / 75.6 ms avg /
4 705 ms max), the warm-CEF run is materially faster — see the CEF-warm lesson
below.

**Lessons (each one cost a real cycle — keep, do not blur):**
- ⚠️ **CEF warm vs cold first-poll cost**: the first poll after `ShowScriptUI`
  is **~4.7 s on a cold CEF** (first `ShowScriptUI` in an Archicad session) and
  **~1.0 s on a warm CEF** (same session, after one prior `ShowScriptUI`).
  Steady-state per-poll cost is ~50 ms either way. The 4.7 s is CEF allocating
  the browser context, JIT-ing the JS, layout/paint — it does NOT happen on
  subsequent `ShowScriptUI` calls. Practical: a first-ever user action on a
  fresh Archicad session feels "slow" once; everything after is snappy.
  `await_result` returns per-poll timings so the spike is visible in the log,
  not silent. (Phase 0's 4.7 s spike was CEF cold; the post-build 1.0 s was
  CEF warm because the showcase had run earlier in the same session.)
- ⚠️ **Source ↔ build gap**: edits to `Sources/PyPackage/evp/*.py` are invisible
  to the running add-on until `.\tools\build\Build-AddOn29.ps1` runs (CMake's `EvPPyPackage`
  target copies the source into `build_29/PyPackage/`). `Sync-Commands.ps1`
  only syncs `Commands/`, NOT the evp package. The error surface is
  `ImportError: cannot import name 'X' from 'evp' (…\build_29\PyPackage\evp\__init__.py)`.
  Bake this check into any future refactor of the PyPackage: build first,
  then import. Source: AGENTS.md.
- ⚠️ **Test pattern for command-dispatched APIs**: the helper's `await_result`
  loop goes through `evp.api.call` for two commands per tick — `EvP.PollCancel`
  (cheap, returns `ok=False`) and `Tapir.GetScriptUIResult` (the work). A
  naive sequence-driven mock consumes one scripted response per transport
  call, so the cancel-poll call eats the first scripted response and the
  nonce test counts off by one. The fix (in `test_webui.py::_patched_call`):
  dispatch by command name; `PollCancel` returns `ok=False` unconditionally
  and only `GetScriptUIResult` consumes the response list.

### Phase 2 — `MassingDataset` ✅ BUILT and user-ran (2026-07-29, in Archicad)

`Commands/MassingDataset/` — `command.py` (the @evp.command), `datasetgen.py` (pure
serialisation helpers), `viewer.py` (HTML page generator), `massingcalc.py` (copy from
MassingFeasibility), `test_dataset.py` (11 offline tests), plus two asset files
(`grasslands_sunset_1k.exr` HDRI, `painted_plaster_wall_1k.zip` PBR textures).
`runtime="embedded"`, `needs_selection=True`. Zero C++.

**The command** (all four chunks in one):

1. Reads selected slabs natively → computes building metrics via `massingcalc`
2. Builds a geometry snapshot via `evp.geometry.snapshot()` → serialises vertices
   (float32), triangles (uint32), normals (float32) to flat JSON lists
3. Writes `massing_options.json` (the dataset contract) to output
4. Reads the HDRI (`.exr`) and extracts PBR textures (`.zip`) from the command
   folder, base64-encodes them
5. Generates the three.js viewer HTML with everything baked in — dataset JSON,
   HDRI, PBR textures, submit nonce — and writes `massing_viewer.html` to output
6. Shows the viewer in Tapir's palette (`ShowScriptUI`, 1024×700, fixed-size) and
   awaits submit-back via `webui.await_result` (5-min timeout, `evp.Cancelled`
   caught for graceful close)

**Dataset contract** (`massing_options.json`):

```json
{
  "version": 1,          "timestamp": "…",
  "projectName": "…",    "camera": {"eye":[…], "target":[…], "up":[0,0,1]},
  "storyLevels": […],    "floorHeight": 3.025,   "plotArea": 12000.0,
  "options": [{"name":"Main","buildings":[{guid,label,footprintArea,height,floors,gfa,meshIndex},…],
               "metrics":[(label,value,unit),…], "totals":{tot_foot,tot_gfa,…}}],
  "meshes": [{guid, elemType, vertexCount, triangleCount, positions:[…flat float32], triangles:[…flat uint32], normals:[…flat float32]}, …]
}
```

Per the plan, the whole selection is one option named `option_name` (default "Main").
GUID-set grouping (`main`/`context`/`optionA`/`optionB` from `historical source docs/renderer-plan.md` §7)
is future work — the dataset structure is forward-compatible.

**Viewer** — self-contained HTML page, shown in the palette AND written as a standalone
file. Three.js imported via CDN import map (`three@0.160`, `OrbitControls`, `EXRLoader`);
no vendored build (the CEF browser loaded the CDN modules successfully in the probe run).

Rendering features:
- ACESFilmicToneMapping + sRGB output colour space
- HDRI image-based lighting via `EXRLoader` → `PMREMGenerator` → `scene.environment`
- PBR textures: diffuse (`map`) + ARM (`roughnessMap`/`metalnessMap`/`aoMap`) on
  `MeshStandardMaterial`
- Shadow-mapped sun (`DirectionalLight`, `PCFSoftShadowMap`, 2048×2048 map, frustum
  tightened to scene AABB), shadow-receiving ground plane
- Coloured/white per-building materials (white with colour tint for identification;
  Archicad material inheritance is future work)

Interaction: OrbitControls (damping), click-mesh → select building → highlight metrics
card, visibility toggle per building, camera-preset reset + top-view button, story-level
reference planes, ground grid.

Submit-back (Phase 2d): "Place Selected" button submits `{nonce, action:"place_selected",
building:{guid, label}}` via `ACAPI.SubmitResult`. The command catches `evp.Cancelled` so
closing the palette is not an error.

**Probe** (2026-07-29, in Archicad, six runs):

```
Run 1 (cold CEF, no HDRI — the initial shape):
  selection: 5 slabs, snapshot: 5 meshes, 40 vertices, 60 triangles
  massing_options.json: 6.7 KB
  viewer HTML: 20.6 KB
  ShowScriptUI → palette open → close → "CANCELLED: stopped by the user"
  ✅ Bug 1: palette close wasn't caught → fixed (try/except evp.Cancelled)

Run 2 (_read_rings missing):
  ✅ Bug 2: elements.py's _read_rings helper had been dropped in a prior refactor
  → restored, user ran .\tools\build\Build-AddOn29.ps1

Run 3 (HDRI + PBR textures present, after build + sync):
  massing_options.json: 6.7 KB
  viewer HTML with HDRI + textures: ~10 MB (HDRI 8 MB base64 + textures 1.6 MB base64)
  three.js CDN modules loaded in the embedded CEF browser → ✅
  HDRI environment lighting applied → ✅
  PBR plaster material applied to buildings → ✅
  Shadows visible on ground plane → ✅
  Palette close → graceful finish, no error → ✅

Run 4 (viewport black — SubdivisionModifier missing):
  ✅ Bug 3: SubdivisionModifier was removed from three.js. Static `import` caused
  the entire module to fail → black viewport. Changed to dynamic `import()` with
  fallback; replaced with `TessellateModifier`.

Run 5 (HDRI rotated 90°, ground on vertical plane, dark shadow hotspots):
  ✅ Root cause: Archicad Z-up clashed with three.js Y-up. OrbitControls and
  HDRI both expect Y-up. Fix: swap Y↔Z at load time (swapYZ helper) — geometry,
  normals, camera preset, story levels all converted. Result: HDRI matches scene
  orientation, ground is horizontal, OrbitControls works natively.
  ✅ Dark hotspots: shadow bias was too low. Raised to bias=-0.0005, normalBias=0.08.

Run 6 (no drop shadows, sun "divides" objects, shadows intermittent):
  ✅ Shadow camera frustum was in world axes, not the light's view space — and
  didn't update when sun moved. Rewrote updateShadowCamera(): near/far bracketed
  around scene centre along the light direction, frustum covers scene diagonal.
  ❌ OPEN: Flat-shading shows intermittent shadows that disappear at some sun
  positions. On smooth-shading, no inter-object drop shadows. Some meshes don't
  cast shadows at all. Shading still needs work.
```

**Known issues (open — not resolved):**
- ❌ **Drop shadows unreliable.** On smooth-shaded mode, objects don't cast
  shadows onto each other. On flat-shaded mode, shadows are intermittent — they
  appear at some sun positions and disappear at others. Some individual meshes
  never cast shadows. Likely root causes: (a) the orthographic shadow camera
  frustum is approximated from scene AABB in world space, not properly projected
  into the light's view plane; (b) the shadow map resolution (2048×2048) may be
  insufficient for the full-scene frustum; (c) PCFSoftShadowMap interacts poorly
  with bias settings on low-poly geometry.
- ❌ **No inter-object occlusion.** Only self-shadowing is visible; building A
  does not cast a shadow onto building B. The shadow map renders each mesh in
  isolation from each light pass rather than as a unified scene depth-map.
  This is the highest-priority shading fix remaining.

**Lessons (each one cost a real cycle — keep, do not blur):**
- ⚠️ **`mc.compute()` enriches the building dicts** with `floors`/`gfa`/`net`/`market`;
  the assembly loop must iterate over the returned `rows`, not the original `buildings`
  (KeyError: `'floors'`).
- ⚠️ **Three.js CDN imports work in the CEF browser.** The embedded browser loaded
  `three.module.js`, `OrbitControls.js`, and `EXRLoader.js` from `unpkg.com` without
  issue — the CORS-block that stopped `fetch()` does **not** apply to `<script
  type="importmap">`/`<script type="module">`.
- ⚠️ **HDRI + textures balloon the HTML.** ~10 MB with the HDRI and PBR textures baked
  in. `ShowScriptUI` accepted this without issue (UniString has no small cap). Phase 3's
  URL-based loading would shrink the payload to metadata-only.
- ⚠️ **Z-up → Y-up conversion is mandatory for three.js.** Archicad's Z-up coordinates
  clash with three.js's native Y-up: OrbitControls, HDRI equirectangular mapping, and
  GridHelper all assume Y-up. Swap Y↔Z on every vertex/normal/camera-preset at load time
  (a `swapYZ` flatten-and-remap on the Float32Array). The dataset JSON stays in Archicad
  coordinates; the conversion is the viewer's responsibility.
- ⚠️ **`SubdivisionModifier` was removed from three.js.** Replaced with
  `TessellateModifier` (subdivides by max-edge-length rather than Catmull-Clark
  smoothing). Dynamic `import()` with `try`/`catch` is mandatory — a failed static import
  kills the entire module, producing a black viewport with zero diagnostics.
- ⚠️ **Shadow camera frustum must be in the light's view space**, not world axes.
  For `DirectionalLight`, `near`/`far` are distances along the light direction, and
  `left`/`right`/`top`/`bottom` define the ortho-box width/height perpendicular to it.
  Approximating the scene AABB diagonal as the frustum size works, but it does not
  *project* the AABB into the light's plane — so some sun angles clip objects or miss
  them entirely.
- ⚠️ **`evp.Cancelled` must be caught** in every `webui.await_result` call site.
  The exception propagates to the top-level runner and prints "CANCELLED: stopped
  by the user". Wrap in `try/except evp.Cancelled`.
- ⚠️ **The `_read_rings` regression.** Commit `6bfee63` dropped the `_read_rings`
  helper from `evp/elements.py` during a refactor of `details()` to nested records,
  but `slab_details()` still called it (`NameError`). The function was restored;
  PyPackage edits need `.\tools\build\Build-AddOn29.ps1` to deploy.
- ⚠️ **No UVs on snapshot geometry.** Snapshot meshes have positions + normals +
  triangles via `ModelerAPI::Element` fan-triangulation — but no UV coordinates.
  `datasetgen.serialize_mesh` generates planar XY UVs, scaled by the mesh span.
  This is correct for horizontal faces (slabs) but stretches on vertical faces;
  a triplanar shader would be needed for general Archicad geometry.

### Phase 2b — Showcase viewer + renderer probes ✅ BUILT (2026-07-30)

Three renderers were tested. The **MassingDataset showcase viewer is the strongest
candidate** — it has the most complete feature set (HDRI, PBR textures, scene centring,
10 showcase shapes, full shadow/material/camera controls) and only **3 small issues**
remain pending verification (see HANDOFF). The other two are useful probes; neither
is the main line.

#### 2b-i — MassingDataset Showcase Viewer ✅ STRONGEST CANDIDATE (only 3 issues remain)

`Commands/MassingDataset/viewer_webgi.py` + `command.py` + `HANDOFF.md`. Despite the
filename (historical — WebGi was attempted first), this is now a **pure THREE.js r160
showcase viewer**. Zero C++. `command.py` has a `use_webgi` toggle (default `True`)
that routes to this viewer; `use_webgi=False` routes to the original lightweight
`viewer.py`.

**What's built and confirmed working** (from HANDOFF status table):
- `WebGLRenderer` with ACES Filmic tonemapping + PCFSoftShadowMap → ✅
- HDRI environment (base64 EXR → `EXRLoader` → `PMREMGenerator` → `scene.environment`
  + `scene.background`) → ✅ BUILT
- Scene centring (AABB of all geometry, bottom-plane centre → origin in Y-up space,
  solves large-world-coordinates OrbitControls) → ✅ BUILT
- 10 showcase primitives in two rows (box, sphere, cylinder, torus, cone,
  icosahedron, dodecahedron, octahedron, torus knot, capsule) → ✅ renders correctly
- Shadow softness controls (light size, bias, map size) → ✅ BUILT
- Material controls (roughness, metalness, texture scale, wireframe edges, flat
  shading) → ✅ BUILT
- Camera presets (Reset, Top, Front) + OrbitControls (damping) → ✅ BUILT
- Debug log panel (same step/warn/err/info pattern as PathTracer viewer) → ✅
- CEF/webview supports WebGL2 → ✅ confirmed
- External CDN access (unpkg for THREE.js r160) → ✅ confirmed
- `AmbientLight` + `HemisphereLight` + `DirectionalLight` (sun) with altitude/azimuth/
  intensity controls → ✅ BUILT
- Ground plane + grid at Y=0 (post-centring) → ✅ BUILT
- Resize handling → ✅ BUILT
- HDRI readback (base64 EXR from `grasslands_sunset_1k.exr`, 6 MB on disk → ~8 MB
  base64 in HTML) → ✅ BUILT
- `dataset.meshes` → `BufferGeometry` with Y↔Z swap + scene-centre offset → ✅ BUILT
- Results table not used (viewer is the output) — correct by design

**Remaining issues** (only these 3, per HANDOFF §Known issues):
1. ⚠️ **PBR textures not visually confirmed** — `painted_plaster_wall_1k.zip` (8.4 MB)
   is base64-encoded to ~11 MB data URIs. `loadTextures()` runs before geometry
   creation, `makeMaterial()` wires `diffTex`/`armTex`/`normTex`/`dispTex` correctly.
   Possible causes: zip not found at runtime path, data URI too large for CEF, or
   `TextureLoader` with data URIs fails silently. Next step: add `onError`/`onLoad`
   callbacks to the loader + log texture JSON data existence.
2. ⚠️ **Archicad geometry not visually confirmed** — depends on selection having slabs
   at runtime. Debug log already shows `dataset.meshes` count; if 0 the issue is
   upstream in command.py, not the viewer.
3. ⚠️ **Drop shadows** — ground plane was at old scene-center Y, now fixed to Y=0
   post-centring (ground, grid, sun target, shadow frustum all origin-relative).
   FIXED in last edit, not yet re-tested.

**WebGi SDK integration attempt** (historical — not the current viewer): `ViewerApp` +
GBuffer/Progressive/Tonemap/SSAO/SSR/Bloom plugins, geometry added to
`viewer.scene.modelRoot`, scene diagnostics confirmed `modelRoot.children=1` +
`viewer.enabled=true` + `viewer.renderEnabled=true`, but canvas stayed black. Root
cause: `autoRender` property on `BaseRenderer` is undefined/not exposed. WebGi's
imperative API expects file loads via asset manager, not raw `Object3D` nodes. The
`<webgi-viewer>` custom-element + in-memory `.glb` blob URL path is the documented
approach but is ON HOLD since the bare THREE.js pipeline already delivers sufficient
quality. Docs at `AddOn/reference/webgi-docs/`.

**Definition of done** (from HANDOFF):
- [ ] PBR textures confirmed rendering on Archicad mesh (debug log shows "diffuse = loaded")
- [ ] Archicad geometry visible alongside showcase shapes
- [ ] Drop shadows visible on ground plane
- [ ] OrbitControls work correctly with large Archicad world coordinates
- [ ] `use_webgi=False` still works (three.js fallback viewer)
- [ ] `use_webgi=True` renders the showcase viewer
- [ ] AST scanner passes
- [ ] `pytest -q AddOn/EvP/Commands/MassingDataset/` — 11 tests pass
- [ ] `Sync-Commands.ps1` + Rescan in palette

#### 2b-ii — `PathTracer` (vanilla three-gpu-pathtracer) ✅ WORKS — useful probe, not the main line

`Commands/PathTracer/` — `command.py` + `viewer.py`. Uses three.js r180 + three-gpu-pathtracer
v0.0.24 + three-mesh-bvh. WebGL2 GPU progressive path tracing. Zero C++.

- **Setup**: Snapshots selected Archicad elements, serializes geometry into inline JSON,
  bakes into self-contained HTML.
- **Rendering**: `WebGLPathTracer` accumulates samples progressively; `setScene()` builds
  BVH. `ACESFilmicToneMapping` + sRGB. `filterGlossyFactor=0.5`.
- **Controls**: roughness / metalness / color per material (presets: matte, painted, metal,
  glass, concrete), sun altitude/azimuth/intensity, sky color, bounces (1–32), tile size
  (1–8), resolution scale (0.25–2.0), pause/reset. OrbitControls (damping), camera presets.
- **Debug**: Step-log panel shows every import/init step + WebGL extension probes.
- **Known caveats**: requires WebGL2 (probe logs extensions so failing CEF is identifiable);
  only solid-colour materials (no Archicad surface mappings); first few seconds are
  "compiling shaders" (black until first sample lands); resolution scale < 1.0 is blocky.

**Verdict**: GPU path tracer works in CEF — imports resolve, setScene merges, samples
accumulate, material + lighting controls update in real time. It does NOT need an
environment map (flat-colour sky works) and responds to explicit directional lights.
This is the **highest-fidelity single render** but the showcase viewer (2b-i) has a
broader feature set (HDRI, textures, shapes, centring) that makes it the stronger
overall candidate for the user's use case.

#### 2b-iii — `PathTracerThreePipe` (threepipe framework) ⚠️ BROKEN — lighting/shadows, HANDOFF exists

`Commands/PathTracerThreePipe/` — `command.py` + `viewer.py` + `HANDOFF.md`. Uses
threepipe v0.5.1 from CDN. Renders 7 hardcoded geometric primitives (sphere, box,
cylinder, torus, plane, circle, sphere) — no Archicad elements needed.

**What works**: `ThreeViewer` creates, `GeometryGeneratorPlugin` builds shapes correctly,
`BaseGroundPlugin` adds ground (toggle works), `AmbientLight2` + `HemisphereLight2`
provide flat illumination (shapes are not black), debug log + material presets +
camera controls are functional. 66 offline tests pass.

**What's broken**:
1. **Directional light** — only sphere reacts; root cause: threepipe `PhysicalMaterial`
   expects environment map for specular/diffuse, may not route standard directional lights.
2. **Shadows** — only sphere receives shadow, no inter-object shadowing.
3. **No environment map** — CEF blocks `fetch()` of `.hdr` from CDN (about:blank origin).
4. **Ground visibility after toggle** — `refreshTransform()` computes from scene bounds.

**HANDOFF** maps five fixes (priority): (1) procedural env-map via canvas `data:` URI,
(2) correct scene layer for shadow pass, (3) explicit `resetShadows()` + `setDirty`,
(4) shadow frustum from scene bounds, (5) fallback to Phong/Unlit materials.
Definition of done: all shapes lit by directional light, ≥2 cast shadows onto ground,
shadow quality controls visible, no CORS errors.

### Phase 4 — Magnum native render test (offline, C++, separate from WebUI)

**Status**: Compiled but outputs black window/PPM. This is a parallel investigation
from `historical source docs/renderer-plan.md` — real-time GPU preview, not photoreal, not print-res. Not
a WebUI phase; documented here because the user tested it alongside the WebUI viewer work.

**Entry points**:
- Source: `AddOn/reference/magnum-bootstrap-base/src/MyApplication.cpp` — smoke-test
  renders 3 boxes + a plane via `Shaders::PhongGL` + `Shaders::FlatGL3D` wireframe,
  readback → `magnum_smoke.ppm`
- Build script: `AddOn/EvP/Build-Magnum.ps1` — CMake configure + build with VS 2022,
  `Corrade + Magnum 2020.06 + SDL2` vendored in `reference/magnum-bootstrap-base/`
- Docs: `AddOn/reference/magnum-2020.06/` — full Magnum source tree with documentation
- SDL2 was pasted in manually at `magnum-bootstrap-base/SDL2-2.30.9/`

**Build**: `.\Build-Magnum.ps1` (Release) → `build\Release\MyApplication.exe`.
`Build-Magnum.ps1` supports `-Clean`, `-SkipConfigure`, and config argument
(default Release, also Debug).

**What compiles**: The full pipeline — `Corrade`, `Magnum` (with
`MAGNUM_WITH_SDL2APPLICATION=ON`), `SDL2-2.30.9`, and the smoke test — compiles
and links. The executable runs, creates an SDL2 window, calls `drawEvent()` once,
writes `magnum_smoke.ppm`, then exits. Structure: `Sdl2Application` main loop,
`PhongGL` shader (ambient + specular + shininess), `FlatGL3D` shader for wireframe,
polygon-offset fill pass, readback via `GL::defaultFramebuffer.read()` → PPM.

**What fails**: Output is a black picture. The render pipeline (Phong shader with
directional light, flat-colour wireframe pass, framebuffer readback) produces no
visible geometry. Root cause not yet diagnosed — could be:
- SDL2 OpenGL context creation parameters (no depth buffer, wrong pixel format)
- The shader configuration (PhongGL in a GL 3.3+ context, but the default
  SDL2Application may request a compatibility profile)
- The camera/projection setup (perspective at 42° fov, lookAt from (0,0,10) to
  (0,0,0) with up (0,1,0) — this looks along -Z, but boxes are at Z≈0 from
  origin, so they should be visible)
- Missing OpenGL context flags (the code sets clear color + depth test + polygon
  offset but does not explicitly request a core profile)

**What's needed to validate the Magnum path**:
1. **Realtime render of geometric primitives** (boxes, plane, sphere, cylinder)
   with a working OpenGL shader that produces visible output. The smoke-test
   geometry is there (`MakeBox`, `MakePlane`); the pipeline needs debugging.
2. **Concrete/clay shader assessment** — does Magnum 2020.06 ship a PBR
   (`MeshVisualizer`, `Phong`, `Flat`) or must a concrete/clay look be built from
   scratch? Magnum ships `Shaders::PhongGL` (used in smoke test),
   `Shaders::MeshVisualizerGL3D`, `Shaders::FlatGL3D` (used for wireframe).
   It does NOT ship a built-in PBR or concrete shader — a clay/concrete look
   would need custom GLSL or assembly from Magnum's material framework.
3. **If image quality passes** for the user with concrete/clay shaders, then add
   **horizontal slice sections** (clip planes in the shader, or Magnum's
   `SceneGraph::Drawable` with a clip-distance uniform).

**Relation to WebUI plan**: If Magnum delivers an interactive viewport (1–5 fps,
offscreen or SDL2 window), the rendered image could be streamed to the Phase 3
palette via the data plane (`/render` already exists) or `EvP.call` bridge.
Magnum is the GPU fallback from `historical source docs/renderer-plan.md` §2; the Phase 4 label here is
a coordination marker, not a fourth WebUI phase.

### Phase 3 — native EvP browser palette + `/ui` (the strategic end-state; C++)

Gated on real demand from Phases 0–2 (e.g. push updates or live geometry needed).

- **HttpServer**: `GET /ui/<file>` static route serving a whitelisted folder
  (`%LOCALAPPDATA%\EvP\ui\`, populated by sync) with a small MIME map. A page served from
  the data-plane origin fetches `/meshes`, `/render`, `POST /evp/call` **same-origin — the
  CORS question evaporates**.
- **Palette**: new `Palette/WebPanel` file pair (palette-seam rules apply: places itself via
  `PlaceAt`, never `Attach`es itself; `ControlPalette.cpp` stays the only observer), hosting
  `DG::Browser` pointed at `/ui/...`.
- **Bridge**: `RegisterAsynchJSObject` exposing ONE function — `EvP.call(jsonString) →
  jsonString` routed through `ApiDispatcher` (the same envelope as `POST /evp/call`), so JS
  gets the entire `EvP.*`/`API.*`/`Tapir.*` surface with zero per-command C++. Push:
  `browser.ExecuteJS` for events (selection changed, command finished).
- Chunks: (a) `/ui` route; (b) palette skeleton showing a static page; (c) `EvP.call`
  bridge + envelope tests; (d) push events. Each is one commit + one probe page.

## 4. Pressure-test caveats

- **One ShowScriptUI palette** — two commands using it fight over one surface; the helper
  must make the current owner explicit (title = command name) and treat a stale
  `GetScriptUIResult` from a previous page as garbage (stamp a nonce into the page, echo it
  in the submit JSON, drop mismatches).
- **Long-poll vs cancel** — the await loop must check `evp.runtime.check_cancel()` every
  tick and give the user a palette Stop that actually returns; never block on a submit
  that may never come without a timeout parameter.
- **Big HTML over the bus** — `htmlContent` crosses as one UniString; an inlined three.js
  (~700 KB) plus meshes may be multi-MB. Works in principle (UniString has no small cap). **Phase
  2c probe confirmed:** the 10 MB payload (HDRI base64 ≈ 8 MB + PBR textures ≈ 1.6 MB +
  meshes + template) passed through `ShowScriptUI` without issue. The three.js module itself
  loads from CDN (not inlined). The fallback — Phase 3's URL loading — would shrink the bus
  payload to metadata-only by serving the HDRI/textures/meshes over the same-origin data plane
  route.
- **Encoding** — the bus is UniString (fine), but the console is cp1252; never print the
  HTML; log lengths, not content.
- **No secrets in pages** — the exported HTML is a share artifact; bake in only what the
  user is meant to share.

## 5. When-to-use

| UI need | Use |
|---|---|
| Params, pickers, results table, progress | **Native palette** (E12) — always |
| Rich formatted report / preview before commit | Web (Phase 1: `webui.report` + `webui.metrics_table`) |
| Docs / README / log tail display (with copy-to-clipboard) | Web (Phase 1: `webui.report` + `webui.markdown`) — log viewer is the first consumer |
| Multi-step form / wizard (each step = one `ShowScriptUI`) | Web (Phase 1: `webui.form_html` + `webui.show_and_await`) |
| Inline image / inline SVG chart of model state | Web (Phase 1: `webui.image`; bake the image) |
| 3D massing-options viewer (HDRI IBL + PBR) | Web + three.js (Phase 2b-i; `MassingDataset` command — strongest candidate, 3 issues remain: PBR textures, geometry confirmation, drop shadows) |
| 3D viewer (lightweight, no textures) | Web + three.js (Phase 2; `MassingDataset` + `use_webgi=False` — original bare `viewer.py`) |
| Photoreal progressive GPU path tracing | Web (Phase 2b-ii; `PathTracer` command — three-gpu-pathtracer, WebGL2) |
| 3D viewer with framework batteries (plugins, material system) | ⚠️ Phase 2b-iii threepipe has broken lighting/shadows; HANDOFF with 5 fixes exists, not the main line |
| WebGi photoreal post-processing (Bloom, SSAO, SSR) | ❌ WebGi SDK pipeline blocked (imperative API expects file loads); `viewer_webgi.py` is now bare THREE.js |
| 3D viewer with live geometry / push updates | **Phase 3 palette only** (same-origin `/ui` + `EvP.call` bridge) |
| Real-time GPU preview (native C++, not WebUI) | **Phase 4 Magnum** (offline test — not yet rendering visible output; compiles, black window) |
