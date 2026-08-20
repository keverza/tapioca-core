# EvP API Reference

Every command reachable through `evp.api.call("Backend.Command", params)`.
Envelope: `{ok, data, error, meta}` — see Transport section at bottom.

---

## Known deviations from standard conventions

These commands break one or more of the implicit rules a coding agent would
assume. Read this before you write a call, not after it fails.

### Key naming (non-obvious or reserved-word collisions)

| Command | Key | Issue |
|---------|-----|-------|
| `AttachElementsToIssue` | `componentType` | NOT `type` — GS reserves `type` as the `ObjectState` type discriminator; a plain `type` field is silently swallowed. |
| `GetConnectedElements` | `connectedElementType` | Same `type` collision. |
| `PlaceLibraryObject` | `libraryPartNames` | **Plural**, not `libraryPartName`. A candidate list tried in order; the one that exists wins. |
| `GetLibraryPartInfo` | `libraryPartNames` | Same plural candidate-list pattern as above. |
| `FindPlacedObjects` | `libraryPartNames` | Same plural candidate-list pattern. |
| `CreateColumn` | `height2` | Second rectangular-section dimension. Not `height` — that key is the column's vertical extent and already carries a different meaning. |
| `DeleteDatabase` | `confirm` | **Required, not optional.** Must be `true` or the call is refused. Deletion is not undoable. |

### Behavior surprising for the command's apparent role

| Command | Expectation | Reality |
|---------|-------------|---------|
| `CreateText` | Inherits the Text tool's current formatting (angle, bold, just). | **Resets formatting to a deterministic baseline** (angle 0, plain face, left-justified, bottom-left anchor, no fixed-width box). Only the fields you explicitly pass are raised. Pass `inheritDefaults: true` on an item to get the old tool-state behaviour. |
| `SetElementSurface` | Three separate commands for paint/clear/restore. | **Three operations in one command**, with priority: `restoreMats` > `clear` > `surface`. Omitting all three is an error. |
| `GetAttributeInfo` with `kind:"profile"` | A read. | Creates a **temporary profiled wall**, reads its dimensions, then deletes it inside an undoable scope. Net project state: unchanged. Must NOT be called inside an open `evp.transaction` (IsWrite=false so the dispatcher does not open an undo scope). |
| `CreateDatabase` with `type:"layout"` | Creates a layout. | **Refused.** A layout needs a master layout, which `ACAPI_Database_NewDatabase` cannot supply. Use `API.CreateLayout` instead. |
| `SetDocumentFrom3DSettings` | A write that is undoable. | **Not undoable** (StructuralCommand). Ctrl+Z does nothing. Not allowed inside `evp.transaction`. |
| `CreateDatabase` | A write that is undoable. | **Not undoable** (StructuralCommand). Same restrictions as above. |
| `DeleteDatabase` | A write that is undoable. | **Not undoable** (StructuralCommand). Also demands `confirm:true`. |
| `CreateRoof` | `baseLine` is a contour edge. | Defaults to the **v0→v2 diagonal** (v0→v1 for a triangle). Every known-good hand-made roof uses an interior segment, never an edge — `baseLine` lets you override. |
| `CaptureScreenshot` | Works from any window. | **Requires the 3D window to be front.** |
| `GetBodyComponents` | A stateless read by index. | **Stateful.** Fetching the body is what makes its sub-components reachable; `DecomposePolygon` and `GetTextureCoordAtPoint` then act on THAT body. Out of order you get `APIERR_REFUSEDCMD`, or the previous body's data. |
| `GetElement3DInfo` | Reads what the 3D window shows. | **Converts the element to 3D on demand**, with no perspective cuts and no 3D cut planes. The element's own solid, not the screen. |
| `GetTexturePixels` | Returns the whole image. | **Capped at 65536 pixels** by default, truncated by whole ROWS. A 2048x2048 texture as JSON integers is a hung Archicad, not a slow response. |
| `GetConnectionTable` | Same as `GetCollisions`. | **Different question.** `GetCollisions` reports solids that OVERLAP (a clash); this reports elements that TOUCH, with the connecting polygon. A slab resting on a wall clashes with nothing. |
| `GetCutPolygons`, `GetBodyBuildingMaterials`, `GetConnectionTable` | Read the current sight. | **Create, select and delete a scratch sight** to regenerate the model with separate components. The previous sight is restored first. |

### Spelling preserved for compatibility

| Command | Field | Note |
|---------|-------|------|
| `GetCollisions` | `hasClearenceCollision` | ACAPI's own misspelling, deliberately kept so the payload matches Tapir's and the existing dumps. |
| `GetBodyComponents` | `elemIndexPlus1`, `bodyIndexPlus1` | Named for what they are: `API_BodyType.head.elemIndex`/`bodyIndex` are the internal index **plus one**. Subtract 1 before passing them as `elemIdx`/`bodyIdx`. |

### Transaction restrictions (StructuralCommand set)

These commands **cannot run inside `evp.transaction`** — Archicad refuses
non-undoable data-structure modifiers while an undo scope is open. Call them
directly with `evp.api.call`.

- `CreateDatabase` (except layouts, which are refused outright)
- `DeleteDatabase`
- `SetDocumentFrom3DSettings`

### Shape divergence: flat arrays vs nested records

| Command | Shape | Reason |
|---------|-------|--------|
| `GetSlabDetails` | **Flat parallel arrays.** | legacy — `MassingFeasibility` and `ShaftShell` ship on it. Needs updating|
| `GetElementDetails` | **Nested records** (one per guid). | New standard; positionally aligned to input. |
| All `EvP.Raycast*` / `EvP.Query` | **Flat parallel arrays.** | The shape numpy wants for thousands-of-rays bulk data. |

---

## EvP.* — Native Commands

### Snapshot & Geometry

```json
{
  "EvP.BuildSnapshot": {
    "required": {"scope": "\"all\" | \"selection\""},
    "optional": {"excludeTypes": "[int]", "meta": "bool | \"basic\" | \"full\""},
    "output": {"snapshotId": "int", "scope": "str", "elementCount": "int", "vertexCount": "int",
               "triangleCount": "int", "hasMetadata": "bool", "metaLevel": "\"none\"|\"basic\"|\"full\"|\"cancelled\"",
               "metadataCancelled": "bool", "droppedElements?": "int", "droppedTriangles?": "int"}
  },
  "EvP.ReleaseSnapshot": {
    "required": {},
    "optional": {},
    "output": {"freedBytes": "int"}
  },
  "EvP.GetSnapshotInfo": {
    "required": {},
    "optional": {},
    "output": {"snapshotId": "int", "scope": "str", "meshCount": "int",
               "guids": "[str]", "elemTypes": "[int]", "vertexCounts": "[int]", "triangleCounts": "[int]"}
  },
  "EvP.GetStatus": {
    "required": {},
    "optional": {},
    "output": {"serverRunning": "bool", "port": "int", "modelOpen": "bool", "snapshotId": "int"}
  }
}
```

### Geometry Queries (gate-free — no main-thread hop)

```json
{
  "EvP.Raycast": {
    "required": {"origin": "[x,y,z]", "direction": "[x,y,z]"},
    "optional": {"maxDist": "float"},
    "output": {"hit": "bool", "t?": "float", "guid?": "str", "elemType?": "int",
               "point?": "[x,y,z]", "normal?": "[x,y,z]"}
  },
  "EvP.SliceZ": {
    "required": {"z": "float"},
    "optional": {"types": "[int]", "guids": "[str]", "weld": "float", "nudge": "bool"},
    "output": {"zUsed": "float", "nudged": "bool", "loopCount": "int",
               "coords": "[x,y,z,...]", "loopPointCounts": "[int]", "loopClosed": "[bool]",
               "loopGuids": "[str]", "loopElemTypes": "[int]"}
  },
  "EvP.RaycastAll": {
    "required": {"origin": "[x,y,z]", "direction": "[x,y,z]"},
    "optional": {"maxDist": "float", "maxHits": "int"},
    "output": {"count": "int", "t": "[float]", "enter": "[bool]", "guids": "[str]",
               "elemTypes": "[int]", "points": "[x,y,z,...]", "normals": "[x,y,z,...]", "truncated": "bool"}
  },
  "EvP.RaycastAllBatch": {
    "required": {"origins": "[x,y,z,...]", "directions": "[x,y,z,...]"},
    "optional": {"maxDist": "float", "maxHits": "int"},
    "output": {"rayCount": "int", "hitCounts": "[int]", "t": "[float]", "enter": "[bool]",
               "guids": "[str]", "points": "[x,y,z,...]", "normals": "[x,y,z,...]", "truncated": "[bool]"}
  },
  "EvP.ClosestPoint": {
    "required": {"point": "[x,y,z]"},
    "optional": {"maxDist": "float"},
    "output": {"found": "bool", "dist?": "float", "guid?": "str", "point?": "[x,y,z]"}
  },
  "EvP.NearestElements": {
    "required": {"point": "[x,y,z]"},
    "optional": {"k": "int"},
    "output": {"guids": "[str]", "dists": "[float]", "count": "int"}
  },
  "EvP.Query": {
    "required": {"shape": "\"box\" | \"sphere\" | \"polygon\""},
    "optional": {"min": "[x,y,z]", "max": "[x,y,z]", "center": "[x,y,z]", "radius": "float",
                 "polygon": "[x0,y0,x1,y1,...]", "zmin": "float", "zmax": "float"},
    "output": {"guids": "[str]", "count": "int"}
  }
}
```

### Model Geometry — structured ModelerAPI reads (E24)

The BREP-level counterpart to the triangle snapshot. `EvP.BuildSnapshot` /
`evp.geometry` give welded triangle soup; these give the modeler's own topology —
polygons with their holes, edges with their adjacent faces, body flags, per-face
surfaces, NURBS. Python wrapper: `evp.model`.

⚠️ **1-BASED THROUGHOUT.** Element, body, vertex, edge and polygon indices all
start at 1, and so does the corner index within a polygon. `evp.geometry`'s numpy
views are 0-based; do not mix them.

⚠️ **`include` is opt-in per section.** An ABSENT `include` means the per-section
defaults; a PRESENT one is an exact list. `"all"` selects everything.

```json
{
  "EvP.GetModelInfo": {
    "required": {},
    "optional": {},
    "output": {"guid": "str (changes when the model regenerates)", "bounds": "{xMin..zMax}",
               "elementCount": "int", "colorCount": "int", "materialCount": "int",
               "textureCount": "int", "fillCount": "int", "lightCount": "int"}
  },
  "EvP.GetModelElements": {
    "required": {},
    "optional": {"guids": "[str]", "types": "[str] typeName e.g. \"wall\"", "skipEmpty": "bool",
                 "coordinateSystem": "\"world\" | \"local\"", "include": "[\"bounds\",\"transform\"]",
                 "offset": "int", "limit": "int"},
    "output": {"totalCount": "int (unpaged)", "count": "int", "offset": "int",
               "modelElementCount": "int", "coordinateSystem": "str",
               "elements": "[{index, guid, type, typeName, invalid, genId, tessellatedBodyCount, meshBodyCount, nurbsBodyCount, pointCloudCount, lightCount, bounds?, transform?}]"}
    // ⚠️ `bounds` is the ELEMENT's box (ModelerAPI GetBounds) and is NOT the extent of its
    // body. Confirmed live 2026-08-03: a wall reporting z 0.150..2.500 has a body whose 104
    // vertices span only 0.150..0.250 — a 10 cm perforated panel inside a 2.35 m claim.
    // Anything choosing a cut plane, a section height or a label position from `bounds` must
    // expect this; read the body's vertices (EvP.GetBodyGeometry) when the geometry matters.
  },
  "EvP.GetBodyGeometry": {
    "required": {"guid | elementIndex": "str | int"},
    "optional": {"body": "int (1-based, default 1)", "source": "\"tessellated\" | \"mesh\"",
                 "coordinateSystem": "\"world\" | \"local\"",
                 "include": "[\"vertices\",\"polygons\",\"edges\",\"convex\",\"normals\",\"vertexHardFlags\",\"all\"]",
                 "maxVertices": "int", "maxPolygons": "int", "maxEdges": "int"},
    "output": {"guid": "str", "elementIndex": "int", "elementType": "str", "source": "str",
               "bodyIndex": "int", "bodyCount": "int", "coordinateSystem": "str",
               "body": "{isWireBody, isSurfaceBody, isSolidBody, isClosed, hasSharpEdge, vertexCount, edgeCount, polygonCount, polygonVectorCount, bounds, hasColor, color?, colorIndex, materialIndex, textureIndex, hasTextureCoordinateSystem, textureCoordinateSystem?}",
               "vertices?": "[float] flat xyz", "verticesTruncated?": "bool",
               "vertexHardFlags?": "[bool]", "normals?": "[float] flat xyz (body vector pool)",
               "edges?": "{count, truncated, vertex1, vertex2, polygon1, polygon2, invisible, visibleIfContour, hasColor, colorIndex}",
               "polygons?": "{count, truncated, skipped, materialIndex, normalVectorIndex, polygonId, invisible, visibleIfContour, isComplex, isGravity, hasMaterialTexture, hasPolygonTexture, materialTextureIndex, polygonTextureIndex, edgeCounts, vertexIndices, edgeIndices}",
               "convex?": "{count, polygonIndex, vertexCounts, vertexIndices, normals}"}
  }
}
```

**The hole convention, which is the one thing to get right here.** Within a
polygon, corner `k` has a vertex index and a SIGNED edge index. A NEGATIVE edge
index means the edge runs the other way; a **ZERO edge index is a contour break** —
everything after it belongs to a hole, not the outer boundary. Read the vertex list
alone and a window opening becomes part of the wall face's outline. Use
`evp.model.polygon_loops(body, i)`, which splits on the markers and returns
`[outer, hole1, ...]`.

### Model Appearance — surfaces, colours, textures, lights, UVs (E24)

⚠️ **MODEL indices are not ATTRIBUTE indices.** These pools contain only what the
3D model uses, renumbered. `EvP.GetAttributeInfo` and the surface pickers speak
Archicad attribute indices; the two do not interchange.

⚠️ On `GetModelMaterials`, **`type` is authoritative and `typeName` is best-effort.**
Values above the published 0..7 enum exist in the wild: confirmed live 2026-08-03, four
surfaces in a stock project (`Paint - Light Gray`, `Concrete - 04`, `Stucco - Beige Rough`,
`Paint - Golden Ochre`) report **`type` 20**, which is in neither
`ModelerAPI::Material::Type` nor `API_MaterTypeID` — both stop at 7. Almost certainly a
CineRender/physical material with no public constant. Such values come back as
`typeName: "unmapped"`, which is deliberately distinct from a failure; switch on the raw
`type` int, never on the name.

```json
{
  "EvP.GetModelMaterials": {
    "required": {},
    "optional": {"indices": "[int] (default: the whole pool)"},
    "output": {"materialCount": "int", "count": "int",
               "materials": "[{modelIndex, type, typeName, name, surfaceColor, ambientReflection, diffuseReflection, specularReflection, specularColor, transparency, transparencyAttenuation, shining, emissionColor, emissionAttenuation, externalReference, hasTexture, textureName?, textureRotationAngle?, textureIndex?, fillIndex, fillColorIndex}]"}
  },
  "EvP.GetModelColors": {
    "required": {},
    "optional": {"indices": "[int]"},
    "output": {"colorCount": "int", "count": "int", "colors": "[{modelIndex, red, green, blue}]"}
  },
  "EvP.GetModelTextures": {
    "required": {},
    "optional": {"indices": "[int]", "usedOnly": "bool"},
    "output": {"textureCount": "int", "count": "int",
               "textures": "[{modelIndex, used, name, available, hasAlphaChannel, gdlStatus, transparentPattern, bumpMapPattern, diffusePattern, specularPattern, ambientPattern, surfacePattern, shiftedRandomly, mirroredInX, mirroredInY, xSize, ySize, pixelMapXSize, pixelMapYSize, pixelMapSize, pixelMapBufferSize, pixelType, pixelTypeName, checksum, fingerprint}]"}
  },
  "EvP.GetTexturePixels": {
    "required": {"index | name": "int | str"},
    "optional": {"x": "int", "y": "int", "width": "int", "height": "int", "maxPixels": "int (default 65536)"},
    "output": {"name": "str", "pixelMapXSize": "int", "pixelMapYSize": "int",
               "x": "int", "y": "int", "width": "int", "height": "int",
               "truncated": "bool", "pixels": "[int] flat ARGB bytes, row-major"}
  },
  "EvP.GetModelLights": {
    "required": {},
    "optional": {"guid | elementIndex": "str | int (element-scoped)", "specials": "bool (default true)",
                 "sweepElements": "bool (default true) — see the note below; false skips the recovery walk",
                 "coordinateSystem": "str", "include": "[\"parameters\"]"},
    "output": {"coordinateSystem": "str", "count": "int", "skippedCount": "int",
               "skipped?": "[{lightIndex, reason, stage:\"fetch\"|\"fields\", failedFields?, partial?}]",
               "sweptElements?": "bool", "recoveredFromElements?": "int",
               "lights": "[{scope, elementIndex?, guid?, lightIndex, viaElementSweep?, type, typeName, castsShadow, color, position, direction, upVector, radius, angleFalloff, falloffAngle1, falloffAngle2, distanceFalloff, minDistance, maxDistance, parameters?}]"}
  },
  // ⚠️ REAL LAMPS ARE NOT IN THE MODEL-SCOPE LIST. `ModelerAPI::Model::GetLight(i)` serves
  // ONLY the three synthetic lights — Light.hpp fixes AMBIENT/CAMERA/SUN at indices 1/2/3 —
  // and every index above that throws GS::IllegalArgumentException from the fetch itself,
  // even though `GetLightCount()` counts all of them (262 in the test project). Proven live
  // 2026-08-03; an element-scoped read returns the same lamps without a single failure.
  // So this command sweeps lamp ELEMENTS to recover them; those records carry
  // `viaElementSweep: true`. `skippedCount` is what could not be read either way — check it
  // rather than assuming `lights` is complete.
  "EvP.GetTextureCoordinates": {
    "required": {"guid | elementIndex": "str | int", "polygon": "int (1-based)",
                 "points": "[float] flat xyz, WORLD coordinates, on that polygon"},
    "optional": {"body": "int (default 1)", "source": "\"tessellated\" | \"mesh\""},
    "output": {"guid": "str", "elementIndex": "int", "source": "str", "bodyIndex": "int",
               "polygonIndex": "int", "count": "int", "u": "[float]", "v": "[float]"}
  }
}
```

`specials:true` (the default) adds the ambient, camera and **sun** lights.
`evp.model.sun()` is the shorthand — the sun's `direction` is the shadow vector,
and nothing else in EvP exposes it.

### NURBS & Point Clouds (E24)

Shells, morphs and revolved objects are NURBS bodies. The tessellated snapshot only
ever sees the polygon approximation, so "is this actually curved, and with what
radius" is unanswerable from it. Check `nurbsBodyCount` on `EvP.GetModelElements`
first.

⚠️ **The accessors are 1-based; the index VALUES they return are 0-based** into the
body's pools. That mismatch is the API's own and both sides are passed through
unchanged.

```json
{
  "EvP.GetNurbsBody": {
    "required": {"guid | elementIndex": "str | int"},
    "optional": {"body": "int (default 1)", "coordinateSystem": "str",
                 "include": "[\"vertices\",\"edges\",\"trims\",\"loops\",\"faces\",\"shells\",\"lumps\",\"curves2d\",\"curves3d\",\"surfaces\",\"all\"] (topology ON by default; the three geometry pools OFF)"},
    "output": {"guid": "str", "elementIndex": "int", "elementType": "str", "bodyIndex": "int",
               "bodyCount": "int", "coordinateSystem": "str",
               "body": "{vertexCount, edgeCount, trimCount, loopCount, faceCount, shellCount, lumpCount, curve2DCount, curve3DCount, surfaceCount, bounds, alwaysCastsShadow, neverCastsShadow, doesNotReceiveShadow, smoothness, edgePenIndex, edgePen, materialIndex, material, textureCoordSys}",
               "vertices?": "{count, coords, tolerances, hard}",
               "edges?": "{count, beginVertex, endVertex, curve3DIndex, tolerances, subdomainBegin, subdomainEnd, isLoopEdge, isRingEdge, isWire, isSurfaceBoundary, is2Manifold, trimCounts, trimIndices, visibility, smooth, color}",
               "trims?": "{count, edgeIndex, vertexIndex, loopIndex, curve2DIndex, tolerances, subdomainBegin, subdomainEnd, isSingular}",
               "loops?": "{count, faceIndex, trimCounts, trimIndices, trimReversed}",
               "faces?": "{count, shellIndex, surfaceIndex, tolerances, loopCounts, loopIndices, material, segmentationPen, textureCoordSys}",
               "shells?": "{count, lumpIndex, faceCounts, faceIndices, faceReversed}",
               "lumps?": "{count, shellCounts, shellIndices}",
               "curves3d?": "[{degree, rational, periodic, domainStart, domainEnd, controlPointCount, controlPoints, knots, weights}]",
               "curves2d?": "[same shape; controlPoints flat uv]",
               "surfaces?": "[{degreeU, degreeV, rational, periodicU, periodicV, controlPointUCount, controlPointVCount, controlPoints, knotsU, knotsV, weightUCount, weightVCount, weights}]"}
  },
  "EvP.GetPointClouds": {
    "required": {},
    "optional": {"guid | elementIndex": "str | int", "coordinateSystem": "str"},
    "output": {"coordinateSystem": "str", "count": "int",
               "pointClouds": "[{elementIndex, guid, cloudIndex, bounds, transform (16 floats, row-major)}]"}
  }
}
```

### C API 3D Component Database (E24)

The `ACAPI_ModelAccess_*` path. **Not redundant with the ModelerAPI reads above** —
`GetElement3DInfo` converts an element to 3D on demand (no 3D window needed, and no
perspective cuts or 3D cut planes in the result), which per the DevKit is the only
route to wall-hole information: look for polygons whose normal is horizontal and
perpendicular to the wall's reference line.

⚠️ **STATEFUL.** `GetBodyComponents` fetches the BODY first, which is what makes its
sub-components reachable; `DecomposePolygon` and `GetTextureCoordAtPoint` then act on
that body. Out of order you get `APIERR_REFUSEDCMD` — or the previous body's data.

⚠️ **MIXED INDEX BASES.** Sub-component indices (vertex/edge/polygon) are 1-based;
`elemIdx`/`bodyIdx` for `GetTextureCoordAtPoint` and `GetCutPolygons` are 0-based —
they are `elemIndexPlus1 - 1` / `bodyIndexPlus1 - 1` from the body record.

⚠️ **AND IT IS A DIFFERENT INDEX SPACE FROM THE MODELER'S.** `elemIndexPlus1` numbers the
ACTIVE 3D DATABASE, not `EvP.GetModelElements`' `index`. The two coincide often enough to
look interchangeable and are not: confirmed live 2026-08-03, a wall at modeler index 11 has
3D-database `elemIndexPlus1` 1, and passing `11 - 1` to a cutting call returns
`APIERR_BADINDEX`. Never derive `elemIdx` from a modeler index — take it from
`GetBodyComponents`/`GetElement3DInfo`. (`EvP.GetCutPolygons` does this for you when you
pass `guid`; the caveat matters when you pass `elemIdx` yourself.)

```json
{
  "EvP.Get3DComponentCounts": {
    "required": {},
    "optional": {},
    "output": {"bodyCount": "int", "lightCount": "int", "materialCount": "int"}
  },
  "EvP.GetElement3DInfo": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int",
               "elements": "[{guid, found, error?, firstBody, lastBody, bodyCount, firstLight, lastLight, lightCount, bounds}] (parallel to input)"}
  },
  "EvP.GetBodyComponents": {
    "required": {"body": "int (from GetElement3DInfo firstBody..lastBody)"},
    "optional": {"include": "[\"vertices\",\"vectors\",\"polygons\",\"polyEdges\",\"edges\",\"material\",\"all\"]"},
    "output": {"bodyIndex": "int",
               "body": "{index, elemIndexPlus1, bodyIndexPlus1, parentGuid, parentType, status, isClosed, isCurved, multiMaterial, multiColor, multiTexture, color, materialIndex, polygonCount, polyEdgeCount, edgeCount, vertexCount, vectorCount, bounds, transform}",
               "vertices?": "[float] flat xyz", "vectors?": "[float] flat xyz",
               "polygons?": "{count, materialIndex, normalIndex, firstPolyEdge, lastPolyEdge, status, invisible, curved, concave, hasHoles, holesConvex}",
               "polyEdges?": "{count, edge (SIGNED; 0 = contour break, a hole follows)}",
               "edges?": "{count, vertex1, vertex2, polygon1, polygon2, color, status, invisible, curved}",
               "materials?": "[{index, fromGDL, name, attributeIndex, materialType, ambientPc, diffusePc, specularPc, transparencyPc, shine, transparencyAttenuation, emissionAttenuation, surfaceRGB, specularRGB, emissionRGB, fillIndex, fillColor}] (PERCENTAGES, unlike GetModelMaterials' 0..1 doubles)"}
  },
  "EvP.DecomposePolygon": {
    "required": {"polygon": "int (1-based, within the ACTIVE body)"},
    "optional": {},
    "output": {"polygonIndex": "int", "subPolygonCount": "int", "declaredSubPolygonCount": "int",
               "vertexCounts": "[int]", "vertexIndices": "[int] flat, split by vertexCounts",
               "note?": "str — present ONLY when the API succeeded but returned no handle: subPolygonCount is 0 and this is NOT an error (a polygon that needs no decomposition). Confirmed live 2026-08-03."}
  },
  "EvP.GetTextureCoordAtPoint": {
    "required": {"elemIdx": "int (0-BASED)", "bodyIdx": "int (0-BASED)", "polygon": "int",
                 "points": "[float] flat xyz, LOCAL coordinates"},
    "optional": {},
    "output": {"count": "int", "u": "[float]", "v": "[float]"}
  }
}
```

The polygon → contour walk: a polygon holds `firstPolyEdge..lastPolyEdge`, a range
into `polyEdges.edge`; each polyEdge is a signed edge index (0 = contour break); each
edge holds its two vertices. `evp.model.body_polygon_loops()` does the reassembly.

### Sections, Building Materials, Connections (E24)

These three regenerate the model **split into separate components** on a scratch sight
(created, selected, generated, previous sight restored, scratch deleted). A composite
wall then reads as one body PER SKIN, which is what makes per-material areas possible —
and `GetCutPolygons` with `separateComponents:true` lines up body-for-body with
`GetBodyBuildingMaterials`.

```json
{
  "EvP.GetCutPolygons": {
    "required": {"plane": "{basePoint:{x,y,z}, normal:{x,y,z}} or {basePoint, axisX, axisY, axisZ}"},
    "optional": {"guid": "str (cuts every body)", "elemIdx": "int (0-based; then `body` is required)",
                 "body": "int (0-based)", "separateComponents": "bool (default false)"},
    "output": {"guid?": "str", "elemIdx": "int", "separateComponents": "bool", "plane": "{...}",
               "totalArea": "float", "bodyCount": "int",
               "bodies": "[{bodyIdx, ok, error?, area, polygonCount, polygons:[{contourCount, contourVertexCounts (first = outer, rest holes), uv, coords, area, perimeter}]}]"}
  },
  "EvP.GetBodyBuildingMaterials": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int",
               "elements": "[{guid, found, error?, elemIdx, modelIndex, bodyCount, bodies:[{bodyIdx, ok, attributeIndex, name}]}]"}
  },
  "EvP.GetConnectionTable": {
    "required": {},
    "optional": {"guids": "[str] (default: EVERY element with 3D geometry)"},
    "output": {"elementCount": "int", "pairCount": "int",
               "connections": "[{guid1, guid2, polygonCount, polygons:[{vertexCount, coords (flat xyz, world), plane}]}]"}
  }
}
```

### Element Data (reads)

```json
{
  "EvP.DumpRoof": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"reports": "[str]"}
  },
  "EvP.GetElementInfo": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int", "infoOfElements": "[{guid, found:bool, type:str, floorInd:int, angle:float}]"}
  },
  "EvP.GetSlabDetails": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int", "guids": "[str]", "found": "[bool]", "thickness": "[float]", "level": "[float]",
               "floorInds": "[int]", "outlineCoords": "[x,y,...]", "outlineArcs": "[float]",
               "outlineCounts": "[int]", "hasHoles": "[bool]", "holeCoords": "[x,y,...]",
               "holeArcs": "[float]", "holeCounts": "[int]", "holesPerElement": "[int]"}
  },
  "EvP.GetElementDetails": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int", "detailsOfElements": "[{guid, found:bool, kind:str, floorInd:int,
               elementId:str, details:{...}}]"},
    "kinds": {
      "slab": {"details": {"thickness": "float", "level": "float", "polygonOutline": "[{x,y}]",
                "polygonArcs": "[float]", "holes": "[{polygonOutline, polygonArcs}]", "hasHoles": "bool"}},
      "roof": {"details": {"thickness": "float", "level": "float", "slantAngle": "float",
                "polygonOutline": "[{x,y}]", "polygonArcs": "[float]", "holes": "[...]", "hasHoles": "bool",
                "roofClass": "\"plane\"|\"poly\"", "baseLine": "{begCoordinate, endCoordinate}",
                "posSign": "bool", "pivotOutline": "[{x,y}]", "pivotArcs": "[float]",
                "levels": "[{levelAngle, levelHeight}]", "levelNum": "int",
                "eavesOverHang": "float", "overHangType": "int"}},
      "mesh": {"details": {"level": "float", "skirtLevel": "float",
                "skirtType": "\"SolidBodyWithSkirt\"|\"WithSkirt\"|\"SurfaceOnlyWithoutSkirt\"",
                "ridges": "\"AllSharp\"|\"AllSmooth\"|\"UserDefined\"",
                "polygonOutline": "[{x,y}]", "polygonArcs": "[float]", "polygonZ": "[float]",
                "holes": "[{polygonOutline, polygonArcs, polygonZ}]", "hasHoles": "bool",
                "sublines": "[{coordinates:[{x,y,z}], vertexIds:[int]}]",
                "levelEnds": "[int]", "nSubLines": "int", "nLevelCoords": "int"}},
      "wall": {"details": {"thickness": "float", "height": "float", "level": "float",
                "slantAngle": "float", "begCoordinate": "{x,y,z}", "endCoordinate": "{x,y,z}"}},
      "beam": {"details": {"level": "float", "planAngle": "float", "slantAngle": "float",
                "isSlanted": "bool", "nSegments": "int", "sectionWidth": "float",
                "sectionHeight": "float", "begCoordinate": "{x,y,z}", "endCoordinate": "{x,y,z}"}},
      "column": {"details": {"height": "float", "level": "float", "planAngle": "float",
                "slantAngle": "float", "isSlanted": "bool", "nSegments": "int",
                "sectionWidth": "float", "sectionHeight": "float",
                "begCoordinate": "{x,y,z}", "endCoordinate": "{x,y,z}"}},
      "polyline": {"details": {"polygonOutline": "[{x,y}]", "polygonArcs": "[float]",
                "closed": "bool", "pen": "int"}},
      "object|lamp": {"details": {"level": "float", "planAngle": "float", "xRatio": "float",
                "yRatio": "float", "reflected": "bool", "libraryPartName": "str",
                "begCoordinate": "{x,y,z}", "endCoordinate": "{x,y,z}"}},
      "fill": {"details": {"pen": "int", "fillPen": "int", "fillBGPen": "int",
                "polygonOutline": "[{x,y}]", "polygonArcs": "[float]", "holes": "[...]", "hasHoles": "bool"}},
      "miss": {"found": false, "kind": "\"\"", "reason": "\"notFound\"|\"unsupportedType\"",
               "typeName": "str", "typeId": "int", "elementId": "str"}
    }
  },
  "EvP.GetLibraryPartInfo": {
    "required": {"libraryPartNames": "[str]"},
    "optional": {},
    "output": {"found": "bool (ok=false when none found)", "libraryPartName": "str",
               "libInd": "int", "sizeA": "float", "sizeB": "float", "paramCount": "int"}
  },
  "EvP.FindPlacedObjects": {
    "required": {"libraryPartNames": "[str]"},
    "optional": {},
    "output": {"guids": "[str]", "count": "int", "libraryPartName": "str", "note?": "str"}
  }
}
```

### Element Identity (ID field)

```json
{
  "EvP.GetElementIds": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int", "elementIds": "[{guid, found:bool, elementId:str, typeName:str, typeId:int, reason?}]"}
  },
  "EvP.SetElementIds": {
    "required": {"elementIds": "[{guid, elementId}]"},
    "optional": {},
    "output": {"count": "int", "changed": "int", "results": "[{guid, ok:bool, error?}]"}
  }
}
```

### Element Modify (the symmetric write for GetElementDetails)

```json
{
  "EvP.SetElementDetails": {
    "required": {"edits": "[{guid, details:{<field>: value, ...}}]"},
    "optional": {},
    "output": {"count": "int", "changed": "int",
               "results": "[{guid, ok:bool, kind:str, applied:[str], error?}]"}
  }
}
```

`details` is **SPARSE** — send only the fields you changed, never a whole
`GetElementDetails` record. A field this kind cannot write is refused by name and
nothing is written for that element. Writable scalars per `kind`:

| kind | fields |
|------|--------|
| `slab` | `level`, `thickness` |
| `roof` | `level`, `thickness`, `slantAngle` (**plane roofs only**; a poly roof is refused) |
| `mesh` | `level`, `skirtLevel` |
| `wall` | `level` (= `bottomOffset`), `thickness`, `height` |
| `beam` | `level` — ⚠️ the beam's **TOP**, not its underside |
| `column` | `level` (= `bottomOffset`), `height`, `planAngle` |
| `object` / `lamp` | `level`, `planAngle`, `xRatio`, `yRatio`, `reflected` |
| `polyline` / `fill` | nothing — read-only kinds |

Polygon/memo geometry, derived values and type facts (`polygonOutline`, `holes`,
`meshSublines`, `pivotOutline`, `roofClass`, `nSegments`, `isSlanted`,
`sectionWidth/Height`, `libraryPartName`, `skirtType`, `ridges`) are read-only.
Numbers may arrive as JSON int **or** real — the command tolerates both, unlike the
coordinate-array reads. Layer 2: `evp.elements.set_details()` (snake_case field
names, matching `details()`) and `evp.elements.set_level_offset({guid: metres})`.

### Attributes

```json
{
  "EvP.GetAttributeInfo": {
    "required": {"name": "str", "kind": "\"composite\" | \"profile\" | \"buildingMaterial\""},
    "optional": {},
    "output": {"name": "str", "kind": "str", "index": "int",
               "thickness?": "float", "height?": "float", "width?": "float"}
  }
}
```

### Project

```json
{
  "EvP.GetStories": {
    "required": {},
    "optional": {},
    "output": {"firstStory": "int", "lastStory": "int", "actStory": "int",
               "indices": "[int]", "names": "[str]", "levels": "[float]", "count": "int"}
  },
  "EvP.GetProjectInfo": {
    "required": {},
    "optional": {},
    "output": {"projectName": "str", "projectPath": "str", "untitled": "bool",
               "fieldNames": "[str]", "fieldKeys": "[str]", "fieldValues": "[str]", "count": "int"}
  },
  "EvP.GetPlaceInfo": {
    "required": {},
    "optional": {"year": "int", "month": "int", "day": "int",
                 "hour": "int", "minute": "int", "second": "int"},
    "output": {"longitude": "float", "latitude": "float", "altitude": "float",
               "north": "float", "northDeg": "float",
               "sunAngXY": "float", "sunAngZ": "float",
               "sunAngXYDeg": "float", "sunAngZDeg": "float",
               "sunDirX": "float", "sunDirY": "float", "sunDirZ": "float",
               "sunAzimuthDeg": "float (compass, clockwise from north)",
               "sunAltitudeDeg": "float",
               "year": "int", "month": "int", "day": "int",
               "hour": "int", "minute": "int", "second": "int",
               "summerTime": "bool", "timeZoneInMinutes": "int", "timeOverridden": "bool"}
  }
}
```

⚠️ `GetPlaceInfo` returns MIXED UNITS, as the DevKit does: longitude/latitude in
DEGREES, altitude in METRES, `north`/`sunAngXY`/`sunAngZ` in RADIANS. The `*Deg`
companions exist so a consumer never has to guess. The sun angles are computed by
`ACAPI_GeoLocation_CalcSunOnPlace` — Archicad's own, the same one its shadows use;
do not reimplement a solar formula. Omitted time fields keep the project's values,
so `{"hour": 15}` means 15:00 on the project's own date. Probe: `PlaceInfoProbe`.

**VERIFIED live 2026-08-03 against an independent NOAA calculation, at TWO
project-north values (90° and 10°) — exact on all samples:**

- `sunAngZ` is the altitude **above the horizon**.
- `sunAngXY` is a **mathematical angle CCW from the model's +X axis**, already in
  MODEL space — so `sunDirX/Y/Z` (the unit vector TO the sun) is a plain
  spherical→cartesian with no north term.
- `north` is geographic north as the same kind of angle; its 90° default puts
  north along +Y.
- **Compass bearing needs `north`: `sunAzimuthDeg = (northDeg − sunAngXYDeg) mod 360`.**

⚠️ **`compass = 90° − sunAngXY` is the trap.** It is correct only when project
north is 90°, which is the DEFAULT — so it passes on most projects and is off by
`90° − north` on the rest. It fooled the first verification run for exactly that
reason. Use `sunAzimuthDeg`, computed in C++, rather than re-deriving it.

### Change Notification (E25) — "has the model changed since I last looked"

```json
{
  "EvP.WatchModel": {
    "required": {},
    "optional": {"enable": "bool (default true)", "guids": "[str] (the bounded path)",
                 "background": "bool (DEFAULT TRUE — returns at once, never stalls)",
                 "sliceMs": "int (default 6)", "gapMs": "int (default 25)",
                 "scope": "\"3d\" | \"all\" (synchronous pass only)",
                 "maxElements": "int (default 2000)", "budgetMs": "int (default 2000)"},
    "output": {"watching": "bool", "scope": "str",
               "listed": "int", "attached": "int", "failed": "int",
               "observed": "int (ARCHICAD's own count)", "truncated": "bool", "cancelled": "bool",
               "firstError": "str (only on a refusal)",
               "token": "int", "listMs": "int", "attachMs": "int", "elapsedMs": "int"}
  },
  "EvP.SyncModel": {
    "required": {},
    "optional": {},
    "output": {"token": "int"}
  },
  "EvP.GetObservedElements": {
    "required": {},
    "optional": {"guids": "[str]"},
    "output": {"count": "int", "observed": "[bool] (only with `guids`, parallel to them)"}
  },
  "EvP.GetChangeToken": {
    "required": {},
    "optional": {"since": "int", "maxGuids": "int (default 200)", "includeGuids": "bool (default true)"},
    "output": {"token": "int", "watching": "bool", "watchedCount": "int", "idleMs": "int",
               "arming": "bool", "armProgress": "{running, done, listed, attached, failed, slices, listMs, longestSliceMs, elapsedMs, phase}",
               "changedCount": "int (only with `since`)", "complete": "bool (only with `since`)",
                "guids": "[str] (parallel with events)", "events": "[str]"}
  },
  "EvP.GetModelDiff": {
    "required": {},
    "optional": {"scope": "\"3d\" | \"file\"", "reset": "bool"},
    "output": {"scope": "str", "baseline": "bool",
               "new": "[str]", "modified": "[str]", "deleted": "[str]",
               "newCount": "int", "modifiedCount": "int", "deletedCount": "int",
               "environmentChanged": "bool", "elapsedMs": "int"}
  },
  "EvP.TakeChanges": {
    "required": {},
    "optional": {"max": "int (default 500)", "peek": "bool"},
    "output": {"guids": "[str]", "events": "[str]", "count": "int",
               "remaining": "int", "overflowed": "bool", "peeked": "bool",
               "token": "int", "idleMs": "int"}
  }
}
```

The two halves cost wildly different amounts, deliberately. `WatchModel` is a
main-thread call that attaches an observer to EVERY element (`scope: "3d"` =
APIFilt_In3D, `"all"` = the whole current database) — call it ONCE per run.
`GetChangeToken` is **gate-free** (`NeedsMainThread()` false) and is meant to be
polled; 300 ms is cheap.

⚠️ **Arm it first.** Without `WatchModel`, Archicad reports only element
CREATION — changes and deletions to elements that already existed are not sent,
because those go only to elements with an observer attached. `watching` says
which state you are in, and goes back to false on a project new/open/close
(Archicad drops every observer with the old document; the token is bumped so a
consumer notices).

⚠️ **Branch on `complete` before `guids`.** The add-on keeps the last 2048 events
in a fixed ring; an edit storm or a stale `since` returns `complete: false`,
meaning "I no longer know which elements changed" — refresh wholesale.

**THREE MODES, and the default cannot stall Archicad** — the requirement is that
watching the model never stops the main Archicad loop; latency is fine.

| Mode | How | Main thread held |
|---|---|---|
| `background` (default) | worker thread, ~6 ms slices, 25 ms gaps | one slice |
| `guids` | inline, no listing pass, no cap | what those elements cost |
| `background: false` | synchronous whole-model pass, capped + cancellable | seconds |

⚠️ **`WatchModel` FROZE ARCHICAD ONCE**, in an uncapped synchronous pass. On a
20-element project the listing took 3545 ms and each attach ~4 ms, both scaling
with the model. The background pass exists because of that, and lists with
APIFilt_None — which never consults the 3D model, so it cannot trigger a
generate. `armProgress.longestSliceMs` on `GetChangeToken` is the honest measure:
it is the worst the main thread was ever held.

⚠️ `budgetMs` caps the ATTACH LOOP only — `ACAPI_Element_GetElemList` is one
uninterruptible call. ⚠️ `scope: "all"` is APIFilt_None: BIGGER and slower than
`"3d"` in element count, not cheaper.

**`EvP.SyncModel`** is the manual fallback — a viewer's Sync button. It bumps the
token exactly as a real change would, so the button and the observer share ONE
refresh path instead of two that drift. Gate-free.

⚠️ **ARCHICAD DOES NOT NOTIFY AN ADD-ON OF ITS OWN CHANGES.** Proved live
2026-08-03: a bus write to a *confirmed-observed* element produced no
notification, while a human edit to the SAME element in the same session did. So
`ApiDispatcher` bumps the token itself after any successful write command or
committed transaction, as one `opaque` event with a null guid ("something
changed, identity unknown" → refresh wholesale). Without that, a viewer would sit
stale through every scripted edit.

⚠️ `truncated` / `cancelled` mean changes to the elements not reached are
INVISIBLE. Branch on them.

⚠️ `attached` is OUR tally; `observed` is Archicad's. Believe `observed` — it is
what actually gets notified. `listed` is the denominator both need (an
`attached: 20` with no `listed` cannot distinguish a 20-element project from 20
of 3000). `listMs` vs `attachMs` matter because `scope: "3d"` (APIFilt_In3D) has
to consult the 3D model and can force it to generate — 5.2 s was measured on the
first live run.

`EvP.GetObservedElements` is the diagnostic for "the token did not move": an
element that was never watched and one that was watched but reported nothing look
identical from Python and have opposite fixes.

`events` are `"opaque" | "new" | "copy" | "change" | "edit" | "delete" | "undoCreated" |
"undoModified" | "undoDeleted" | "redoCreated" | "redoModified" | "redoDeleted" |
"propertyValue" | "classification"`. `idleMs` is milliseconds since the last
notification (-1 if none ever) — a drag fires continuously, so wait for the model
to SETTLE before rebuilding. Python: `evp.changes`. Probe: `ChangeTokenProbe`.

**`EvP.GetModelDiff`** — Observer-free diff via `ACAPI_DifferenceGenerator_*` (AC26+).
The first call establishes a baseline and reports nothing changed; each later
call diffs the current project against that baseline, returns the difference,
and adopts the current state as the new baseline. `scope: "3d"` (default,
APIDiff_3DModelBased — what a viewer cares about) or `"file"` (every element
edit). `environmentChanged` covers the north, layers, etc. ⚠️ **It is a poll,
not a notification** — latency is fine, call it from a background loop.
`elapsedMs` is returned so a caller can back off on its own. On first call (or
with `reset:true`), `baseline` is `true` and the lists are empty — "nothing to
compare against", NOT "nothing changed". Gate-free read, one main-thread call.

**`EvP.TakeChanges`** — Drains entries from the change tracker's ring buffer
explicitly, taking up to `max` entries (default 500, 0 = unlimited). With
`peek:true`, leaves them for a later drain. `remaining > 0` just means "drain
again" (normal); `overflowed` means more than the ring capacity changed and some
were dropped — refresh wholesale. **Gate-free** — this only moves entries out of
a std::map. Parallel `guids`/`events` arrays like `GetChangeToken`.

### Selection & UI

```json
{
  "EvP.GetSelection": {
    "required": {},
    "optional": {},
    "output": {"guids": "[str]", "count": "int"}
  },
  "EvP.SetSelection": {
    "required": {"guids": "[str]"},
    "optional": {"add": "bool"},
    "output": {"selected": "int", "missing": "[str]", "count": "int"}
  },
  "EvP.ModifySelection": {
    "required": {"op": "\"add\" | \"remove\" | \"replace\" | \"clear\""},
    "optional": {"guids": "[str] (required unless op=clear)"},
    "output": {"selected": "int", "missing": "[str]", "changed": "int", "count": "int"}
  },
  "EvP.ModifySelectionSet": {
    "required": {"name": "str", "op": "\"update\" | \"add\" | \"remove\" | \"clear\""},
    "optional": {"guids": "[str]", "current": "bool (capture live Archicad selection)"},
    "output": {"changed": "int", "count": "int", "guids": "[str]"}
  },
  "EvP.GetSelectionSet": {
    "required": {"name": "str"},
    "optional": {},
    "output": {"guids": "[str]", "count": "int"}
  },
  "EvP.ListSelectionSets": {
    "required": {},
    "optional": {},
    "output": {"names": "[str]"}
  },
  "EvP.ReselectSelectionSet": {
    "required": {"name": "str"},
    "optional": {},
    "output": {"selected": "int", "missing": "[str]", "changed": "int", "count": "int"}
  },
  "EvP.HighlightElements": {
    "required": {"guids": "[str]"},
    "optional": {"color": "[r,g,b,a]", "wireframe3D": "bool", "dimOthers": "[r,g,b,a]"},
    "output": {"count": "int"}
  },
  "EvP.ClearHighlights": {
    "required": {},
    "optional": {},
    "output": {}
  },
  "EvP.ZoomTo": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int"}
  }
}
```

### UI Feedback (bus-local — no gate hop for SetStatus/ShowAlert)

```json
{
  "EvP.SetStatus": {
    "required": {"message": "str"},
    "optional": {},
    "output": {"shown": "str"}
  },
  "EvP.ShowAlert": {
    "required": {"message": "str"},
    "optional": {},
    "output": {"shown": "str"}
  },
  "EvP.ShowResults": {
    "required": {"headers": "[str]", "rows": "[str]"},
    "optional": {},
    "output": {"rows": "int"}
  },
  "EvP.ShowSelectionPrompt": {
    "required": {"message": "str"},
    "optional": {},
    "output": {"shown": true}
  },
  "EvP.HideSelectionPrompt": {
    "required": {},
    "optional": {},
    "output": {"shown": false}
  },
  "EvP.PollSelectionPrompt": {
    "required": {},
    "optional": {},
    "output": {"continued": "bool", "cancelled": "bool", "active": "bool"}
  }
}
```

### Runtime & Diagnostics

```json
{
  "EvP.SetTracing": {
    "required": {"enabled": "bool"},
    "optional": {},
    "output": {"enabled": "bool"}
  },
  "EvP.PollCancel": {
    "required": {},
    "optional": {},
    "output": {"cancelled": "bool", "running": "bool", "reason": "str"}
  },
  "EvP.GetErrorTrail": {
    "required": {},
    "optional": {"limit": "int"},
    "output": {"entries": "[str]", "total": "int", "logPath": "str"}
  }
}
```

### Plan Overlay (Diagnostic — Win32 window probing)

```json
{
  "EvP.EnumChildWindows": {
    "required": {},
    "optional": {},
    "output": {"mainWindow": "{hwnd, className, title, rect:{l,t,r,b}, style, exStyle, isVisible, clientRect}",
               "windows": "[{hwnd, className, title, rect, style, exStyle, isVisible, clientRect, children:[int]}]",
               "count": "int"}
  },
  "EvP.ProbeWindowAt": {
    "required": {},
    "optional": {"x": "int", "y": "int", "target": "\"any\" | \"plan\" | \"model3d\""},
    "output": {"point": "{x, y, defaulted}", "target": "str",
               "windowType": "int", "isPlanWindow": "bool", "is3DWindow": "bool",
               "targetMatched": "bool", "note?": "str",
               "strictChain": "[WindowState]", "hitChain": "[WindowState]",
               "hostChain": "[WindowState] (Archicad's own tree, our overlay excluded)",
               "realChild?": "{...}"}
  },
  "EvP.ProbeOverlayShow": {
    "required": {},
    "optional": {"parent": "int (HWND, default main window)", "x": "int (default 100)",
                 "y": "int (default 100)", "w": "int (default 400)", "h": "int (default 300)",
                 "cover": "bool", "coverHwnd": "int (only when cover:true)",
                 "layered": "bool (default false)", "alpha": "int (0-255, default 255)",
                 "hatch": "bool (default false)"},
    "output": {"overlay": "WindowState", "parent": "WindowState",
               "screenRect": "{l,t,r,b}", "covered": "bool",
               "layered": "bool", "alpha": "int", "hatch": "bool"}
  },
  "EvP.ProbeOverlayHide": {
    "required": {},
    "optional": {"restoreStyle": "bool (default true)"},
    "output": {"wasVisible": "bool", "paintCount": "int", "styleRestored": "bool",
               "styleStillSet": "bool"}
  },
  "EvP.ProbeOverlayState": {
    "required": {},
    "optional": {"repaint": "bool (force repaint and re-sample)"},
    "output": {"exists": "bool", "paintCount": "int",
               "lastPaintMsAgo?": "int", "screenRect?": "{l,t,r,b}",
               "overlay?": "WindowState", "parent?": "WindowState",
               "parentClientRect?": "{l,t,r,b}", "styledWindow?": "WindowState",
               "pixels?": "{sampled, colours:{centre, nw, ne, sw, se}, magentaPoints, exactPoints}",
               "repaintPainted?": "bool", "pixelsAfterRepaint?": "..."}
  },
  "EvP.ProbeSetClipSiblings": {
    "required": {"hwnd": "int"},
    "optional": {"enable": "bool (default true)", "clipSiblings": "bool (default true)",
                 "clipChildren": "bool (default false)"},
    "output": {"target": "WindowState", "before": "str (hex style)", "after": "str (hex style)",
               "enabled": "bool"}
  },
  "EvP.ProbeForceRedraw": {
    "required": {},
    "optional": {"mode": "\"acapi\" | \"invalidate\" | \"both\" (default)", "hwnd": "int (for invalidate)"},
    "output": {"mode": "str", "acapiErr?": "int", "acapiError?": "str", "invalidated?": "bool"}
  }
}
```

⚠️ These are **diagnostic probes** — they enumerate Win32 child windows, create/hide test overlays,
and manipulate window styles at the OS level. They do NOT mutate the Archicad project (all derive
from `MainThreadCommand`, not `WriteCommand`). Only `EnumChildWindows` is registered on the JSON
port; the rest are EvP-bus-only.

### Plan Track (Overlay — Model-to-Screen Coordinate Tracking)

```json
{
  "EvP.OverlayTransform": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "error?": "str",
               "transform": "{valid, scaleX, scaleY, offX, offY,
                refModelA:{x,y}, refModelB:{x,y}, refPointA:{x,y}, refPointB:{x,y},
                impliedW, impliedH, canvasW, canvasH, dpiX, dpiY, dpiApplied}"}
  },
  "EvP.SetOverlayGeometry": {
    "required": {"polylines": "[[x,y,x,y,...], ...] (model-space metres, flat pairs)"},
    "optional": {},
    "output": {"ok": "bool", "polylines": "int", "points": "int"}
  },
  "EvP.SetOverlayTracking": {
    "required": {},
    "optional": {"enable": "bool (default true)", "intervalMs": "int (default 33)"},
    "output": {"ok": "bool", "error?": "str", "tracking": "bool", "intervalMs": "int"}
  },
  "EvP.OverlayTrackStats": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "exists": "bool", "tracking": "bool", "intervalMs": "int",
               "polls": "int", "recomputes": "int", "repaints": "int",
               "acapiFailures": "int",
               "transform": "{valid, scaleX, scaleY, ...}", "modelToken": "int"}
  },
  "EvP.OverlayCalibrate": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "error?": "str",
               "rows": "[{window, clientW, clientH, impliedW, impliedH, kx, ky, disagree}]"}
  }
}
```

⚠️ These commands need an overlay window created via `ProbeOverlayShow` first — the transform
and tracking are derived from the canvas it covers. `OverlayCalibrate` answers all candidate
window projections in one pass; the agreeing row's `k` is the display-scaling factor.

### ArchViz — Custom 3D Viewer (bgfx/Diligent)

```json
{
  "EvP.OpenViewer": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "posted": "bool", "error?": "str"}
  },
  "EvP.CloseViewer": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "posted": "bool", "error?": "str"}
  },
  "EvP.ViewerState": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "running": "bool", "initialized": "bool", "failed": "bool",
               "error": "str", "renderer": "str", "width": "int", "height": "int",
               "frames": "int", "fps": "float", "resets": "int", "backends": "str",
               "elements": "int", "triangles": "float", "vertices": "float",
               "gpuBytes": "float", "pending": "int", "materials": "int",
               "materialMisses": "int", "transparentRanges": "int",
               "pickAvailable": "bool", "pickSeq": "float", "pickedGuid": "str",
               "selectedCount": "int",
               "sunApplied": "bool", "sunBelowHorizon": "bool",
               "sunX": "float", "sunY": "float", "sunZ": "float", "ambient": "float"}
  },
  "EvP.ViewerNavLog": {
    "required": {},
    "optional": {"enable": "bool (default true)", "intervalMs": "int (default 50)",
                 "query": "bool (read-only, no state change)"},
    "output": {"ok": "bool", "error?": "str",
               "running": "bool", "intervalMs": "int",
               "viewerRows": "int", "archicadRows": "int", "archicadFails": "int",
               "maxArchicadGapMs": "int", "path": "str"}
  },
  "EvP.ViewerRefresh": {
    "required": {},
    "optional": {"full": "bool (default true)", "live": "bool",
                 "stop": "bool", "query": "bool (read-only)",
                 "sliceMs": "int (default 8)", "gapMs": "int (default 16)",
                 "maxSeconds": "int (default 300)",
                 "settleMs": "int (default 400, live mode)", "pollMs": "int (default 100, live mode)"},
    "output": {"ok": "bool", "started": "bool", "running": "bool", "done": "bool",
               "gaveUp": "bool", "phase": "str", "total": "int", "extracted": "int",
               "empty": "int", "pushed": "int", "materials": "int",
               "triangles": "float", "slices": "int",
               "longestHoldMs": "int", "longestRoundTripMs": "int",
               "acquireMs": "int", "throttledMs": "int", "elapsedMs": "int",
               "live": "bool", "fullPasses": "int", "partialPasses": "int",
               "removed": "int", "armed": "int", "armRefused": "int",
               "handlersInstalled": "bool", "dirtyPending": "int",
               "lastSyncMs": "int", "lastPassMs": "int"}
  },
  "EvP.ProbeDiligentDevice": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "posted": "bool", "error?": "str"}
  },
  "EvP.DiligentProbeState": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "attempted": "bool", "running": "bool",
               "succeeded": "bool", "error": "str"}
  },
  "EvP.OpenDiligentViewport": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "posted": "bool", "error?": "str"}
  },
  "EvP.DiligentViewportState": {
    "required": {},
    "optional": {},
    "output": {"ok": "bool", "running": "bool", "initialized": "bool", "failed": "bool",
               "error": "str", "frames": "int", "fps": "float",
               "width": "int", "height": "int", "resizes": "int"}
  }
}
```

⚠️ `OpenViewer` / `CloseViewer` / `ProbeDiligentDevice` / `OpenDiligentViewport` POST their work
to the main thread — they return at once and the panel/viewport appears on the next event-loop
turn. `ViewerState`, `DiligentProbeState`, `DiligentViewportState`, and `ViewerRefresh` are
gate-free (read atomics, no ACAPI). `ViewerNavLog` is main-thread code (uses `SetTimer` for
Archicad's camera sampling). `ViewerRefresh` with `live:true` arms an observer-based watch loop
that re-extracts changed elements; `ViewerRefresh` with `stop:true` joins the worker thread.
`handlersInstalled` distinguishes "observers attached" from "observers actually receiving events".

### Topology

```json
{
  "EvP.GetCollisions": {
    "required": {"guids1": "[str]", "guids2": "[str]"},
    "optional": {"volumeTolerance": "float", "performSurfaceCheck": "bool", "surfaceTolerance": "float"},
    "output": {"elemGuids1": "[str]", "elemGuids2": "[str]", "hasBodyCollision": "[bool]",
               "hasClearenceCollision": "[bool]", "count": "int"}
  },
  "EvP.GetConnectedElements": {
    "required": {"guids": "[str]", "connectedElementType": "str"},
    "optional": {},
    "output": {"guids": "[str]", "counts": "[int]", "connectedGuids": "[str]", "count": "int"}
  }
}
```

### Screenshots

```json
{
  "EvP.CaptureScreenshot": {
    "required": {},
    "optional": {"view": "\"current\" | \"top\""},
    "output": {"view": "str", "bytes": "int", "url": "str"}
  }
}
```

### 3D View & Projection

```json
{
  "EvP.Get3DProjection": {
    "required": {},
    "optional": {},
    "output": {"isPersp": "bool",
               "isPersp=true":  {"posX": "float", "posY": "float", "cameraZ": "float",
                "targetX": "float", "targetY": "float", "targetZ": "float",
                "azimuth": "float", "rollAngle": "float", "viewCone": "float",
                "distance": "float", "isTwoPointPersp": "bool"},
               "isPersp=false": {"azimuth": "float", "projMod": "int",
                "tranmat": "[float*12]", "invtranmat": "[float*12]"}}
  },
  "EvP.Get3DWindowSets": {
    "required": {},
    "optional": {},
    "output": {"hSize": "int", "vSize": "int", "zoomScaleX": "float",
               "zoomScaleY": "float", "zoomDispX": "float", "zoomDispY": "float"}
  },
  "EvP.Set3DProjection": {
    "required": {},
    "optional": {"azimuthDelta": "float (degrees)", "azimuth": "float (degrees, set absolute)",
                 "distance": "float", "cameraZ": "float", "targetZ": "float",
                 "viewCone": "float", "regenerate": "bool (default true)"},
    "output": {"changed": "bool", "before": "{isPersp, azimuth, ...} (full, for restore)",
               "after": "{isPersp, azimuth, ...} (read-back, not echoed)", "refused?": "[str]"}
  },
  "EvP.ModelToScreen": {
    "required": {"points": "[[x,y,z], ...] (metres)"},
    "optional": {},
    "output": {"count": "int", "failed": "int", "windowType": "int", "is3DWindow": "bool",
               "screen": "[{ok, x?, y?, why?}] (pixels)", "coord": "[{ok, x?, y?, why?}] (drawing metres)"}
  }
}
```

⚠️ `Set3DProjection` is SPARSE — only the fields present are changed; the rest keep the user's values. `azimuth` / `azimuthDelta` is in DEGREES. Not undoable (view setting, not a database edit). `before` is returned so a caller can restore the view exactly.

⚠️ `ModelToScreen` uses Archicad's own ModelToScreen -> CoordToPoint pipeline, returning BOTH pixel and drawing-space coordinates. Reports `is3DWindow` so a caller can verify the right window answered.

### Issues (Mark-ups)

```json
{
  "EvP.CreateIssue": {
    "required": {"name": "str"},
    "optional": {"tagText": "str"},
    "output": {"issueId": "str"}
  },
  "EvP.AttachElementsToIssue": {
    "required": {"issueId": "str", "guids": "[str]"},
    "optional": {"componentType": "\"Highlight\" | \"Creation\" | \"Deletion\" | \"Modification\""},
    "output": {"attached": "int"}
  }
}
```

### Element Modification (Writes)

```json
{
  "EvP.PlaceLevelDimension": {
    "required": {"x": "float", "y": "float"},
    "optional": {"mode": "\"static\" | \"associative\"", "value": "float",
                 "parentGuid": "str", "text": "str"},
    "output": {"guid": "str"}
  },
  "EvP.CreateMesh": {
    "required": {"outline": "[x0,y0,x1,y1,...]"},
    "optional": {"polyZ": "[float]", "baseLevel": "float", "ridgeCoords": "[x,y,z,...]",
                 "ridgeCounts": "[int]", "skirtLevel": "float",
                 "skirt": "\"SolidBodyWithSkirt\"|\"WithSkirt\"|\"SurfaceOnlyWithoutSkirt\"",
                 "floorInd": "int", "onFloorPlan": "bool", "layer": "str"},
    "output": {"guid": "str", "baseLevel": "float", "skirtLevel": "float",
               "floorInd": "int", "switchedToFloorPlan": "bool"}
  },
  "EvP.CreateWall": {
    "required": {"begX": "[float]", "begY": "[float]", "endX": "[float]", "endY": "[float]"},
    "optional": {"arcAngles": "[float]", "base": "float", "floorInd": "int",
                 "height": "float", "structure": "\"basic\"|\"composite\"|\"profile\"",
                 "attrName": "str", "thickness": "float",
                 "refLine": "\"outside\"|\"center\"|\"inside\"|\"coreOutside\"|\"coreCenter\"|\"coreInside\"",
                 "flipped": "bool"},
    "output": {"guids": "[str]", "created": "int", "floorInd": "int", "warning?": "str"}
  },
  "EvP.CreateColumn": {
    "required": {"x": "[float]", "y": "[float]"},
    "optional": {"base": "float", "floorInd": "int", "height": "float",
                 "layer": "str", "shape": "\"rectangular\"|\"circular\"|\"profile\"",
                 "width": "float", "height2": "float", "diameter": "float",
                 "profile": "str", "material": "str", "angle": "float"},
    "output": {"count": "int", "guids": "[str]", "results": "[{ok, guid?, error?}]", "floorInd": "int"}
  },
  "EvP.CreateRoof": {
    "required": {"outline": "[x0,y0,x1,y1,...]"},
    "optional": {"arcs": "[float]", "base": "float", "floorInd": "int", "pitch": "float",
                 "mode": "\"plane\" | \"poly\"", "riseLeft": "bool",
                 "baseLine": "[x1,y1,x2,y2]", "planeHeight": "float", "overhang": "float",
                 "polyContour": "bool", "structure": "\"basic\"|\"composite\"",
                 "attrName": "str", "winding": "\"asis\"|\"cw\"|\"ccw\""},
    "output": {"guid": "str", "floorInd": "int"}
  },
  "EvP.CreatePlaneRoofSample": {
    "required": {},
    "optional": {"x": "float", "y": "float", "angle": "float", "hole": "bool",
                 "size": "float", "devStory": "bool", "devStructure": "bool",
                 "devReassertClass": "bool", "devThickness": "bool",
                 "devPosSign": "bool", "devBaseLineEdge": "bool", "attrName": "str",
                 "base": "float", "floorInd": "int", "riseLeft": "bool"},
    "output": {"deviations": "str", "guid": "str", "floorInd": "int"}
  },
  "EvP.PlaceLibraryObject": {
    "required": {"libraryPartNames": "[str]", "x": "float", "y": "float"},
    "optional": {"angle": "float", "floorInd": "int", "level": "float", "layer": "str",
                 "paramNames": "[str]", "paramValues": "[str]", "paramCount": "int",
                 "inheritFromGuid": "str"},
    "output": {"guid": "str", "libraryPartName": "str", "libInd": "int"}
  },
  "EvP.SetElementSurface": {
    "required": {"guids": "[str]"},
    "optional": {"surface": "str", "clear": "bool", "restoreMats": "[int]", "restoreChained": "[bool]"},
    "output": {"guids": "[str]", "found": "[bool]", "changed": "[bool]", "kind": "[str]",
               "prevMats": "[int]", "prevChained": "[bool]", "op": "\"paint\"|\"clear\"|\"restore\""}
  }
}
```

### Drafting

```json
{
  "EvP.GetTextElements": {
    "required": {},
    "optional": {"guids": "[str]"},
    "output": {"fromSelection": "bool", "texts": "[{guid, content, x, y, hasBounds, xMin, yMin, xMax, yMax,
               anchorX, anchorY, anchor, pen, angle, size, multiStyle, nLine, floorInd, layer}]",
               "count": "int", "skipped": "int"}
  },
  "EvP.GetArcElements": {
    "required": {},
    "optional": {"guids": "[str]", "scope": "\"database\"|\"selection\"", "wholeOnly": "bool"},
    "output": {"scope": "str", "arcs": "[{guid, x, y, radius, isCircle, begAngle, endAngle,
               ratio, angle, pen, layer, floorInd}]", "count": "int", "skipped": "int"}
  },
  "EvP.CreateText": {
    "required": {"texts": "[{text, x, y}]"},
    "optional": {"texts[i].floorInd": "int", "texts[i].layer": "str", "texts[i].size": "float",
                 "texts[i].angle": "float", "texts[i].pen": "int", "texts[i].font": "int",
                 "texts[i].just": "\"left\"|\"center\"|\"right\"|\"full\"",
                 "texts[i].anchor": "\"topLeft\"|\"topCenter\"|\"...\"",
                 "texts[i].bold": "bool", "texts[i].italic": "bool", "texts[i].underline": "bool",
                 "texts[i].strikeOut": "bool", "texts[i].width": "float",
                 "texts[i].fixedAngle": "bool", "texts[i].fixedSize": "bool",
                 "texts[i].spacing": "float", "texts[i].widthFactor": "float",
                 "texts[i].charSpaceFactor": "float", "texts[i].inheritDefaults": "bool"},
    "output": {"count": "int", "guids": "[str]", "results": "[{ok, guid?, error?}]"}
  },
  "EvP.PlacePicture": {
    "required": {"path": "str", "x": "float", "y": "float"},
    "optional": {"floorInd": "int", "layer": "str", "width": "float", "height": "float",
                 "rotAngle": "float", "anchor": "str", "mirrored": "bool",
                 "transparent": "bool", "name": "str", "usePixelSize": "bool", "dpi": "float"},
    "output": {"guid": "str", "pixelWidth": "int", "pixelHeight": "int",
               "placedWidth?": "float", "placedHeight?": "float"}
  }
}
```

### Drawings

```json
{
  "EvP.GetDrawingClipPolygon": {
    "required": {"guids": "[str]"},
    "optional": {},
    "output": {"count": "int", "drawings": "[{guid, found, isCutWithFrame, clipPolygon:[x,y,...],
               arcs:[...], pos:{x,y}, bounds:{xMin,yMin,xMax,yMax}, ratio, drawingScale, name, error?}]"}
  },
  "EvP.SetDrawingClipPolygon": {
    "required": {"guid": "str", "clipPolygon": "[x0,y0,x1,y1,...]"},
    "optional": {"arcs": "[float]", "isCutWithFrame": "bool"},
    "output": {"guid": "str", "pointsWritten": "int", "pointsReadBack": "int", "verified": "bool", "note?": "str"}
  },
  "EvP.PlaceDrawingFromView": {
    "required": {"x": "float", "y": "float"},
    "optional": {"viewName": "str", "viewGuid": "str", "layoutName": "str", "layoutGuid": "str",
                 "ratio": "float", "angle": "float", "anchor": "str",
                 "name": "str", "clipPolygon": "[x,y,...]", "arcs": "[float]",
                 "modelOffset": "{x, y}", "layer": "str"},
    "output": {"guid": "str", "viewName": "str", "layout": "str", "cropped": "bool"}
  }
}
```

### Layout Book

```json
{
  "EvP.ListDatabases": {
    "required": {},
    "optional": {"types": "[\"worksheet\", \"detail\", \"layout\", \"masterLayout\", \"3dDocument\"]"},
    "output": {"count": "int", "databases": "[{type, guid, name, ref, title, masterLayoutGuid?, error?}]"}
  },
  "EvP.ListViews": {
    "required": {},
    "optional": {"map": "\"view\"|\"myView\"|\"private development "|\"layout\"", "placeableOnly": "bool"},
    "output": {"count": "int", "views": "[{name, guid, path, itemType, placeable, map}]", "note?": "str"}
  },
  "EvP.CreateDatabase": {
    "required": {"type": "\"worksheet\"|\"detail\"|\"3dDocument\""},
    "optional": {"name": "str", "ref": "str"},
    "output": {"type": "str", "guid": "str", "name": "str", "ref": "str"}
  },
  "EvP.SetDocumentFrom3DSettings": {
    "required": {"guid": "str"},
    "optional": {"fromCurrent3DView": "bool", "transparency": "bool", "cutaway3D": "bool", "materialFrom3D": "bool"},
    "output": {"guid": "str", "name": "str", "isPersp": "bool", "applied": "[str]", "verified": "bool"}
  },
  "EvP.DeleteDatabase": {
    "required": {"type": "str", "guids": "[str]", "confirm": "bool"},
    "optional": {},
    "output": {"deleted": "int", "results": "[{guid, ok, name?, error?}]"}
  }
}
```

### Transactions

```json
{
  "EvP.CommitTransaction": {
    "required": {"name": "str", "steps": "[{command, params, bindings, bindingCount}]"},
    "optional": {},
    "output": {"results": "[str]", "steps": "int"}
  }
}
```

---

## API.* Commands (Backend: API — Archicad JSON API Proxy)

Proxied in-process via the `archicad` Python package. Source of truth: `AddOn/reference/python-api-archicad-29.3000/`.

> Note: The local `archicad` package is the **only** trustworthy source for `API.*` schema. Never take `API.*` names/enums from Tapir's docs (`AGENTS.md`).

### Properties

```json
{
  "API.GetAllPropertyGroupIds": {
    "required": {},
    "optional": {"propertyType": "\"BuiltIn\" | \"UserDefined\""},
    "output": {"propertyGroupIds": "[{guid}]"}
  },
  "API.GetPropertyGroups": {
    "required": {"propertyGroupIds": "[{guid}]"},
    "optional": {},
    "output": {"propertyGroups": "[...]"}
  },
  "API.GetAllPropertyNames": {
    "required": {},
    "optional": {},
    "output": {"properties": "[...]"}
  },
  "API.GetPropertyIds": {
    "required": {"properties": "[{type, nonLocalizedName | localizedName}]"},
    "optional": {},
    "output": {"properties": "[...]"}
  },
  "API.GetDetailsOfProperties": {
    "required": {"properties": "[{propertyId}]"},
    "optional": {},
    "output": {"properties": "[...]"}
  },
  "API.GetPropertyDefinitionAvailability": {
    "required": {"propertyIds": "[{propertyId}]"},
    "optional": {},
    "output": {"availabilities": "[...]"}
  },
  "API.GetPropertyValuesOfElements": {
    "required": {"elements": "[{elementId}]", "properties": "[{propertyId}]"},
    "optional": {"onlySupportedTypes": "bool"},
    "output": {"propertyValuesForElements": "[...]"}
  },
  "API.SetPropertyValuesOfElements": {
    "required": {"elementPropertyValues": "[{elementId, propertyId, propertyValue}]"},
    "optional": {},
    "output": {"executionResults": "[...]"}
  }
}
```

---

## Tapir.* Commands (Backend: Tapir — Proxy to Tapir Add-on)

Passed through `API.ExecuteAddOnCommand`. Document specific commands the repo actually uses; the catch-all covers the rest.

```json
{
  "Tapir.GetZoneBoundaries": {
    "required": {"zoneElementId": "{guid}"},
    "optional": {},
    "output": {"zoneBoundaries": "[...]"}
  },
  "Tapir.*": {
    "required": "varies",
    "optional": "varies",
    "output": "varies"
  }
}
```

---

## Transport

```
evp.api.call(command: str, params?: dict, raise_on_error?: bool) -> Result
```

```json
{
  "Result": {
    "ok": "bool",
    "data": "dict | null",
    "error": {"code": "str, message: str, detail?: str} | null",
    "meta": {"backend": "str", "zone": "str", "duration_ms": "float",
             "main_thread_ms": "float", "api_version": "str", "call_id": "str"}
  }
}
```

`API_VERSION`: `"1.0.0"`

Backends: `EvP.*` (native, in-process), `API.*` (Archicad JSON API, through the gate), `Tapir.*` (proxied via `API.ExecuteAddOnCommand`).

Errors raise `evp.api.EvpError` by default (pass `raise_on_error=False` to get the envelope). Cancellation raises `evp.api.Cancelled`.

---

## Registry Verification

109 commands registered in `CommandRegistry.cpp` via `GetName()` + `MakeCommand`:

| Factory | Commands |
|---------|----------|
| MakeSnapshotCommand | BuildSnapshot, ReleaseSnapshot, GetSnapshotInfo, GetStatus |
| MakeCaptureCommand | CaptureScreenshot, Get3DProjection, Get3DWindowSets, Set3DProjection, ModelToScreen |
| MakeQueryCommand | Raycast, SliceZ, RaycastAll, RaycastAllBatch, ClosestPoint, NearestElements, Query |
| MakeElementReadCommand | DumpRoof, GetElementInfo, GetSlabDetails, GetElementDetails, GetLibraryPartInfo, FindPlacedObjects |
| MakeElementModifyCommand | SetElementDetails |
| MakeIdentityCommand | GetElementIds, SetElementIds |
| MakeAttributeCommand | GetAttributeInfo |
| MakeProjectCommand | GetStories, GetProjectInfo, GetPlaceInfo |
| MakeCreateCommand | PlaceLevelDimension, CreateMesh, CreateWall, CreateColumn |
| MakeRoofCreateCommand | CreateRoof, CreatePlaneRoofSample |
| MakeDraftingCommand | GetTextElements, GetArcElements, CreateText, PlacePicture |
| MakeDrawingCommand | GetDrawingClipPolygon, SetDrawingClipPolygon, PlaceDrawingFromView |
| MakeLayoutCommand | ListDatabases, ListViews, CreateDatabase, SetDocumentFrom3DSettings, DeleteDatabase |
| MakeLibraryObjectCommand | PlaceLibraryObject |
| MakeSelectionCommand | GetSelection, SetSelection, ModifySelection, ModifySelectionSet, GetSelectionSet, ListSelectionSets, ReselectSelectionSet, HighlightElements, ClearHighlights, ZoomTo |
| MakeTopologyCommand | GetCollisions, GetConnectedElements |
| MakeIssueCommand | CreateIssue, AttachElementsToIssue |
| MakeSurfaceCommand | SetElementSurface |
| MakeModelGeometryCommand | GetModelInfo, GetModelElements, GetBodyGeometry |
| MakeModelAppearanceCommand | GetModelMaterials, GetModelColors, GetModelTextures, GetTexturePixels, GetModelLights, GetTextureCoordinates |
| MakeNurbsCommand | GetNurbsBody, GetPointClouds |
| MakeComponent3DCommand | Get3DComponentCounts, GetElement3DInfo, GetBodyComponents, DecomposePolygon, GetTextureCoordAtPoint |
| MakeCuttingPlaneCommand | GetCutPolygons, GetBodyBuildingMaterials, GetConnectionTable |
| MakeNotifyCommand | WatchModel, GetChangeToken, TakeChanges, GetModelDiff, GetObservedElements, SyncModel |
| MakePlanOverlayCommand | EnumChildWindows, ProbeWindowAt, ProbeOverlayShow, ProbeOverlayHide, ProbeOverlayState, ProbeSetClipSiblings, ProbeForceRedraw |
| MakePlanTrackCommand | OverlayTransform, SetOverlayGeometry, SetOverlayTracking, OverlayTrackStats, OverlayCalibrate |
| MakeArchVizCommand | OpenViewer, CloseViewer, ViewerState, ViewerNavLog, ViewerRefresh, ProbeDiligentDevice, DiligentProbeState, OpenDiligentViewport, DiligentViewportState |

Bus-local commands (handled in ApiDispatcher, not in CommandRegistry):
SetTracing, GetErrorTrail, SetStatus, ShowAlert, ShowResults, PollCancel, PollSelectionPrompt, ShowSelectionPrompt, HideSelectionPrompt, CommitTransaction.

Cross-check: every `GetName()` return in the 27 `*Commands.cpp` files matches the table above. Every row has a source in the handlers.
