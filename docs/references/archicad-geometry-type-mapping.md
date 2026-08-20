# Archicad 29 API Geometry Type Mapping

## Document Information
- **Source**: Extracted from Archicad 29 API DevKit Documentation
- **Date**: 2026-07-10
- **Purpose**: Mapping between numeric geometry types (Type4, Type20, etc.) and their actual element names
- ✅ **Verified 2026-07-25** against `reference/archicad29-api-devkit/Support/Inc/APIdefs_Elements.h`
  (the `API_ElemTypeID` enum itself, not the generated HTML): all **74** rows match, with no
  symbol in the header missing here and none here absent from the header. This table is
  therefore safe to cite — unusual enough in this repo to be worth stating.

> **This is the decoder for `typeId`.** `EvP.GetElementInfo` returns `type` and
> `EvP.GetElementDetails` returns `typeId` on a miss record. Both also offer a `typeName`, but
> that comes from `ACAPI_Element_GetElemTypeName` and is **localized to the running Archicad's
> language** — fine for a human reading a log, wrong as a machine key. Match on the **number**
> and decode it here.
>
> ⚠️ **One exception, measured 2026-07-25: `150 API_ExternalElemID` is a BUCKET, not a type.**
> A single 813-element sweep found **eight** distinct things sharing it — Piping Routing
> Element (×116), Piping Branch (×48), Equipment (×23), Piping Accessory (×17), Piping Terminal
> (×7), Ventilation Routing Element (×5), Ventilation Terminal (×3), Ventilation Branch (×1) —
> i.e. the whole MEP add-on's element set, 220 elements under one id. For `typeId == 150` the
> **only** discriminator available from this API is the localized `typeName`, which is exactly
> the thing that is not a stable key. Treat 150 as "some external add-on's element", do not try
> to branch on it, and if MEP ever matters, reach for the MEP add-on's own API rather than
> decoding names here.

## Repository Disposition

- Authority: `AddOn/reference/archicad29-api-devkit/Support/Inc/APIdefs_Elements.h`, cross-checked against the generated DevKit documentation.
- Conclusion: stable numeric `typeId` decoder for the recorded AC29 DevKit snapshot; localized `typeName` remains human-readable only, and external-element type 150 is intentionally not decoded further.

---

## Element Type ID Mapping

Based on the `API_ElemTypeID` enum from the official Archicad API documentation:

| Type ID | Constant Name | Element Type |
|---------|---------------|--------------|
| 0 | API_ZombieElemID | Zombie Element |
| 1 | API_WallID | Wall |
| 2 | API_ColumnID | Column |
| 3 | API_BeamID | Beam |
| 4 | API_WindowID | Window |
| 5 | API_DoorID | Door |
| 6 | API_ObjectID | Object |
| 7 | API_LampID | Lamp |
| 8 | API_SlabID | Slab |
| 9 | API_RoofID | Roof |
| 10 | API_MeshID | Mesh |
| 11 | API_DimensionID | Dimension |
| 12 | API_RadialDimensionID | Radial Dimension |
| 13 | API_LevelDimensionID | Level Dimension |
| 14 | API_AngleDimensionID | Angle Dimension |
| 15 | API_TextID | Text |
| 16 | API_LabelID | Label |
| 17 | API_ZoneID | Zone |
| 18 | API_HatchID | Hatch |
| 19 | API_LineID | Line |
| 20 | API_PolyLineID | Polyline |
| 21 | API_ArcID | Arc |
| 22 | API_CircleID | Circle |
| 23 | API_SplineID | Spline |
| 24 | API_HotspotID | Hotspot |
| 25 | API_CutPlaneID | Cut Plane |
| 26 | API_CameraID | Camera |
| 27 | API_CamSetID | Camera Set |
| 28 | API_GroupID | Group |
| 29 | API_SectElemID | Section Element |
| 30 | API_DrawingID | Drawing |
| 31 | API_PictureID | Picture |
| 32 | API_DetailID | Detail |
| 33 | API_ElevationID | Elevation |
| 34 | API_InteriorElevationID | Interior Elevation |
| 35 | API_WorksheetID | Worksheet |
| 36 | API_HotlinkID | Hotlink |
| 37 | API_CurtainWallID | Curtain Wall |
| 38 | API_CurtainWallSegmentID | Curtain Wall Segment |
| 39 | API_CurtainWallFrameID | Curtain Wall Frame |
| 40 | API_CurtainWallPanelID | Curtain Wall Panel |
| 41 | API_CurtainWallJunctionID | Curtain Wall Junction |
| 42 | API_CurtainWallAccessoryID | Curtain Wall Accessory |
| 43 | API_ShellID | Shell |
| 44 | API_SkylightID | Skylight |
| 45 | API_MorphID | Morph |
| 46 | API_ChangeMarkerID | Change Marker |
| 47 | API_StairID | Stair |
| 48 | API_RiserID | Riser |
| 49 | API_TreadID | Tread |
| 50 | API_StairStructureID | Stair Structure |
| 51 | API_RailingID | Railing |
| 52 | API_RailingToprailID | Railing Toprail |
| 53 | API_RailingHandrailID | Railing Handrail |
| 54 | API_RailingRailID | Railing Rail |
| 55 | API_RailingPostID | Railing Post |
| 56 | API_RailingInnerPostID | Railing Inner Post |
| 57 | API_RailingBalusterID | Railing Baluster |
| 58 | API_RailingPanelID | Railing Panel |
| 59 | API_RailingSegmentID | Railing Segment |
| 60 | API_RailingNodeID | Railing Node |
| 61 | API_RailingBalusterSetID | Railing Baluster Set |
| 62 | API_RailingPatternID | Railing Pattern |
| 63 | API_RailingToprailEndID | Railing Toprail End |
| 64 | API_RailingHandrailEndID | Railing Handrail End |
| 65 | API_RailingRailEndID | Railing Rail End |
| 66 | API_RailingToprailConnectionID | Railing Toprail Connection |
| 67 | API_RailingHandrailConnectionID | Railing Handrail Connection |
| 68 | API_RailingRailConnectionID | Railing Rail Connection |
| 69 | API_RailingEndFinishID | Railing End Finish |
| 70 | API_BeamSegmentID | Beam Segment |
| 71 | API_ColumnSegmentID | Column Segment |
| 72 | API_OpeningID | Opening |
| 150 | API_ExternalElemID | External Element |

---

## Common Geometry Type References

Based on the user's request for "Type4", "Type20", etc., here are the most commonly referenced geometry types:

- **Type4** = API_WindowID = Window
- **Type20** = API_PolyLineID = Polyline
- **Type1** = API_WallID = Wall
- **Type2** = API_ColumnID = Column
- **Type3** = API_BeamID = Beam
- **Type8** = API_SlabID = Slab
- **Type9** = API_RoofID = Roof

---

## What `EvP.GetElementDetails` speaks (2026-07-25)

The read is `kind`-discriminated, not type-discriminated, so this is the current coverage —
kept here because a miss record's `typeId` is decoded with the table above.

| Type ID | Element | `kind` |
|---|---|---|
| 1 | Wall | `wall` |
| 2 | Column | `column` |
| 3 | Beam | `beam` |
| 6 | Object | `object` |
| 7 | Lamp | `lamp` |
| 8 | Slab | `slab` |
| 9 | Roof | `roof` |
| 20 | Polyline | `polyline` |

Everything else returns `found:false` with `reason:"unsupportedType"` plus its `typeId`.
Observed unspoken in the wild so far: **19 Line, 22 Circle, 23 Spline** (plan §7.12). Note
**21 Arc** and **22 Circle** are distinct types — an easy pair to transpose.

---

## Source Information

This mapping was extracted from:
- **Online**: https://graphisoft.github.io/archicad-api-devkit/group___element.html
- **Local (generated HTML, what it was extracted from)**:
  `AddOn/reference/archicad29-api-devkit/docs/group___element.html`
- **Local (the enum itself — what it was VERIFIED against, and what to re-check it against)**:
  `AddOn/reference/archicad29-api-devkit/Support/Inc/APIdefs_Elements.h`, the `API_ElemTypeID`
  enum starting at `API_ZombieElemID`.

Re-verify after any devkit refresh — the devkit copies are snapshots (§E14), and this table
being *currently* exact is not a guarantee it stays exact.

---

*Document generated from Archicad 29 API DevKit*  
*Last updated: 2026-07-10*
