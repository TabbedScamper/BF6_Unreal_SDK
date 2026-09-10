# Memory report follow-up, September 10, 2026

Development changes against SDK 09947ff and High Poly ff7b9ff (released 0.8.2).
These measurements do not change the published 0.8.2 release.

## Changes retained in development

- Release the two hidden SDK map-context drawing buffers after High Poly takes
  over. Retain the actors, transforms, materials and independent placement-ray
  geometry. Reconstruct drawing sections from the SDK files when returning to
  Low Poly or manually unhiding the actor. Avoid restoration inside the map-closing
  broadcast. Failed background restoration retries are limited to once per ten seconds.
- Move exact generated 2D texture bytes into owned session backing files before
  GPU upload, then release permanent CPU bulk data. Keep full GPU resolution,
  filtering, color space and mip contents unchanged. If backing-file creation
  fails, retain the original bulk-data path. Backing files are deleted when the
  last owner releases them.
- Read the current High Poly game-install record as well as its legacy location.
  Include `Battlefield 6` directly under fixed-drive roots in automatic discovery.
  This corrects discovery on startup; it does not reopen an active reader when
  the install selection changes.
- Correct the test harness to accept both UTF-8 and UTF-16 experience exports.
  Add comparison switches and populated-map restoration checks.

## Measured memory

Fixture: MP_Isolated, `Night Ops Breakthrough copy`, High Poly build, default
rendering, 1536 MiB texture pool, fresh High Poly cache, 12-second flight. This
local fixture differs from the reporting user's scene with 63 placed High Poly
objects. Measurements cover the editor process tree. One matched control and
one initial candidate run are shown; these are observations, not statistical
confidence bounds.

| Measurement | Both changes disabled | Both enabled | Reduction |
| --- | ---: | ---: | ---: |
| Median private commit during flight | 25,291.4 MiB | 23,902.8 MiB | 1,388.6 MiB |
| Median summed working set during flight | 19,130.0 MiB | 17,705.3 MiB | 1,424.7 MiB |
| Peak job commit | 25,821.7 MiB | 25,314.5 MiB | 507.2 MiB |

Allocation logs independently identify 236.8 MiB terrain context, 893.3 MiB
asset context and 389.3 MiB generated texture bulk released. Allocator retention,
upload staging and other concurrent resources mean that totals do not translate
one-for-one into process counters. The savings arrive after the map is built;
build-peak memory improves less than steady memory.

The original released-binary baseline measured 25,557.4 MiB median private commit
and 26,141.4 MiB peak job commit, but its final export check stopped on the harness's
UTF-16 bug. It is not a full-workflow acceptance run.

## Validation and limits

- Both plugins build successfully with Unreal 5.8.
- Install discovery was exercised with a distinct validated fixture folder in
  the current High Poly record. The native status command selected that folder
  before normal install candidates, while leaving the active reader unchanged.
  The disposable record was restored after the check.
- Fifteen Python harness tests pass, including UTF-8, UTF-16 little-endian and
  UTF-16 big-endian exports.
- Five native tests passed with the real renderer: context-buffer recovery,
  authored mip backing, exact generated texture backing, compact corners and
  geometry metadata. The context test retains the same placement-ray hit across
  three release/restore cycles. Generated texture testing compares every lookup
  byte after reloading and after GPU readback, and verifies backing-file cleanup.
- The two-open integration run restored the real map's context during reopening:
  terrain in 0.348 seconds and assets in 1.022 seconds. Its initial restoration
  assertion used an editor API that excludes transient actors; the harness now
  enumerates all actors in the editor world for this check.
- A subsequent full integration run passed both populated-map detail switches,
  export, undo, save, reopen and clean exit. The switches restored both context
  sections at their original locations in 1.378 and 1.372 seconds, then released
  them again in High Poly. A debugger was attached for shutdown diagnosis.
- Initial complete workflows reached successful export, undo, save and reopen,
  then exited with a late access violation after Unreal closed its log. This
  occurred with both optimizations disabled too. It must not be labeled a clean
  stability pass or assumed to be unrelated without further evidence.
- The final run without an attached debugger again passed the editing and
  restoration checks but reproduced the late shutdown access violation. Its
  peak job commit was 24.60 GiB. The clean debugged run does not clear this
  failure: publication remains pending shutdown diagnosis.
- Hidden viewport runs do not certify FPS. Fresh High Poly cache does not mean
  cold OS cache or cold engine DDC. The 10-second opening target was not met.
- Remaining memory is still too high to certify the 16 GB hardware target.
- The original Night Ops Blocks workspace SHA-256 remained unchanged throughout
  these tests; all experience opens used disposable project copies.

## Suggestions not adopted wholesale

Terrain index and weight textures are lookup data. Lossy BC compression or ordinary
averaged index mips can select the wrong terrain material. This change preserves
them exactly. Generated texture arrays remain unchanged; GPU residency reduction
requires a separately validated format and sampling implementation.

Replacing all procedural geometry with runtime StaticMesh/Nanite is a larger
change, not an automatic CPU-memory fix. The local Unreal 5.8 fast mesh builder
forces CPU access for vertex buffers, and the full build path can retain source
MeshDescriptions. A replacement needs measured build peaks, placement and
selection behavior, and a valid CPU-data lifecycle before adoption.

The reported 18 GiB Nanite reservation is virtual address space, not evidence of
18 GiB committed RAM. It is not included as a potential saving.

The older 2.66 GB narrowing crash cannot be attributed without its stack. The
SDK mesh decoder's unchecked 32-bit lengths remain a separate hardening item.
Duplicated SDK geometry on disk is also separate from the steady RAM savings.

See [the test procedure](../LOW-SPEC-TESTING.md) for comparison controls and
restoration checks.
