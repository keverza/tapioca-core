# Probe Record: Plan Anchor

- ID: `PROBE-PUBLIC-PLAN-ANCHOR`
- Owning task: `public core / PUBLIC-PLAN-ANCHOR`
- State: active
- Creation date: 2026-08-14
- Review date: 2026-09-12
- Question: Do the wall anchors register exactly over Archicad's floor-plan lines through pan, zoom, curved walls, and floating callouts?
- Evidence plan: Compare straight and curved wall anchors in the plan overlay at multiple zooms and pans, checking registration, constant pixel width, arc direction, and callout visibility.
- Procedure: Bring a floor plan to the front, run `PlanAnchorProbe`, inspect the six numbered visual cases, then run the close action so the overlay and anchors are removed.
- Results: The wall-outline read is settled at six of six walls; the drawing/registration half remains open, including the final visual overlay report.
- Conclusion: Keep the probe active until the plan layer is visually closed against Archicad's own drawing.
- Disposition: Active probe. Retained in `Diagnostics/Probes/` until the plan registration report closes PUBLIC-PLAN-ANCHOR.
