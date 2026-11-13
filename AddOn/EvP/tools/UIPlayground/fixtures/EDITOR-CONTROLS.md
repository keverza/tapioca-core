# Virtual editor panel: control inventory (#191 U0)

Every control `edithud::Panel` draws, directly or through the functions it calls, in the
order the panel draws them today. Taken from source at core `035d31db`. This is the list
the #191 slices are checked against: a control may move between areas, but it keeps its
action, its enable condition and its test.

Columns:

- **Label**: the ImGui label, or a description in *italics* when there is none.
- **Draw function** and **file:line**: under `Sources/AddOn/ArchViz/`; the `HudVirtualEditor` prefix is dropped.
- **Action**: what it calls or sets.
- **Enabled when**: the condition of the nearest `BeginDisabled`. "always" means no condition.
- **Test**:
  - a test that reaches the control through the panel (its label, its ID, or a pixel press);
  - `action:` a test that calls the same action without the control;
  - `none`.
- **Area**: the #191 target. A is the command bar, B the toolbar, C the Inspector, D the Outliner.
- **Ctx**: the Inspector context in #191's table (1 to 11). Several numbers mean several contexts; `all` means the shared tail under every selection context; `-` means the control is not in the Inspector.

Shared enable terms:

- **Busy**: `GizmoModeLocked (s) || !s.drawing.empty ()`.
- **ToolLock**: `s.profileTyping || s.landscapeCommand || GumballRepositioning (s)`.
  It covers the whole block from the tool radios to Local/World axes.
- **EditLock**: `s.drag != None || s.profileTyping`.
  It covers the block from Undo through `BooleanControls`.

## Panel (`Panel.cpp:202`)

| Label | Draw function | file:line | Action | Enabled when | Test | Area | Ctx |
|---|---|---|---|---|---|---|---|
| Save and close / Discard and close / Cancel | `ClosePrompt` | Draft.cpp:624 | draft Save with `closeAfter` / `s.close` / clear prompt | no `draftRequest` | HudVirtualEditorDraft.ClosingWithUnsavedWorkAsksAndEachAnswerDoesWhatItSays | C | 1 |
| *activity line* (text only) | `TerrainActivityBadge` | TerrainActivity.cpp:12 | none, display | n/a | action: HudTerrainDesign.TheEditorPreviewsFromTheFieldAndMeshesOnlyWhenAsked175 | A (state chip) | - |
| *LandscapePanel* (see next section) | `LandscapePanel` | LandscapePanel.cpp:41 | | | | | |
| *object counts* (text) | `Panel` | Panel.cpp:209 | display | n/a | none | D (header) | - |
| Export debug scene | `Panel` | Panel.cpp:214 | `RequestDebugExport` | not export pending, gesture, drawing, capture, import, gizmo lock or landscape command | action: VirtualEditorDebug.RefusesUnfinishedInputAndCancelsChangedRevisionWithoutWriting | A (File menu) | - |
| Select (V) | `Panel` | Panel.cpp:226 | `SetTool Select`, landscape mode Select, clear drawing | not ToolLock | action: HudVirtualEditor.RectangularArrayDraftCancellationCorrectionAndNoOpAreAtomic | B | - |
| Polyline (L) / Path (P) / Curve (B) | `Panel` | Panel.cpp:229-237 | `SetTool`, landscape mode DrawSource | not ToolLock | action: HudVirtualEditor (draw tests drive keys) | B | - |
| Rectangle (Q) / Polygon (N) / Ellipse (O) | `Panel` | Panel.cpp:238-246 | `SetTool`, DrawSource, clear drawing | not ToolLock | action: HudVirtualEditor.ShapeToolsPlaceAClosedOutlineFromTwoClicksInOneUndoStep | B | - |
| Mirror (M) / Trim (X) | `Panel` | Panel.cpp:247-252 | `ToolKey 'M'` / `'X'` | not ToolLock | action: HudVirtualEditor.TheMirrorToolMirrorsTheSelectionAcrossTwoClickedPoints | B | - |
| Keep original | `Panel` | Panel.cpp:254 | `s.mirrorKeeps` | Mirror tool | action: HudVirtualEditor.TheMirrorToolMirrorsTheSelectionAcrossTwoClickedPoints | C | 3 |
| *Trim hint* (text) | `Panel` | Panel.cpp:258 | display | Trim tool | none | A (prompt row) | - |
| Sides (Up/Down) | `Panel` | Panel.cpp:262 | `s.polygonSides` | Polygon tool | action: HudVirtualEditor.ShapeToolsPlaceAClosedOutlineFromTwoClicksInOneUndoStep | C | 3 |
| *shape hint* (text) | `Panel` | Panel.cpp:264 | display | shape tool | none | A (prompt row) | - |
| Add selected Archicad lines and fills | `Panel` | Panel.cpp:268 | `draftingImport`, as "lines" | no import, gesture or drawing | action: HudVirtualEditorEllipse.RefusesBusyLockedInvalidAndAmbiguousEditsWithoutHistory | C | 11 |
| As property line / As building outline / Doors as entrances | `Panel` | Panel.cpp:271-282 | `draftingImport` as property / building / entrances | as above, and a landscape exists | none | C | 11 |
| Window select / Lasso select | `Panel` | Panel.cpp:291-295 | `s.lasso` | not ToolLock | HudVirtualEditor.ACtrlDragLassoesAndThePanelChoiceSwapsIt | B | - |
| Gumball | `Panel` | Panel.cpp:302 | `SetGizmoMode Gumball` | not ToolLock or gizmo lock | action: HudVirtualEditor.AForcedModeChangeDuringAGumballDragIsRefused | B | - |
| Move (G) / Rotate (R) / Scale (S) / Stretch (T) / Extrude (E) | `Panel` | Panel.cpp:305-311 | `SetGizmoMode` | not ToolLock or gizmo lock, and `GizmoAvailable` | action: HudVirtualEditor.AForcedModeChangeDuringAGumballDragIsRefused | B | - |
| Snapping, Snap options (tree), each snap mode, Flush edges near pointer, Angle step, Grid spacing, 3D proximity | `SnapControls` | SnapControls.cpp:28 | `snapEnabled`, `snapModes`, `edgeSnap`, `angleStep`, `snapGrid`, `snapProximity` | not ToolLock | HudVirtualEditorSnapControls.GridAndTerrainElevationStartAsExplicitOptIns | B (Snap popover) | - |
| Local axes / World axes | `Panel` | Panel.cpp:313-317 | `s.localAxes` | not ToolLock | action: HudVirtualEditor.AClickOnAGumballHandleMovesNothing | B | - |
| *plane buttons*, Lock plane, Drawing plane at selected face, Horizontal drawing plane along path..., *path part plane* | `PlaneControls` | Plane.cpp:213 | `SetWorkingPlane`, `UseFacePlane`, `BeginPathPlaneEntry`, `UsePathPartPlane` | face button: one face selected; path button: a path | action: HudVirtualEditor.AFacePickCancelsInputAgainstTheCommittedFaceNotItsPreview | B (Plane popover) | - |
| Contours | `Panel` | Panel.cpp:322 | `s.contours`, republish | always | action: HudTerrainDesign.ANewMeshGenerationRetracesContoursEvenWhenTheTriangleCountIsUnchanged | B (Contours popover) | - |
| *contour step combo* (`##contourStep`) | `Panel` | Panel.cpp:333 | `s.contourStep`, republish | contours on | action: as above | B (Contours popover) | - |
| Undo / Redo | `Panel` | Panel.cpp:346-350 | `History` | not EditLock | action: HudVirtualEditorDraft and HudLandscape undo tests | A | - |
| Delete | `Panel` | Panel.cpp:352 | `DeleteSelection` | not EditLock | action: RoofSlope.DeleteChangesOnlyTheVirtualFootprintAndCanBeUndone | C (action row; the Delete key is unchanged) | all |
| Exit | `CommandBar` | CommandBar.cpp:240 | `RequestClose` | not EditLock | action: HudVirtualEditorDraft.ClosingWithUnsavedWorkAsksAndEachAnswerDoesWhatItSays | A (right edge) | - |
| *one button per applicable action* (Offset, Join, Duplicate...) | `Panel` | Panel.cpp:360-373 | `edit::Applicable (Actions (s))`, then `s.edits.Apply (op)`, `SelectMade` | not EditLock; greyed when the action makes no op | action: EditSelection.ActionsComeFromTheTable | C (action row) | all |
| Offset distance | `Panel` | Panel.cpp:378 | `actionDefaults.offsetDistance` | Offset is offered | action: EditPolyline.OffsetCopyGoesOutsideARingOfEitherWindingAndLeftOfALine | C (with Offset) | all |
| Offset join | `detail::OffsetJoinControl` | Panel.cpp:81 | the offset join setting | Offset is offered | none | C (with Offset) | all |
| Offset miter limit | `detail::OffsetMiterLimitControl` | Panel.cpp:92 | the offset miter limit | Offset is offered | none | C (with Offset) | all |
| Duplicate offset XYZ (m) | `detail::DuplicateOffsetControls` | Panel.cpp:69 | `s.duplicateOffset` | not EditLock | none | C (More) | all |
| Rectangular array..., Array spacing X (m), Array spacing Y (m), Apply array, Cancel array | `RectangularArrayControls` | Array.cpp:245 | `BeginRectangularArray`, the draft, `ApplyRectangularArray`, `CancelRectangularArray` | objects selected, Select tool, Select landscape mode, not EditLock | HudVirtualEditor.RectangularArrayPanelTypesParametersAppliesCancelsAndEscapes | C | 2, 10 |
| Apply boolean / Cancel boolean | `BooleanControls` | Boolean.cpp:122 | `ApplyBoolean` / `CancelBoolean` | a draft exists; Apply needs no draft error | none (key path covered by the HudVirtualEditorBoolean suite) | C | 2 |
| Layers and groups (tree): Hide, Lock, Isolate, Show isolated, Show all, Unlock all, Select all, Invert, Same role, Same layer, Same kind; per layer: visible, locked, name, Move here, Delete, Use this colour; New layer; group tree rows | `OrganizeControls` | Organize.cpp:243 | the `edit::` organize operations | no drag or drawing; selection buttons need a selection | action: EditOrganize suite | D (Objects); the selection-set buttons go to the `Select ▾` menu | - |
| Major/minor radius fields, Circle (use major radius) / Ellipse, Convert to curve | `EllipseControls` | Ellipse.cpp:112 | `SetSelectedEllipseValue`, `SetSelectedEllipseMode`, `ConvertSelectedEllipse` | one unlocked ellipse selected | HudVirtualEditorEllipse.RefusesBusyLockedInvalidAndAmbiguousEditsWithoutHistory | C | 8 |
| *tracker values* (text) | `Panel` | Panel.cpp:397 | display | tracker shown | action: HudVirtualEditor tracker tests | C | 3, 4 |
| *profile header* (text) | `Panel` | Panel.cpp:416 | display | a profile path | none | C | 4 |
| Kerb radius, Corner radius or Junction corner | `detail::PathRadiusControls` | Panel.cpp:159 | the path's kerb and corner radii, as a widget gesture | not `profileTyping` | HudVirtualEditor.ATypedJunctionRadiusCanBeCancelledOrCommitted | C | 4 |
| Smoothing | `detail::BreakSmoothingControl` | Panel.cpp:186 | `edit::DragCurve`, as a widget gesture | a break is selected | HudPathWidgetGesture.ASmoothingSliderDragIsOneUndoEntry | C | 4 |
| Fix elevation / Unfix elevation | `Panel` | Panel.cpp:428 | `FixElevationAt` / `UnfixBreak` | a break is selected, not `profileTyping` | action: EditPath.FixedEntranceHoldsWhileGradesChange | C | 4 |
| Crown / Falls left / Falls right | `Panel` | Panel.cpp:455-463 | `SetPathFall` | a whole path or a range is selected | action: EditPath.AFallIsSetForEveryRangeAndFlipped | C | 4 |
| Flip fall | `Panel` | Panel.cpp:467 | `FlipPathFall` | a sided fall | action: EditPath.AFallIsSetForEveryRangeAndFlipped | C | 4 |
| *profile canvas* (`##profile`): label click, break, tangent and nub drag, double-click to add a break | `Panel` | Panel.cpp:479 | `profileTyping`, `DragBreak`, `DragCurve`, `DragTangent`, `InsertPathBreak` | a profile path | HudVirtualEditor profile tests (pixel presses) | C | 4 |
| *message* (text) | `Panel` | Panel.cpp:713 | display `s.message` | non-empty | none | A (prompt row) | - |

## LandscapePanel (`LandscapePanel.cpp:41`, only with a landscape)

| Label | Draw function | file:line | Action | Enabled when | Test | Area | Ctx |
|---|---|---|---|---|---|---|---|
| Rebuild terrain (3D) | `TerrainDesignPanel` | Terrain.cpp:772 | `SetTerrainLive` | always | action: HudTerrainDesign.PathsOnlyNeitherRebuildsNorDrawsTheTerrainUntilTurnedBackOn | C | 11 |
| Topography preview (header) | `TerrainSetupPanel` | TerrainSetup.cpp:619 | none, a collapsing header | n/a | HudTerrainSetup.FreshLandscapeOffersCreationInsteadOfAnEmptySelectorAndDisabledDeadEnd | C | 11 |
| Design path (combo) | `TerrainSetupPanel` | TerrainSetup.cpp:636 | `ChooseTerrainDesignPath` | idle or a section drag | HudTerrainSetup.LiveExistingPathsResolveWithoutDrawingAndExplicitChoiceIsPreserved | C | 11 |
| Draw design path / Edit design path | `TerrainSetupPanel` | TerrainSetup.cpp:645-652 | `DrawTerrainDesignPath` / `EditTerrainDesignPath` | idle | HudTerrainSetup.DrawActionsUseTheExistingToolsAndCompletedPathBecomesTheChoice | C | 11 |
| Terrain cross-section (tree): Section ranges, the section sliders, Taper widths, Falls to, unit combos, Reload section values | `SectionPanel` | TerrainSetup.cpp:483 | the section draft, `ApplyTerrainSectionEdit`, slider drag gestures | a chosen path, idle | HudTerrainSetup.PanelRendersExplicitSectionParametersInsteadOfOnlyCaptureButtons, HudMixedPathSections, HudPathCutUnits | C | 4 (now under 11) |
| Draw property line | `TerrainSetupPanel` | TerrainSetup.cpp:659 | `DrawTerrainPropertyLine` | no property line, idle | HudTerrainSetup.FreshLandscapeOffersCreationInsteadOfAnEmptySelectorAndDisabledDeadEnd | C | 11 |
| Set terrain from selection / Recapture terrain | `TerrainSetupPanel` | TerrainSetup.cpp:666-670 | `RequestTerrainCapture` | idle; Rust build only | HudTerrainSetup.RegionOnlyWorkflowDoesNotRequireAnAuthoredDesignPath | C | 11 |
| Fixed terrain heights (header), Fix selected heights, Release selected heights, *control list* | `TerrainControlsPanel` | Terrain.cpp:119 | `SetTerrainControlSelection` | no gesture, gizmo lock or drawing | action: HudTerrainDesign.FixedControlSelectionSharesHistoryWorkerAndConflictLayer | C | 9 |
| Include paths in terrain grading | `TerrainDesignPanel` | Terrain.cpp:788 | `SetTerrainPaths` | no worker running | action: EditRegionTerrain.RegionOnlyHudStartsFromHeldTerrainAndRebuildsAfterUndo | C | 11 |
| Grade along the chosen path only | `TerrainDesignPanel` | Terrain.cpp:796 | `SetTerrainBand` | no worker, and a source | action: HudTerrainDesign.TheOutlineGradesByDefaultAndTheBandIsOneSwitchAway | C | 11 |
| Build mesh | `TerrainBuildSection` | TerrainActivity.cpp:36 | `RequestTerrainBuild` | not pending, requested or in a gesture | action: HudTerrainDesign.TheEditorPreviewsFromTheFieldAndMeshesOnlyWhenAsked175 | C (Build now); A chip when Stale | 11 |
| *preview cut/fill, unmet list, outline-step legend* (text) | `TerrainBuildSection`, `TerrainDesignPanel` | TerrainActivity.cpp:45, Terrain.cpp:805 | display | previewed / built | none | C | 11 |
| *"Landscape. Sources remain editable."* (text) | `LandscapePanel` | LandscapePanel.cpp:48 | display | n/a | none | dropped | - |
| Select sources / Draw source | `LandscapePanel` | LandscapePanel.cpp:54 | `SetLandscapeMode` | not Busy | action: HudLandscape.BackgroundViewLayoutLeavesTheFrontViewsSourceDragCaptured | B (the same as Select and the draw tools) | - |
| Place entrances | `EntrancesPanel` | Entrances.cpp:108 | `SetLandscapeMode Entrance` | not Busy or gesture | action: HudLandscape.EntrancesArePlacedOnABuildingOutlineAndAPathIsSentToOne | B | - |
| Path to entrance | `EntrancesPanel` | Entrances.cpp:112 | `ReachSelectedPathEntrance` | as above | action: as above | C | 4 |
| *entrance threshold* (`##z`), Remove | `EntrancesPanel` | Entrances.cpp:122, 139 | set the threshold Z / remove the entrance | as above | action: as above | C | 5 (building) |
| *faces / regions count* (text) | `LandscapePanel` | LandscapePanel.cpp:59 | display | n/a | none | D (Issues header) | - |
| Session snapshot / Restore snapshot | `LandscapePanel` | LandscapePanel.cpp:63-68 | `WriteLandscape` into `landscapeSaved` / `ReloadLandscape` | not Busy; Restore needs a snapshot | HudVirtualEditorDraft.TheSessionSnapshotIsNotADurableSave | A (File menu) | - |
| Recover / Discard (recovery offers) | `DraftControls` | Draft.cpp:555-562 | draft Recover / DiscardRecovery | no draft request pending, not busy | HudVirtualEditorDraft.ANextSessionOffersInterruptedWorkAndRecoverOrDiscardDoWhatTheySay159 | C | 1 |
| *draft name* (`##draft-name`), Save draft, Open draft... or Refresh drafts, Keep recovery snapshots, *draft list*, Hide drafts | `DraftControls` | Draft.cpp:582-620 | `draftName`, draft Save / List / Open, `recoveryEnabled` | not pending or busy | HudVirtualEditorDraft suite (action level) | A (File menu); the draft list opens in C ctx 1 | - |
| Union / Subtract / Intersection | `LandscapePanel` | LandscapePanel.cpp:73-80 | `BeginLandscapeCommand` | not Busy | HudVirtualEditorBoolean.PanelBooleansPickWholeRegionsDrawnInTheNormalWorkflow160 | B (`Bool ▾`) | - |
| *command name, prompt, pick count* (text) | `LandscapePanel` | LandscapePanel.cpp:82-90 | display `LandscapeCommandPrompt` | always | action: HudLandscape.AUnionMergesTwoOrMoreWholeShapesInOneUndoStep | A (prompt row) | - |
| Cancel (Esc) | `LandscapePanel` | LandscapePanel.cpp:91 | `CancelLandscapeCommand` | a command runs | action: HudVirtualEditorBoolean.PanelBooleansPickWholeRegionsDrawnInTheNormalWorkflow160 | A (prompt row) | - |
| *sub-tabs* Sources / Operations / Materials / Warnings | `LandscapePanel` | LandscapePanel.cpp:95 | tab choice | no landscape command | none | removed in U4/U5 | - |
| *next line's role* (Reference line, Region boundary, Cutter, Property line; ID `draw-source-role`) | `LandscapePanel` | LandscapePanel.cpp:100 | `s.landscapeRole` | no landscape command | action: HudLandscape.AnyLineTakesALandscapeRoleInOneUndoStep | C | 3 |
| *selected lines' role* (six roles; ID `selected-source-role`) | `LandscapePanel` | LandscapePanel.cpp:131 | `SetSelectedSourceRole` | a selection | action: HudLandscape.AnyLineTakesALandscapeRoleInOneUndoStep | C | 5, 7, 10 |
| *referenced-lines count* (text) | `LandscapePanel` | LandscapePanel.cpp:128 | display | references exist | none | C | 5, 7 |
| Reference line display (header): per line Visible, Locked, Width (pixels), Line colour | `ReferenceLinesPanel` | LandscapePanel.cpp:13 | `SetReferenceLineStyle` | not Busy or gesture | action: EditBoolean.EditorOwnedReferenceLinesSubtractIntoAHoleAndKeepCoherentSources | C (the selected line only); the full list in D | 7 |
| Region settings: elevation sliders (ID `region-surface`), Elevation, Slope, Fall side, Use two selected vertices, Terrain interaction, Ground transition | `RegionPanel (s)` | Regions.cpp:217 | `SetRegionSurface` and the region's object settings, as slider gestures | not Busy, gizmo lock or drawing; per-mode conditions | HudTerrainSetup.RegionElevationSliderDragsWithoutTypingAndCreatesOneUndoEntry, HudTerrainSetup.RegionSurfacePanelNamesContourDirectionAndHidesForReferenceSelection | C | 5 |
| Recipe rows: ##up / ##down, Footprint on, Effect, *row select*, Footprint (tree) | `RecipePanel` (`Controls`, `Row`) | Recipe.cpp:42-140 | `MoveRecipe` (arrows), `SetLandscapeOperationEnabled`, the object's effect, selection | not Busy; arrows need a legal move | HudVirtualEditorRecipe suite | D (Recipe) | - |
| Build terrain | `RecipePanel` (`Quantities`) | Recipe.cpp:176 | `RequestTerrainBuild` | as Build mesh | HudVirtualEditorRecipe suite | removed in U5 (Decided 4) | - |
| Copy table, *quantities table* (`##quantities`), *row select* | `RecipePanel` (`Quantities`) | Recipe.cpp:187-222 | clipboard / selection | always | HudVirtualEditorRecipe suite | D (Recipe) | - |
| *Diagnostics* (text) | `RecipePanel` | Recipe.cpp:298 | display | diagnostics exist | HudVirtualEditorRecipe suite | D (Issues) | - |
| Footprint steps (authored order) (header), Footprint on per step | `LandscapePanel` | LandscapePanel.cpp:155-164 | `SetLandscapeOperationEnabled` | no landscape command | action: EditLandscape.DisableAndDeleteRetainConstructionHistory | D (Recipe, More) | - |
| Materials tab: Material preset metadata, Choose Archicad fill / surface..., Refresh project materials | `RegionPanel (s, true)` | Regions.cpp:343-354 | the material spec, `MaterialPickerRequest`, catalog refresh | as region settings | HudTerrainSetup.RegionSurfacePanelNamesContourDirectionAndHidesForReferenceSelection | C (Material heading) | 5, 6 |
| *Warnings* (text) | `LandscapePanel` | LandscapePanel.cpp:187 | display | n/a | none | D (Issues) | - |

## After #191 U3

The toolbar (B) draws Iconoir glyphs (`IconId` 25-43) with the label and hotkey as the tooltip.
Actions and enable conditions are unchanged.

- **Row 1:** the nine tools, in the order Select, Polyline, Path, Curve, Rectangle, Polygon,
  Ellipse, Mirror, Trim, then `Bool` (Union, Subtract, Intersection in a popover; moved here from
  `LandscapePanel`, shown with a landscape). Test: `HudVirtualEditorCommandBar.ToolbarSelectsEveryTool191`
  presses each glyph; `ToolbarDisabledWhileCommandRuns191` checks ToolLock.
- **Row 2:** Window select glyph and `La` (Lasso; no Iconoir glyph fits), the Gizmo popover (its
  glyph is the current mode's), the Snap popover (`SnapControls (s, true)`, options drawn open), the
  Local/World toggle, the Plane popover (`PlaneControls`), the Contours popover.
- **Command bar (A):** Undo and Redo are glyphs (Ctrl+Z, Ctrl+Y); Done reads Exit.
- **Height:** `HudVirtualEditorCommandBar.PinnedHeaderFitsTheBudget191` keeps A + B within 120 px.
- Every tool and modifier is reached in at most two clicks: one for a glyph, two for a popover entry.

## Notes found while taking the inventory

- No test addresses `DuplicateOffsetControls`, `OffsetJoinControl` or `OffsetMiterLimitControl`.
  #191 says tests call these directly. A grep of `tests/cpp` finds no such call, so their
  rows say `none`. U4's `NoInventoryControlUnreachable` should cover them.
- The tool radios, gizmo modes, axes, contours and Undo/Redo are tested only through their
  actions and keys, never by pressing the control. U3's `ToolbarSelectsEveryTool` closes
  that gap.
- The Playground builds without the Rust kernel. `TerrainSetupPanel` there shows "Terrain
  preview requires the Rust geometry build" in place of the capture buttons, and no terrain
  build runs. So the Playground cannot reproduce a refusal by building. A fixture's
  `status.error` has to be supplied to the session (U2).
