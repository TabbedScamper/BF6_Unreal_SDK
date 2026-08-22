# BF6 Unreal SDK version history

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
