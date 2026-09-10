# Large block workspaces and appearance

Implemented against Blockly 10.3.0 and Unreal 5.8. Tested using Night Ops Breakthrough: 5,088 block models and 104 variables. The stress fixture duplicates the block graph with distinct block IDs and a horizontal offset, producing 10,176 blocks while retaining shared variables. It is a synthetic stress case, not a second published experience.

## Changes

- `viewport.js` maintains block bounds and prunes off-screen SVG subtrees. Parent bounds include connected children and following statements, so a visible tail cannot disappear when its parent is above the viewport. Rendering and dragging restore hidden drawing before measurement. Models remain available to editing, undo and serialization.
- `overview.js` offers an optional cached silhouette view below both 20% zoom and the creator's text cutoff. It is off by default because silhouettes do not reproduce all Portal artwork. Clicking a silhouette returns to an editable block. Setting labels to always visible disables this view at every zoom.
- More > Appearance controls text visibility distance and optional dark red variable colors. The native host persists these choices. The default text cutoff moved from 35% to 15% zoom. Icons remain visible when text is hidden.
- Field spacing offers Portal and Compact presets. Saved site layout measurements confirm 34-pixel editable fields and 16-pixel text in Portal. Compact reduces the fields to 28 pixels while preserving the font size and enough row height for icons. It changes renderer padding, not the document. Tests cover shorter text/variable fields, restoring Portal spacing, and unchanged serialization.
- Native SVG text replaces the default PNG label substitution, which could keep stale labels after field changes. Original Portal definitions, theme, artwork and CSS remain in use.
- Native browser painting requests 60 FPS instead of the engine browser widget's default 24. This is a requested maximum, not a performance guarantee.
- Import replaces the old workspace instead of appending to it. Local-file and website-workspace loads share the synchronization baseline and cannot emit initialization changes as edits back to Portal. The canvas stays unavailable until the import settles.
- Navigation lists and stale-object warnings avoid unnecessary rebuilding. Canvas panning uses pointer events, matching Blockly's input handling.
- The footer map selector uses an upward-opening scrollable popup with a 400-unit height limit. MAPS and SCENE have centered alignment.

## Appearance packaging defect

The native host returned early when there was no downloaded website mirror, bypassing the packaged icon/font resource. The packaged resource also contained absolute font paths from the packaging machine. A fresh or disposable installation therefore lost category icons, block artwork and fonts.

Pack format 5 embeds font sources and block images. A host with no mirror now reads this pack. A local mirror can still rebuild the pack after a website update. Existing version-4 caches are invalidated. The packaged resource contains no `file:///` references.

## Validation

Unreal Development Editor builds passed. The existing headless round-trip suite passed with no semantic differences, preserving all variables and block connections.

The native CEF check also saved an 8% text cutoff through the C++ bridge, reloaded the page, verified the restored value, then returned it to the 15% default. The running editor loaded bundled artwork without a local site mirror; BFText-Regular reported loaded. The Appearance menu was inspected with 5,088 blocks present.

`Resources/blocks/test/browser.cjs` passed with both fixture sizes, including a run using Chromium's 4x CPU slowdown. Checks cover initial labels before adding a block, longer field text edited off screen, rendered width after returning to view, variable-theme serialization, move undo/redo, real mouse panning, overview selection, icon preservation, always-visible labels, replacement imports, and no outgoing edit operations during workspace loading. Chunked bridge messages are reassembled before checking their operation; an autosave is not mistaken for a Portal mutation.

The slowed run completed imports in about 25 seconds for 5,088 blocks and 68 seconds for 10,176. This tests browser execution under CPU pressure; it does not emulate a Ryzen 2600, 16 GB of RAM or a low-end GPU. No 10-second large-script load guarantee is established.

Native Unreal CEF measurements used a 1914 x 1041 browser surface, full bundled artwork, 10,176 blocks, default text cutoff and overview disabled. Over 119 measured animation-frame intervals per scale:

| Zoom | Mean frame interval | 95th percentile |
| --- | ---: | ---: |
| 60% | 16.7 ms | 16.8 ms |
| 30% | 22.0 ms | 33.4 ms |
| 10% | 39.8 ms | 83.3 ms |
| 2.5% | 161.2 ms | 216.7 ms |

These are browser animation-frame scheduling intervals, not independently counted Slate presentations. Normal editing is substantially more responsive, but full-detail viewing of thousands of blocks at once still misses 60 FPS. Earlier faster overview measurements used missing artwork or a different text cutoff and must not be presented as measurements of the final full-detail default.

## Remaining limits and follow-up

### Close-up zoom correction, September 10

Chromium continued laying out text beneath hidden SVG groups whenever the canvas scale changed. In a 160-step zoom trace, JavaScript averaged 0.63 ms per step but layout consumed 8,076 ms in total. Replacing the SVG transform with a CSS transform did not improve it.

Off-screen blocks now park only their drawing nodes behind ordered placeholders. Block root groups and the entire connected transform hierarchy stay in the document. Artwork returns before rendering, editing, overview capture or entering the visible area. This keeps the real Portal text and paths, without using a zoom screenshot or changing the program. The equivalent trace's layout total fell to 189 ms.

Native Unreal CEF 128, 1914 x 1041 surface, full bundled artwork and no simplified overview, zoomed between approximately 80% and 192% in 160 steps:

| Blocks | Before mean / p95 | After mean / p95 |
| --- | ---: | ---: |
| 5,088 | 146.7 / 166.8 ms | 16.67 / 16.7 ms |
| 10,176 | 253.0 / 283.4 ms | 16.67 / 16.7 ms |

These are browser animation-frame intervals on the development machine, not independent display presentations or certification of minimum-spec hardware. This test concerns close-up zoom. It does not supersede the limitations of drawing an entire detailed workspace at once.

The 5k/10k real-browser suite also verifies cursor anchoring, unchanged coordinates for every block, attached transform roots, restoration of path and field nodes, unchanged serialization, off-screen edits, themes, spacing, undo/redo, mouse panning, optional overview picking and replacement imports.

### Other limits

- A consistent 60 FPS with every block fully detailed at every zoom remains unachieved. Further work should measure faithful cached drawing rather than silently removing styling.
- The map footer was inspected in a running editor with an 11-map rotation and stayed compact. Popup opening and map-switch interaction still need a successful native input test.
- A Blocks tab restored before an experience opens can initially receive no project workspace. Reloading the page after the save opens loads it correctly. A complete project-switch contract should preserve unsaved edits before replacing an already-open block workspace.
- Dragging connected stacks, reconnection, multiple simultaneous editor views and live Portal round trips need broader interactive coverage. Programmatic move undo and real canvas pan are covered; they are not substitutes for every drag gesture.

## Reproducing the browser checks

Install Playwright in a test environment, or set `BF6_PLAYWRIGHT` to an existing module path and `BF6_CHROMIUM` to a browser executable. Run:

```powershell
node Plugins/BF6UnrealSDK/Resources/blocks/test/browser.cjs path/to/night_ops_workspace.json path/to/results
```

Set `BF6_CPU_RATE=4` to repeat under CPU throttling. The test writes screenshots and JSON results to the specified result directory and does not connect to Portal.

For native testing, use a disposable Unreal project with `-bf6-offline`. That launch option prevents the Portal website browser from opening while keeping local editors available. Add `-cefdebug=9223` only for a local debugging session; it exposes the embedded browser to local debugging clients.
