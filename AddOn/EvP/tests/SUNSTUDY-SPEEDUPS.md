# Sun-study speedups (2026-10-03)

Five independently committed changes. Live Archicad acceptance and large-model
timings are deliberately deferred; offline timings are not production speed claims.
Analysis remains the model minus Context/Ignored, with Late filtering glass faces.

## 1. GPU fallback and readback

- Three bounded, independent 16,384-ray slots overlap dispatch/readback and CPU
  resolution, instead of waiting after each 4,096-ray dispatch.
- Only the active result range is copied. CPU checks compact into parallel packets;
  fallback/validation overlap traces once, not twice.
- Full first-4,096 validation per direction and sparse later checks remain.
  Every ambiguous/work-budget ray still uses exact CPU resolution; any mismatch
  disables the backend and replays the whole timestep. No precision downgrade.
- Telemetry distinguishes arithmetic ambiguity, work-limit exhaustion, unique CPU
  checks, bytes read, and in-flight packets. Work budgets/tolerances are unchanged.
- Validation: shader ABI, every-ray/binary-bit parity, georeferencing, boundaries,
  ring wrap/tails, concurrency, cancellation, and immutable context-buffer reuse.
  The offline D3D adapter reports Microsoft Basic Render Driver, not the live RTX.

## Deferred live acceptance

Compare `sun-gpu-step` CPU checks, GPU compute, wait, fallback reasons, and wall time
on the same large model/settings. Verify cancellation never publishes a partial day.
The existing 53 missing solid meshes must be investigated before claiming complete
model accuracy; these optimisations do not waive extraction diagnostics.
