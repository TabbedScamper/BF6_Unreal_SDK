# BF6 Unreal SDK - map of the tool

READ THIS FIRST when working on the plugin. It answers "where does this go" and
"what will bite me", which are the two questions that otherwise cost a session
each time.

Run `Tools\check.ps1` before saying anything is done. It compiles every file on
its own and lints the rules below. It does NOT need the editor closed.

## The shape

| File | Lines | What it is |
|---|---:|---|
| `BF6UnrealSDK.cpp` | ~13.3k | Engine + session logic. Spawning, save/load, import/export, volumes, scatter, blocks. Implements `BF6Api`. |
| `BF6BuildMode.cpp` | ~7.2k | All Slate. The pie menu, panels, map selector, attribute dock, overlays. Consumes `BF6Api`. |
| `BF6SdkImport.cpp` | ~490 | One-time setup: turns the user's unzipped Portal SDK into the tool's data packs by driving the SDK's own Godot headlessly. |
| `BF6Internal.h` | ~87 | Module-internal plumbing shared between the .cpp files. Not a public surface; keep it short. |
| `BF6BuildMode.h` | ~590 | **The seam.** ~340 declarations. Widgets never touch engine types; everything crosses here as `FString`/`int32`/flat structs. |
| `BF6Outliner.cpp` | ~580 | Scene tree: folder generation, selection, the docked attributes panel. |
| `BF6Extension.cpp` | ~150 | Add-on registry (`BF6SDKExtension.h`). How BF6HighPoly attaches. |
| `SBF6PreviewViewport.*` | ~225 | The model preview viewport. |
| `BF6MapManifest.h` | 36 | Level code, display name, thumbnail, size class, paid flag. |

Add-ons live in `Plugins/Add-Ons/`, currently just `BF6HighPoly`, and reach the
tool only through `Public/BF6SDKExtension.h`.

## The seam is the whole design

`BF6BuildMode.h` exists so Slate code cannot see `AActor`. Keep it that way: if
a widget needs something about an actor, add a flat accessor to `BF6Api` rather
than handing the widget an actor pointer. This is what makes the UI file
readable and the two files independently compilable.

## Laws that already cost a session

These are not style. Each one was a bug that took a round trip to find.

**Two context menus, not one.** The viewport builds
`LevelEditor.ActorContextMenu`; the outliner builds
`SceneOutliner.DefaultContextMenu`. Extending one and expecting both fails
silently. Both are fed from `BF6_BuildPortalSection`; add entries there.

**The tree refresh is automatic now, and there is one place it can break.**
An outliner row whose folder row does not exist yet is dropped and never comes
back, so a spawned object lands in the world and nowhere in the tree. This used
to be each caller's job and five batch spawners forgot it. `BF6_FileActor` now
calls `MarkSceneTreeDirty()`, every path that files an actor goes through
`BF6_FileActor`, and the mark coalesces to one refresh per frame however many
actors are spawned. Do not add manual refreshes after spawning. `check.ps1`
fails if that one line ever leaves `BF6_FileActor`.

**The editor caches a selection pivot.** Moving an actor in code does not move
the gizmo until the selection changes. Call `BF6_NotePivotMoved()`.

**`GCurrentLevelEditingViewportClient` is null while a Slate popup holds
focus**, so anything invoked from a menu must go through `BF6_ViewportToFly()`.

**Never recreate what you can move.** Scatter/paint reconcile preview actors by
transform instead of destroy-and-respawn. Clear a proc mesh with
`ClearAllMeshSections()` before destroying it or its whole vertex payload is
serialised into the undo buffer.

**`BF6BuildMode.cpp` has no `LOCTEXT_NAMESPACE`.** Use
`FText::FromString(TEXT("..."))`. `check.ps1` catches slips.

**Includes go in the ONE block at the top of the file.** Three of them used to
sit 4,900 lines down, which meant everything above them compiled only because a
unity blob had already pulled them in.

**Forward declarations go above the namespace, never inside it.** `class AActor*`
written inside `namespace BF6Api` declares `BF6Api::AActor`, a phantom type that
resolves correctly only by unity accident.

## Conventions

**Telling the user something happened.** `Notify()` for anything they need to
read and might act on. `BF6_MiniToast()` for confirmation of a thing they just
did. `FMessageDialog` only when an answer changes what happens next.
`UE_LOG(LogBF6, ...)` for us, never as the only signal to a user.

**Every control gets a hint.** `BF6_MakeHint(title, body)` on tool buttons and
panel rows; plain `.ToolTipText` only for one-line clarifications. Say what it
does and what it will not touch.

**User-facing text**: no em dashes, no emoji, no shouting headers. Sentence
case, plain words, and name the consequence.

**Mutation rides the undo system.** Anything that changes the map opens an
`FScopedTransaction` and calls `Modify()` on what it touches before touching it.
Zone shapes additionally mirror into actor tags, because the live loop registry
is not transactional.

## Splitting the monoliths

In progress. `BF6SdkImport.cpp` was the first cut and is the pattern to copy:
pick a section whose inbound dependencies are few (that one needed only
`g_pluginDir`, `Notify`, the log category and `BF6_SnapshotSdkHistory`), move
it, declare the crossings in `BF6Internal.h`, and drop `static` from exactly
those symbols and no others. Verify with `check.ps1`, then confirm nothing
outside still reaches for a symbol that stayed file-local.

Section banners (`// ====`) mark the remaining split lines. Watch the ordering:
a symbol used across the seam cannot keep internal linkage, and single-file
compiles do NOT link, so a clean check is necessary and not sufficient.

## Known soft spots

- `BF6UnrealSDK.cpp` and `BF6BuildMode.cpp` are still large.
- The map-open path refreshes the tree inside the context spawn, which runs
  BEFORE `BF6_LoadBaseSetup` and `LoadSession`. Masked today by the outliner
  rebuilding on level change; worth confirming rather than trusting.
- Some controls are still undocumented. The icon-only and single-glyph ones
  (pin, collapse, hide, delete, full library) now carry hints; most of what is
  left is `< BACK` and `CLOSE`, where a hint would be noise. The unlabelled
  icon buttons are still worth a pass.
- Undo was audited rather than inferred: every `FScopedTransaction` that mutates
  an EXISTING actor calls `Modify()` on it first. The raw transaction-to-Modify
  ratio looks alarming and means nothing, because a transaction that only spawns
  or destroys needs no `Modify()`.
