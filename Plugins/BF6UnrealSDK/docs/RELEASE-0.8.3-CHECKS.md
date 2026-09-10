# 0.8.3 release verification

Windows, Unreal Engine 5.8. SDK and High Poly are a matching release pair.

- Native editor target builds successfully. Both descriptors declare 0.8.3.
- Real Chromium regression checks pass for 5,088 and 10,176 blocks: imports,
  labels, culling, category colors, zoom, undo, export and replacement imports.
- Experience loading, map rotation, queued switches, complete project recovery
  and update snapshots pass with the 5,088-block Night Ops workspace. Project
  snapshot tests retain off-screen files and focused rules. Its original saved
  workspace hash remains unchanged.
- Category hue controls were exercised through the actual right-click menu and
  through Unreal's native preference bridge, including panel reload and unchanged
  program serialization. Help and toolbox colors follow the applied palette.
- Linked Portal refresh fetched the current 38-experience list without unlinking
  or navigating away from the open experience editor. The previously failing
  Custom Rush Template 4.0 copy imported six maps, 25 rules, 20 subroutines and
  76 variables, with zero unconvertible blocks. No mode was published to Portal.
- Capture tests cover list remount, navigation during a pending remount, fresh
  replay, exact typed-array request bytes and choosing Modify rather than Publish.
- The packaged base project passed its startup and clean-exit smoke test.
- Five native tests passed from the packaged pair using a real RHI: context
  recovery, authored mip backing, generated exact backing and GPU readback,
  compact corners and geometry metadata. Fifteen harness unit tests pass.
- Populated Low Poly and full High Poly workflows exited with code 0 without a
  debugger after correcting the script runner's shutdown lifecycle. High Poly
  included two Low Poly restoration cycles, export, undo, save and reopen.
  The original immediate-quit driver also crashed with the published 0.8.2 pair;
  that result was not accepted as evidence of a clean exit.
- ZIP checks verify CRCs, case-insensitive path uniqueness, matching descriptors,
  required resources, portable artwork and manifest SHA-256/size agreement.
  User Saved data, intermediate output and local investigation files are excluded.
- The map selector retains Install High Poly when the add-on is absent. The
  shared updater discovers each repository's plugin archive; NEW FEATURES reads
  the first version section from both packaged changelogs.

The full High Poly run still missed the 10-second opening and 6 GiB GPU budget.
Hidden-window tests do not certify 60 FPS. Memory remains above the 16 GB target.
The reported roughly 1.4 GiB saving is from one matched map comparison, with
smaller peak-memory savings. See [the measurement report](reports/memory-0.8.2-followup.md).
