# BF6 Unreal SDK version history

## 0.8.0 (2026-09-09)

**High Poly, loadouts and weapon cards**

- Install High Poly from the map selector. The existing update check checks both repositories; matching SDK and add-on updates share the updater. NEW FEATURES includes both components' release notes.
- Select a loot or soldier spawner and press Space to open its loadout menu. Choose weapons, gadgets or throwables, and available weapon attachments. Soldier previews also offer character, outfit, faction and pose controls.
- Generate a weapon card from a configured loot spawner, then edit its layout in the UI builder. Native TypeScript export uses the weapon and its attachment package. Refreshing a card preserves an existing layout.
- Send the spawner to Blocks for a base-loot recipe or Script for a configured weapon helper. Preview choices are saved locally; selecting an attachment alone does not change Portal gameplay.
- Configured pickup helpers replace a confirmed pickup in its weapon slot with the attachment package. Modes can call the direct hook from their own pickup tracking, or arm the conservative watcher from a proximity, buy-station or progression rule. The watcher requires two distinct known weapons already equipped and refuses ambiguous inventory changes.
- The full Project download can start without the optional High Poly add-on installed.

**High Poly rendering and build completion**

- Glass uses colored transmittance instead of rendering opaque black. Smoke backdrop cards read their authored textures and transparency; the known packed rising-smoke sheet animates in the viewport.
- Corrected material lookup for placed police sedans, billboard advertisement textures, and exact-metadata vegetation rejected because its SDK proxy has a different shape.
- Vehicle previews assemble repeated wheels, tracks, cockpit and weapon parts at their supported mounts. Soldier eyes use separate iris and sclera textures.
- Build progress includes placed-object dressing, loadout previews, and final asset/shader compilation before reporting completion.
- UI sound reads wait for map geometry under the shared reader lock. Editor shutdown drains sound workers before releasing their native context.

**Current limits**

- Cold builds can still exceed ten seconds; performance depends on map size and hardware. This release does not establish a ten-second cold-load or universal 60 FPS guarantee.
- Vehicle coverage varies. An unresolved mount is omitted and reported as an incomplete preview; the Abrams driver-camera mount remains one known case.
- Smoke cards can still expose a sharp intersection when the camera flies through them. Full particle effects and authored animation timing are not reconstructed.
- The native TS-to-Blocks converter can change function-local state semantics across waits, recursion and overlapping invocations. Review its warnings and use the extended TypeScript target for those patterns.
- Generated pickup helpers and weapon cards have compile and simulation coverage; they still need playtesting in a published Portal mode. Existing ammunition is not preserved by the replacement helper.

**Battlefield installed through the EA App is found**

- The tool looked for the game at one hardcoded Steam path, so anybody who bought Battlefield the ordinary way had no install as far as it was concerned: every mesh read was unavailable and the log said there was no game install on a machine where the game was plainly there. It now checks what you chose, then what the High Poly add-on already found, then the EA App and Steam in their usual places, then every fixed drive. `BF6.Install.Status` says where it looked, and `BF6.Install.Set` points it somewhere else.

**The block editor looks like Portal before you have signed in**

- A fresh install fell back to a list of block shapes with no colours, no output checks and no real tooltips, so the editor worked and did not look like Portal until you had connected once. The last captured definitions and style now ship with the plugin, and a first run draws the real thing. Refreshed at release time with `BF6.Blocks.BakeOffline`.

**Work is harder to lose**

- A save that failed still let BUILD and PUSH carry on, so a locked or read-only file produced a build of the older contents on disk, pushed it, and reported success.
- A save acknowledgement arriving after you had typed more marked the newer text clean, so autosave skipped it and those edits existed only on screen.
- Changing project discarded unsaved edits made in the last two seconds. Switching now writes first, and refuses to switch if it cannot. It also covers the whole switch rather than just that first write: the editor is held read only from the moment the flush starts until the new project is open, so an edit typed while the switch is in flight cannot fall into the gap. Anything still unsaved when the old project closes is kept and offered back the next time you open that project, rather than disappearing.
- NEW FILE wrote a one line stub over whatever name you typed, so typing index.ts replaced the project entry point. It decided a file was free whenever it could not be read, which is the opposite of what a read failure means. It now settles the question from the project's own file listing, opens the existing file when there is one, and writes nothing at all if it cannot get a listing to check against.
- Opening a file that already had unsaved changes pushed the copy from disk into it and lost what you had typed. An open file with unsaved work is now just brought to the front. PULL asks first before replacing unsaved text with the copy from Portal.
- The atomic file write deleted the destination before moving the replacement into place. It steps the old one aside now and puts it back if anything fails.
- Saving a map wrote straight over the previous save, so a write that died halfway, a full disk, a file locked by a sync client, or the editor going down at the wrong moment left a truncated file where a good save used to be. The new copy is written beside it, read back to prove it arrived whole, and only then replaces the old one.
- Project snapshots now cover everything you authored, not a short list: maps, spatial exports, blocks, UI designs, settings, source and the template's own configuration. Restoring checks every file it writes and will not delete anything unless it has a verified copy of both what it is restoring and what is already there. A restore that could not be completed says so instead of reporting the number of files it managed.
- UI designs are kept inside the project rather than only leaving through an export, so they are covered by those snapshots, and the designer keeps a recovery copy as you work. The recovery copy is offered back when it is newer; it never overwrites the design you actually saved.
- Exporting blocks as a script project overwrote handwritten files. It refuses the whole export rather than clobbering anything this tool did not write. It also no longer treats having generated a file once as permission to keep overwriting it: the record now holds a fingerprint of what was written, so a generated file you have since edited counts as yours and the export stops rather than discarding your changes. If the record itself cannot be written, that is reported, because the next export would otherwise be unable to tell the difference.
- A spatial export that failed to write still announced success and opened the folder.

**An SDK import that fell short says so**

- If some conversion workers failed while others succeeded, the import stopped when progress stalled, recorded the SDK version as fully converted, and told you it was complete. The tool then believed it had every model, quietly missing whichever ones those workers were carrying, and the next launch saw a current version and never offered to finish. The version is only stamped when everything converted now. A short run names what is missing and stays unstamped, so the next launch offers to finish it, and nothing already converted is done twice.

**Object ids stay in their band**

- With Respect bands on, a category whose band filled up carried on numbering past the end of it and into the next category's range. Capture points could be numbered into the HQ band, and the only symptom was a script addressing the wrong object during a match. Allocation now stops at the edge of the band and tells you which one ran out and what its range is, instead of quietly borrowing from a neighbour.
- Renumbering also works the whole assignment out before changing anything, so a band that runs out leaves every id exactly as it was rather than renumbering half the objects and stopping.

**Sending to the right place**

- Push, pull and save-on-site only checked that some Script page was open, never which experience. Pushing while browsing another experience overwrote that one. Both ids are compared now and a mismatch is refused.
- A build finishing after you switched project applied its results to whichever project was open. Runs carry the project they started in.
- Importing an experience joined attachment filenames straight into the project path. Names are contained and verified now, and failed writes are reported instead of counted as success.

**High Poly**

- Playing an SFX while previews were building crashed the editor. Two background paths raced the same unguarded catalogue mount inside the native core; they are serialised now.
- The quick select radial showed low poly previews while the object library showed high poly. The radial asks the same add-on for its picture.
- The add-on loaded the native core from a development path that exists on no installed copy of the plugin.
- Changing the game folder left the reader on the previous install while the cache pointed at the new one.

**The UI builder's exports actually build**

- Every export format failed to compile against the installed Portal template, all thirty combinations of format and template, while the builder's own tests passed. So the tool handed you files, told you to copy them into your project, and the project would not build. The TypeScript export imported a package called modlib that does not exist anywhere. The other two passed colours as plain arrays where the API takes an opaque vector type, set properties the widgets do not have, and looked the widget classes up on the wrong object. All thirty combinations compile now.
- Buttons that carry a label are built as a container button, because a plain button is a leaf and cannot hold one.
- Where a format genuinely cannot carry something you set, such as padding on a container or a hover colour, the export now says so instead of dropping it silently.

**Beginner recipes**

- Four of the eleven recipes did not compile against the installed template: wrong event names, a message passed where an enum was wanted, a widget that does not exist, and an invented enum member. All eleven typecheck now.
- The area recipe killed players who did what they were told: leaving the zone during the warning did not cancel the pending kill. Leaving and stepping back in was still wrong after that first fix, because the waiting handler only asked whether the player was inside: leave at two seconds, return at three, and the original timer woke at five and killed them two seconds into a grace period meant to be five. Each entry now carries its own visit number, so every visit gets the full warning and a handler waking late can tell it is stale.

**CHANGES: what this build can actually do**

- A new section on the build screen, between LOG and UI. The installed SDK, the Portal website and the installed game each have their own idea of what is available, and they disagree constantly: a function can be real in the engine, rejected by the website at upload, and missing from the typings all at once. CHANGES collects what each one says, keeps a record of it, and tells you what moved since the last time you looked.
- Every scan reports what it managed to look at, per section. This matters more than it sounds: a settings page whose dropdowns never opened, or a route the scan did not reach, makes everything in it look deleted. Anything missing from a section that was not fully read is reported as not observed, and only a section that was read completely twice can report that something is genuinely gone.
- Evidence is kept as a ladder rather than a yes or no. An identifier seen in a file, a parsed type, a control on a page, a value the server accepted and an effect measured in a running game are five different claims, and the tool never merges them into one.
- A watchlist tracks named candidates. `SetTickRate` and its `TickRates.Rate_60Hz` are real in the engine and refused by the site at upload, and the 60 Hz they offer only ever applied on a local host: a measured online host runs at exactly 30 Hz. The four archived water getters are listed as candidates with their signatures, and explicitly not as available, because nothing has tested them at runtime.
- COPY THE RUNTIME PROBE hands you a snippet that asks the engine which of those names exist. It calls none of them. Host a local match, then READ PROBE RESULTS reads the answers back out of the game log. The probe checks four functions it knows are real before reporting anything: if those four do not answer, it says so and records nothing, because a broken probe and a missing feature look identical otherwise.
- Also on the console: `BF6.Caps.Scan`, `BF6.Caps.Status`, `BF6.Caps.Probe`, `BF6.Caps.ReadProbe`, and over MCP for release checks.

**Updating, more carefully**

- The update check only ever looked at the tool. It fetched the tool's latest release, compared it with the tool's own version, and said you were up to date on that alone, so a High Poly add-on a version behind was invisible. That is the one thing a paired release cannot afford, because the two are built together. The check now looks at everything you actually have installed, says which of them are behind, and applies them in one go: both, or neither. An add-on-only update is a normal outcome rather than a case nobody thought about.
- The tool and the add-on cannot be left on different versions. Checking each against its own latest release was enough to notice an add-on that had fallen behind and not enough to guarantee the result: if one lookup failed, or the two were not published at the same version yet, one half updated and the pair no longer matched, which is the exact thing the version numbers exist to prevent. The whole plan is checked before anything is offered, and installing the add-on for the first time refuses a version that does not match the tool rather than fetching whatever is newest.
- A downloaded package must carry every module its own descriptor declares, not merely some binary. A package holding a descriptor and one unrelated file used to pass, and produced a plugin that failed at load time after replacing a working install.
- A rollback that could not remove a failed fresh install said the rollback was clean. It now says the folder is still there, that it is incomplete, and to delete it before starting the editor.
- A downloaded package is now checked before anything is overwritten, not after. It has to carry the descriptor for the plugin it claims to be, name the version you agreed to install, and bring at least one binary. A truncated download, an error page saved as a zip, or the wrong release's file used to get as far as replacing your install before anyone looked, and the version was only read back once it was too late to matter.
- An update saves your map and project before it closes the editor, and cancels if that does not succeed. The Blocks, Script and UI panels keep their work inside the page and cannot be flushed from outside, so the confirmation says so and asks you to save them yourself rather than implying a guarantee it cannot give.

- The rollback that runs when an update fails could delete files the update had not touched. It restored from the backup folder if one existed, without checking the backup was complete, and purged anything the destination had that the backup did not: a backup that failed halfway meant the recovery destroyed more than the failure did. It now records separately whether a folder was there before, whether its backup verified complete, and whether anything was actually written to it, and only the verified case is allowed to remove files. An incomplete backup is copied back without deleting anything, says plainly that the folder now holds a mix, and keeps the backup.
- A failed install of something that was not there before now removes only what it created.
- Every restore result is checked. The log used to say restored whether or not it was.
- Each update gets its own working folder. Two updates, or a dry run during a real one, used to share the same staging files, the same script and the same log, so one could quietly corrupt another. A second update while one is staged is now refused rather than started.
- After the restart, the report says what actually landed, per component. It used to check the tool's version alone, so a paired update where the add-on failed and the tool succeeded still said "Updated" and meant it about half the work. A partly applied update now names which half is missing and where its log is.

**Installing**

- The tool worked out where the High Poly add-on lived by assuming the layout of the folder it is developed in. Installed the ordinary way, with both packages unzipped side by side, it reported a running add-on as not installed and offered to download a second copy over the top. It asks the editor where the add-on actually is now, so any layout works, and the paired updater writes to the real locations.
- The add-on has its own icon in the plugin list rather than the blank placeholder.

**Testing**

- The block editor can audit itself, for overlapping controls, buttons wired to nothing, modules loaded but not installed, edit guards left switched off and a canvas with no size. `BF6.Blocks.SelfTest`, and over MCP as a release check.
- The pan self test measured the wrong pair of numbers, so a broken pan could still report a plausible result.
- The block editor's self test counted a check it could not run as a check that passed, so a run that could only look at eight things out of twelve still reported twelve passed. Skips are counted and reported separately now, and the number quoted is the number of checks that actually ran.
- The self test could never see its own result. The page answers after the tool has already returned, so it could only ever report that nothing had arrived. Asking for it and reading it back are two steps now, which is also why it does not simply wait: waiting happens on the editor's own thread and would freeze it.
- The game log Battlefield writes during a local session can be pulled or followed live from the Blocks and Script screens, and has its own LOG section. It had never worked: the folder name on disk carries permanent mojibake for the trademark sign, and the tool was building that name with a real one.

**Moving around the block canvas**

- Panning a scrolled canvas jumped to the edge and then refused to move. The gesture was feeding Blockly a scroll offset where it wanted a canvas position, and the first thing Blockly does with that number is clamp it, so on a large project every drag collapsed onto the same limit. Dragging now moves by the distance the mouse moved, at any zoom and anywhere in the workspace.
- Clicking the minimap put the target off centre by a factor of the zoom, correct only at 100%.
- The what-fits-here suggestions could open underneath the side panel, where they could not be read or clicked.

**Watching the game log from two places at once**

- Turning on Watch in the Blocks editor and in the LOG section left only one of them working, and stopping either one silently stopped the other while both still said they were watching. Each view now keeps its own subscription to a single reader: every view gets every line, and the log is only released when the last one closes.

**Smaller**

- Opening a map no longer reopens whichever editor you last had covering it. It remembers where the camera was instead.
- SHOW AS BLOCKS in the Script editor was never wired to anything.
- Comments on blocks came out below the statement they belonged to, comments on value blocks were dropped, and a block or a rule you had disabled still ran in the exported script.
- A build finishing while the Script editor was waiting on the tool could strand that request forever, leaving the buttons disabled with nothing to press. The build banner and the table of unanswered requests were accidentally the same variable. Requests are also now released rather than waiting indefinitely if an answer never arrives, and a request that timed out before it was ever sent is dropped rather than delivered later, after you were told it had not happened. A request that was sent and never answered now says its outcome is unknown, because that is the truth.
- Opening the Portal blocks page could reload it to re-read the block catalogue without checking whether anything on the page was unsaved. It now leaves a page with unsaved changes alone and asks you to save first.
- A damaged block definitions cache beat the copy shipped with the plugin, because only an empty file counted as missing. Anything unreadable is now set aside and the shipped copy is used, with a note saying so.
- Rebuilding the block icon cache wrote into the plugin's own installed files rather than into your project, which did nothing at all if the plugin sits somewhere read-only.
- An endpoint like `https://localhost.example.invalid` was labelled as staying on your machine because the address merely contained the word localhost. The host is parsed properly now, and anything that is not genuinely loopback is called remote.
- Asking for the errors of the last twenty minutes used the local clock against a log written in UTC, so away from UTC the window silently covered hours and old failures looked current.


## 0.7.4 (2026-09-04)

**The export says what the editor shows**

- Objects deleted from a base setup stayed in the export. The map's own default HQs were the visible case: gone from the editor, still in the JSON. The export now walks the live map, and anything the base shipped that is no longer there is left out and logged.
- Both of those HQs also came out on team 1. Attributes chosen from a list were written as the position in the list rather than the value, so a team read as a number and Portal took the first one. Selections now export their actual value, and an index with no matching option says so in the log instead of going out silently.
- A base setup that ships the same name more than once produced repeated ids, which collide on import. Repeats now get their own id, the way the editor already numbers duplicate labels.

**Reopening a saved map leaves it as you left it**

- Saving a Godot import, leaving, and coming back added a second copy of every node and renamed the originals. The tree metadata was rebuilt without checking what was already there. It now recognises the nodes already on the map and reuses them.

**Auto exposure is off**

- The viewport no longer rebalances its own brightness while you move. A dark corner stays dark and a bright one stays bright, so what you place is lit the way the map is lit.

**Under the tool**

- Portal SDK caches move out of the plugin's Source folder into Saved. An existing install moves its own data across on first launch, without duplicating the large packs.
- The core reader was resynced: the ocean sim, ground coverage, terrain bake, the static texture table, the livery hash and the shadow section filter all come across on the current ABI.
- Three editor log categories that emitted known, non-actionable notices are pinned to errors only. They were burying real project warnings. Nothing that is an actual error is hidden.
- Add-ons and automated benches can now open a map session through the tool's own loader, read and set the build viewport camera, read the map image state, and apply the validator fixes marked safe. These were the seams add-ons had to reach around before.

## 0.7.3 (2026-08-24)

**Attach: build the scene tree without touching the tree**

- Parent to node is now ATTACH, and it parents to anything loose, not just nodes. Pick the parent by clicking it in the world: the screen border says ATTACH - PICK THE PARENT while the mode is live, any loose object or node takes the selection, and Esc backs out. Objects inside a group or block are refused as parents, with the reason said out loud.
- A search bar in the ATTACH panel finds the parent by name across everything on the map that can take children - type a name and attach to it without flying there. Nodes rank first, the arrow flies you to a match without committing, and Enter attaches to the top match.
- DETACH TO MAP ROOT is a proper button now instead of a line of grey text.
- A Node can be placed straight from the object library, so the whole tree can be assembled in the viewport.
- Ctrl+Z undoes attaches and detaches - the whole family: click-the-parent, the panel list, the search, detach, and group-under-node.

**Coordinates in the attributes panel**

- Every selected object now shows its transform: position in metres, rotation in degrees, scale - X, Y and Z labelled in the gizmo's own colours. Values are relative to the parent, so putting a child 10 m above its node is typing 10 into Z. Objects with no other attributes, nodes included, still get the section.

**The map screen**

- Going back to < Maps now closes the open level, after the usual warning if there is unsaved progress. Nothing is held open in the background anymore: what the map screen shows is what is, and any map can be deleted from it. The Return to build button went with it.

**New features, not history**

- The History panel is now NEW FEATURES: the newest tool and SDK changes up front, with TOOL HISTORY and SDK HISTORY tabs behind them. Unread changes put a small orange dot on the button corner - the same language Battlefield uses for unlocks - and opening the panel clears it.

**Validate finds it, then shows you**

- The checks panel grew a severity filter: ALL, PROBLEMS, WARNINGS, ADVICE.
- Clicking a finding flies to it and ghosts everything else, so the one object with the problem is unmistakable. Closing the panel brings the world back.

**An action ends by saying so**

- Finishing an attach, a detach, a spawn run, or placing a bundle used to leave the old selection lit, which read as the action not having happened. Now the selection states the result: attaches and detaches clear it, a placed bundle comes in selected, an ended spawn run hands the selection back to its HQ, and grouping under a new node selects the node.
- Volume and path points no longer draw through the object library when it is pinned open.

## 0.7.2 (2026-08-23)

**Two things the tool was hiding**

- The DRAW scatter shape is back on screen. It existed the whole time, but the shape row was wider than its panel and the last button clipped clean off the edge - which read as the feature not existing. The SHAPE label has its own line now and all five shapes fit: circle, square, ring, paint, and draw.
- A Scene button next to Maps, bottom left. Closing the Scene panel by its tab used to leave no visible way back; now one press reopens it, and pressing it while the panel is already open just brings it forward.

## 0.7.1 (2026-08-23)

**The map image**

- The official top-down map picture, draped over the low-poly terrain and assets - the Godot SDK's terrain decal, matched. Toggle it from the Display/Sun panel; the first press downloads the tile for the current map and caches it, and if your SDK install already has tiles from Godot's own button, those are used without downloading anything.
- It lands on the map context only. Placed objects never catch it, so a road stripe cannot paint itself across the roof of something you built.
- It is a real actor named after the map. Select it and shift it with the gizmo if it needs realigning, and the shift is remembered for that map.

## 0.7.0 (2026-08-23)

**Two-way Godot**

- Save as .tscn. The new button beside Save writes your whole map as a Godot scene the official Portal SDK opens: every object, your scene tree as real node parenting, zones with their shapes, heights and colours, waypoint paths as real curves, and every link between objects. Drop the file in the SDK's User_Created levels folder and keep working there.
- The round trip is verified against a real 4,000-object creator map: all 3,926 placed objects, all 166 links, every zone shape and elevation came back identical, and the file opens clean in Godot.
- Object references in the file point at the scenes your own SDK install really contains, scanned from disk rather than guessed, so nothing arrives as a missing dependency.

**Waypoint paths**

- The third thing you can draw, after the two volume types. Place an AI_WaypointPath and it comes with a patrol route: drag the points like a zone's, Ctrl+LMB adds one, Ctrl+RMB removes one, and the arrows on the ribbon show which way the AI walks.
- LOOP / OPEN on the path's wheel joins the ends or opens them.
- Paths import from .tscn and .spatial.json files, survive save and reload, and export both ways. Imported maps used to lose their patrol routes silently; they arrive whole now.

**Objects that own other objects**

- The HQ treatment now covers every type with links: capture points, sectors, combat areas, player spawners, ring of fire and the rest. Select one, press Space, and the wheel offers exactly what it needs - make and link its areas, place and link its spawns or flags or MCOMs - read from the SDK's own schema, so a type a future SDK adds gets the same treatment with no update.
- Placing linked objects follows the layout finished Portal maps use: the created area or spawn is parented under its owner in the scene tree, so the tree reads as the assembly it is.
- A capture point's spawn arrays name their team, so a spawn placed into the Team 2 list is a Team 2 spawn without you touching a field.
- Q and E turn whatever you are carrying, before you set it down. Shift steps by 5 degrees, Ctrl snaps to 15. Works in spawn runs too, so spawns face the way you mean.
- Escape cancels a spawn run and takes back everything the run placed. Enter keeps them. The highlight on already-linked spawns cleans up after itself instead of permanently recolouring them.

**Mode setup**

- Drop one finished piece without running a whole mode: HQ for either team, a flag with its capture area and both team spawn sets, a sector with its area, an MCOM. Fully linked, parented, numbered in the id ranges community scripts expect, and one Ctrl+Z removes the whole piece.
- The wizard explains itself properly now: what each piece is for and what a good placement looks like, not just "aim and click". Esc and Ctrl+Z are spelled out on every step, and Esc was always available - now you can see it.

**Zones look like the SDK**

- A zone's colour renders at the same density it has in Godot. Imported colours looked nearly twice as dark here because our walls blend both faces; the display now compensates, and the stored colour stays exactly what the creator picked.
- White edge lines on every zone - bottom, top and each vertical crease, the same three Godot draws - so the shape reads instead of fogging.
- Zone transparency defaults match the SDK's collision colour.

**The read-only base map**

- Trying to edit a base map answers you now: the screen edge pulses amber and a shine runs around the controls panel, which lists what actually works there and how to start building. The object library hides its shelf, since nothing on it can be placed yet.
- The controls panel stops claiming you can place objects on a map that refuses it.

**Fixes**

- Pressing Enter after typing a custom map name creates it. Both name boxes.
- A map name the filesystem cannot take is refused with the reason, instead of reporting a save that never happened. Creating over an existing name asks before replacing it.
- You can delete the map you have open. The confirm says what that means, and the view drops back to the base map.
- Deleting objects cleans the links that pointed at them, in both delete paths, and one undo brings back objects and links together.
- Reloading a save no longer breaks links when the editor renames objects to avoid name clashes with the base setup - the links follow the renames. This is why a re-exported map could lose its conquest HQ areas.
- Ctrl+Z now removes placed bundles and wizard steps. It never could: the transaction recorded the property edits and none of the spawned actors.
- Everything that spawns an object shows up in the scene tree, watched at the engine level rather than fixed site by site.
- Letting go of right click after flying no longer opens a context menu over what you were lining up, and no longer leaves the camera stuck to the pointer.
- The collision overlay sits a millimetre off the surface instead of ten centimetres.
- Old community maps that built zones from collision shapes import as real volumes instead of disappearing.
- Validate learned the SDK converter's own hard rules: an oversized combat volume and a ring of fire missing its shapes are called out here instead of at upload.

## 0.6.0 (2026-08-22)

**The scene tree**

- The outliner is a Godot scene tree now, icons and all. Parenting works the way it does in Godot: parent to an empty node, or to another object. Folders are gone as the way you organise a map, so what you learned in the official SDK carries straight over.
- Add a node anywhere in the tree, and it stands in for everything under it. Select the node and Colorize, the collision overlay, PICK PLACE, MULTIPLY, scatter, save-as-block, grouping, moving and deleting all act on its whole branch, not just the marker.
- Imported `.tscn` scenes arrive with the tree exactly as you authored it: empty nodes included, in the order the file lists them, with the map's base setup parented under the map-name node in the SDK's own layout.
- Warning badges in the tree, driven by Validate. Hover a triangle to read what is wrong with that object, and the sweep refreshes as you work.
- Tree search understands what you type. It matches whole words first, then several words in any order, ranked by how well they fit, so searching "node" finds your nodes and "car sedan" finds the sedans.

**Imports that arrive intact**

- Combat volumes stopped going missing on import. The `.tscn` reader matched attribute names loosely, so any line carrying `uid="` swallowed the property after it: about twenty-five nodes were dropped per scene, silently. That is why a map saved in Godot with its combat area linked could import with the link gone.
- Opening a level no longer duplicates nodes. Three of the six paths that clear the previous tree did not recognise node markers, so each open left another copy behind.
- Node markers are ignored by camera previews, so a node parked in front of a deploy camera no longer blocks the shot.

**Validate**

- Two checks were wrong and have been corrected. HQs are not required to sit outside the combat volume - measured against the shipped maps, five of sixteen sit inside it - and a combat area with no volume linked is now advice rather than an error, because the SDK's own CombatArea script treats that link as optional.
- New: an object that the current SDK release does not list for this map is flagged as advice, not an error. DICE moves objects between folders and map lists between releases and they usually still load, so the check tells you rather than failing you.
- New: non-uniform scale is called out, with the reason. Two objects in three hundred and eighty across the shipped maps are scaled unevenly, and collision does not follow the stretch.

**Walk the map**

- WALK in the radial puts you on the ground at soldier eye height, measured from the SDK's own soldier mesh, so a wall you placed reads at the height it will really be. WASD to move, Shift to run, Ctrl to crouch, Space to jump.
- Real ground contact: steps up to 45 cm are walked over, slopes up to 45 degrees are climbable, and nothing snaps you down - walk off a roof and you fall off it.
- Mouse look needs no button held and never traps the pointer at the edge of the screen.
- You can build from down there. F opens the build menu on foot, left click places or picks whatever you are looking at, and Esc or the FLY pill stands you back up with the camera exactly where you left it.
- Base maps you cannot edit still open the radial, offering WALK on its own, because walking a map is how you decide whether to build on it.

**Speed**

- Moving an object with the gizmo is instant. Unreal snapshots a transacted object three times over a drag, and our meshes carry their vertices as a property, so each snapshot copied the whole model - a flash on grab, then seconds of stall on release and again on undo. The vertex payload is now set aside for the length of the transaction and put back after, without touching the render state.
- Deleting a node was taking three seconds and is now immediate: the fast delete path did not recognise nodes, so they fell through to the stock one.
- Placement rays run against an index we build ourselves rather than cooked collision. Building it costs a fraction of a second where cooking cost seconds per map, and it returns surface normals, so objects sit flat on what they land on.

**Placing**

- Scatter reads the topmost surface. On maps with a plane under the terrain - Contaminated among them - both the drawn area and the objects were landing on the bottom-most one.
- Dropping objects onto your own placed objects survived the move to the new ray index: whatever you are carrying is excluded, so it cannot land on itself.

**The objects menu**

- Categories were rebuilt from the whole catalogue. Taking the top folder buried the library: 5,325 of the SDK's 11,142 types sat under Generic and another 1,339 under Uncategorized, so trees read as "Generic" and cars as "Props" or nothing at all. Shelves are now chosen from the most meaningful folder in the path and then from what DICE named the object. Generic is empty, Vehicles holds 615 types where it had none, and Nature holds 1,022 where it had 345.
- A search field sits at the hub of the objects wheel. Type two letters and the same panel a category opens appears, filtered across every shelf, with the cursor still in the box so you can keep typing.
- The shelves are paged, with the page numbers under the hub and FULL LIBRARY as a button above the search field, so the ring stays a readable size instead of growing across the screen.
- The hub reads BACK on any step-in menu and actually goes back one level.
- Opening the wheel no longer leaves the previous one stacked behind it, which had been quietly dimming the viewport a layer at a time.
- The mode banner sits below the budget bar instead of across it, in colours you can read.

**Cameras**

- HQs and flags carry their deploy camera in their attributes, and that camera now previews like any other: select the object and see exactly what it sees. Set from view writes your current view back into those attributes.
- A newly opened map looks at the middle of its combat area from above, at an angle, instead of starting at the origin. Saved maps still return to the view you left.

## 0.5.7 (2026-08-21)

- Colorize, the collision overlay and the assign-mode highlighting now work for everyone. They needed materials that only ever shipped inside the full project download, so updating from inside the editor never delivered them and those three features quietly did nothing. The materials live in the plugin itself now, so an in-editor update brings everything with it.

## 0.5.6 (2026-08-21)

- Maps open in about a third of the time. A 3,000 object map went from roughly ten seconds to under three. Models are read once instead of once per copy, naming no longer slows down as the map grows, and the map's own scenery appears immediately with its collision prepared a moment later. Reopening or resuming a map you already had open is faster again, because its scenery is left standing.
- You can place objects on top of your own placed objects. Drop a crate on a roof you built, a light on your gantry. Whatever is being carried is ignored, so it no longer lands on itself.
- Shift makes the camera fly faster, like Godot. Hold right mouse to fly, hold Shift to go fast, and let Shift go to carry straight on at normal speed. Releasing right mouse over an object no longer selects it by accident.
- The object library only steps aside once you actually move, so looking around without flying leaves it alone.

## 0.5.5 (2026-08-21)

- The object library gets out of your way while you fly. Hold right mouse to move around and the strip drops away; let go and it comes straight back, up if you had it pinned, hidden if you had it on auto-hide.

## 0.5.4 (2026-08-21)

- The space bar menu is five choices instead of seventeen: Objects, Mode Setup, Validate, Colorize, Collision. Every object category now lives one step inside Objects, with a Back wedge that always sits in the same place. Checks is renamed Validate and has taken in Object IDs, since duplicate and missing ids are the same question as the rest of the checks.
- Colorize, brought over from the Godot recolorizer. Colour by type gives every distinct object its own hue across the whole map, so repeated props and one-offs separate instantly, or paint a selection from the swatches. Colours are saved with your map and come back when you reopen it, and Clear selection or Clear all puts the real materials back. It is a view aid: nothing about it reaches your export.
- Collision overlay, brought over from the high-poly tool. The game scales collision evenly from the X axis, so an object you stretched still bumps as though it were square, and players walk through the part you added. Red shows what you actually hit. It opens on the stretched objects by default and tells you how many there are, or shows the selection straight from the object menu. It is a guide, not the game's real collision data.
- Object ids are treated as yours. Assigning ids now fills in blanks only and never renumbers an id you already set, because scripts address objects by those numbers. If everything selected already has one, it asks before renumbering. New ids also skip numbers already in use, so assigning can no longer create duplicates.
- Exports warn when a re-imported copy would drop an object id that the base setup had, instead of losing it silently.
- Buttons take effect immediately. Anything that changes what you see, colours, collision, assign-mode dimming, used to wait for you to move the camera before it appeared.

## 0.5.3 (2026-08-21)

- Updating from inside the editor works. It never actually started its own installer, so pressing Yes closed the editor, changed nothing, and offered the same update again on the next launch. If the installer cannot start now, the editor stays open and tells you exactly what to do instead of closing on you.
- Because the broken updater is in the version you already have, updating to this one needs one manual step. Run Fix-AutoUpdate.bat from this release, or press Yes as usual and then run apply_update.ps1 from your project's Saved/BF6UnrealSDK/update folder. After that, updates install themselves.
- Imported Godot scenes keep the tree you built. On import you are asked once whether to keep your folders exactly as authored or file everything by type, and that choice sticks. The outliner button flips between the two at any time, and your original tree is remembered either way, right down to props parented under other props.

## 0.5.2 (2026-08-21)

- Deleting is instant. Every delete path (Del, outliner, Edit menu) strips the heavy mesh data before the undo record, so removing a whole scattered forest takes milliseconds instead of seconds - and undo still brings everything back intact.
- Esc finally behaves: it closes any open popup, backs out of assign mode into the attributes menu, then deselects. Cancelling an assign hands the selection back to the object you were editing. Esc never steals from camera piloting or text boxes.
- You always know what mode you are in: the screen gets a Revit-style coloured frame and a top banner naming the mode and its exits - assign (green), block edit (blue), group edit, carrying, scatter, zone shape, and mode setup.
- Assign mode glows: everything assignable turns solid neon - cyan free, green assigned, orange picked - and the lines to the owner carry the same colours, so what is linked to what reads at a glance.
- Viewing a base map without a custom level now says so: an amber READ ONLY frame with pointers to the Create button or the map screen, plus a reminder if you click the map anyway.
- Dragging a zone's top dot to the floor snaps its height to 0 - infinite, the Season 4 rule - with a note the first time.
- Version history lives in the tool: a History button shows this changelog and the SDK's own version history, and every new SDK download generates a what-changed list (new maps, models, types, and script APIs) automatically.
- Exports from re-imported maps no longer produce duplicate object ids, which the Portal site rejected on upload.

## 0.5.0 (2026-08-21)

- The scatter editor: a live SCATTER pill in the Proton Scatter spirit. Sliders for count, radius, rotation, wobble (fine-tune X/Y lean), elevation, and size re-form the scatter in real time, every copy rolling its own values. Circle, square, ring, or hand-drawn fill shapes with draggable corner dots, edge inserts, and their own undo history. Enter keeps the whole scatter as one undo step.
- Godot hands: click-drag moves objects (groups whole, Ctrl snaps), rubber-band select on empty ground, PICK PLACE carries a selection on the cursor, Alt+Arrows duplicates flush, MULTIPLY builds rows, grids, and circles.
- Zones: drag the TOP dots to set height with live walls, RESET CENTER moves the origin to the middle, and clicking inside a big zone no longer selects it - only its walls do.
- Cameras: live picture-in-picture of what any camera sees, SET CAMERA from the editor view, Look through, and a native Unreal camera on every camera object for frustum lines and right-click Pilot. Deploy cameras import aimed correctly; fixed cameras use the game's real facing.
- Mode setup wizard: Conquest and Breakthrough scaffolding, click by click, fully linked with convention object IDs, checked automatically.
- Checks: lint for unlinked spawners, zone windings (one-click fix), duplicate object IDs, HQs inside the combat area, and upload sizes against the measured Portal limits, which the budget bar tracks alongside physics cost.
- The SDK installs itself: official EA download with resume, parallel unpacking with a real progress bar, parallel model conversion, and a verified manual fallback. Fixes the object-conversion failure fresh installs hit.
- Library drags show the real model riding the cursor, the library steps aside until the drop, drops land exactly under the release point, and a read-only base explains itself instead of swallowing the drop.
- Import takes Godot .tscn scenes as well as .spatial.json. Saves live one folder per custom map with delete in the resume list; session reloads keep blocks, groups, names, and links; the outliner files objects by role.

## 0.4.0 (2026-08-20)

- Focus editing: double-click a group or placed block to edit inside it with everything else ghosted and unselectable. Esc reverts everything, Enter keeps - and for a block, updates every placed copy on the map.
- Saving a block groups it immediately, and duplicated blocks are independent.
- Radial polish: fixed center hub, closer pills, attribute names sized to fit, and assign mode returns to the attributes menu on Esc or Enter.
- Orbitable object previews.

## 0.3.1 (2026-08-19)

- Zone walls stretch with their height, custom heights load from saves, and height 0 (infinite) draws at 5 m like Godot.
- Zone point editing survives Del and undo.

## 0.3.0 (2026-08-19)

- Godot-style controls with an F1 controls sheet.
- The Object Library: every placeable as a card with a generated 3D thumbnail, search, custom categories, and drag-to-place.
- Blocks: reusable, shareable prefabs.
- Group editing, and the BF6_Unreal_SDK rename.

## 0.2.x (2026-08-19)

- 0.2.4: the SDK import tells the truth - a conversion that produces nothing stops with a clear error instead of claiming success.
- 0.2.3: updates no longer silently fail on locked files; the apply retries, falls back, relaunches directly, and logs every step.
- 0.2.2: the in-editor updater shows every step.
- 0.2.1: the object catalogue loads on every machine (Battlefield 6 is not required).
- 0.2.0: the prebuilt project download - no Visual Studio, no compiling.

## 0.1.0 (2026-08-19)

- First testable build: Portal-styled map selector, base setups for all 25 maps, space bar radial placement with live model previews, physics budget bar, save and resume, and spatial.json import and export.
