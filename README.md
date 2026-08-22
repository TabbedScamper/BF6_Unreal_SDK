# BF6 Unreal SDK

Build Battlefield 6 Portal maps inside Unreal Engine.

This is a community-made tool. It is not affiliated with EA or DICE. It recreates the Portal SDK editing workflow inside Unreal Engine 5, using the same low-poly assets, gameplay objects, and map data the official Godot-based SDK ships with, and exports maps in the same `.spatial.json` format the SDK's experience exporter packages for Portal.

## What it does

Open the project and the editor becomes a map builder:

- **Map selector**: a full-screen picker styled after the Portal site's choose-maps page. Every map shows its key art, name, and placeable object count. Click a map to open its base, or resume one of your saved projects on it.
- **Base setup**: every map loads with its shipped gameplay layout, including HQs, spawn points (with correct facings), deploy cameras, and the combat area volume, plus the low-poly terrain and asset meshes so you always know where you are.
- **Placement**: aim anywhere on the map and press **Space**. A radial menu opens with the object categories (Architecture, Props, Nature, Gameplay, and so on). Pick a category, search it, hover an object for a live 3D preview of its model, and double click to place it right where you aimed. All 9,700+ SDK low-poly models are supported.
- **Object library**: a slide-up library of every placeable as cards with generated 3D thumbnails. Search it, reorganize objects into your own categories, drag a card straight into the world (the real model rides your cursor across the terrain, and the library steps aside until you drop), or click one for an orbitable preview (drag to spin, scroll to zoom) before you commit.
- **Godot-style hands**: click an object to select it and drag to move it in the same motion, rubber-band select on empty ground, Ctrl to snap, and **PICK PLACE** to carry any selection on your cursor and click it down. Alt+Arrows duplicates flush against itself for fast walls and rows, and **MULTIPLY** builds rows, grids, and circles from any object.
- **Scatter**: a live editor like Godot's Proton Scatter. The scatter appears instantly and re-forms as you drag sliders for count, radius, rotation, wobble (with fine-tune X/Y lean), elevation, and size variation, each copy rolling its own values. Fill a circle, square, or ring, or draw any outline right on the terrain with editable corner points. One undo removes the whole scatter.
- **Mode setup wizard**: guided Conquest and Breakthrough scaffolding. Point and click through numbered steps and every click places a fully linked bundle (HQ with spawns, flags with capture areas, sectors wired up) with convention object IDs, then the checks run automatically.
- **Validate**: a lint pass that catches unlinked spawners, broken zone windings (with a one-click fix), oversized areas, duplicate object IDs, objects the current SDK release does not list for your map, and upload-size problems before the Portal site does. Anything it flags gets a warning badge in the scene tree, so you can read the problem next to the object.
- **Cameras**: select any deploy or fixed camera for a live picture-in-picture of exactly what it sees, updating as you move it. **SET CAMERA** hands it your current editor view, Look through jumps you into its view, and right-click Pilot flies the camera directly with a real Unreal camera frustum showing its aim.
- **Blocks**: save any selection as a reusable block. Place copies anywhere, and blocks are single files you can share with anyone. Double-click a placed copy to edit inside it; press Enter and every copy on the map updates to match, or Esc to revert the whole edit.
- **Group editing**: double-click any group to tab into it. Everything outside the group turns transparent and unclickable, so you only ever touch what you meant to. Enter keeps the changes, Esc puts everything back.
- **Budget bar**: a live bar on top of the viewport tracks the SDK's per-map physics cost limit AND the Portal site's real upload limits, measured against the live service (3 MB per map spatial, with the whole-experience cap shown too), including your map's minified versus raw size.
- **Gameplay editing**: select any gameplay object and press **Space** for its context radial. Edit the full attribute suite (HQ teams, MCOM settings, combat area timers, vehicle spawner types, and more, with real dropdowns from the SDK data) right inside the menu, and edit zone polygons Godot-style: orange point handles appear on selection, drag to move, Ctrl+click an edge to add, Del to remove.
- **Assign mode**: when a field links objects together (spawn points to an HQ, volumes to a combat area), everything that can't be assigned fades out, valid targets get color-coded markers with lines showing what's already assigned, and clicking the markers picks them. Enter confirms and drops you back in the attributes menu.
- **SDK hints**: a toggle that explains SDK concepts on hover, so new builders learn teams, spawns, and volumes without leaving the editor.
- **Walk the map**: drop out of the camera and onto the ground at soldier eye height to judge scale, cover and sightlines. Run, crouch, jump, step over kerbs, fall off roofs - and keep building while you are down there, with the build menu on F and placement on left click. Stand up and the camera is where you left it.
- **Camera your way**: standard Unreal navigation, or switch on the Godot-style camera (middle mouse orbit, Shift+middle mouse pan) if that is what your hands know. A pinned panel in the corner always shows the controls for whatever you are doing.
- **Save / resume**: name your custom map, save it, and pick it back up later from the map selector. Sessions autosave every 60 seconds, blocks and groups survive reloads, saves live in one folder per custom map (easy to back up or share), and the resume list can delete saves you are done with.
- **Godot scene tree**: the outliner is Godot's, icons and all. Parent to an empty node or to another object exactly as you would in the official SDK, and a node stands in for its whole branch - colour it, check its collision, carry it, scatter from it, save it as a block. Imported `.tscn` scenes keep the tree you authored, and Validate's warnings show as badges beside the objects they belong to.
- **Export**: writes your map to `<map>.spatial.json` in the SDK's Portal format, verified against shipped Portal experiences. Export with readable names, or minified ([PortalSpatialMinifier](https://github.com/dfanz0r/PortalSpatialMinifier)-style) so it fits Portal's upload size limits. Drop the file into an SDK mod folder and run the SDK's experience exporter to package it for Portal.
- **Import**: one button takes both `.spatial.json` (your own exports or SDK samples) and Godot `.tscn` scenes, so projects built in the official SDK come straight across with transforms, links, zones, and spawns intact. The tool detects which map the file belongs to and opens it there.
- **Managed SDK**: the tool downloads the newest Portal SDK for you, straight from EA's official service, unpacks it in parallel with live progress, and converts everything automatically. When EA releases a new SDK it offers the update, and your maps and blocks carry over untouched.

## Why Unreal instead of the official SDK

This tool covers the official Godot SDK's workflow, and then adds what it never gave you:

- **Blocks**: reusable, shareable prefabs where editing one copy updates every copy on the map. The Godot SDK has nothing like it; repeated layouts mean repeated hand-placement.
- **A live scatter editor** with per-copy rotation, wobble, elevation, and size limits, preset and hand-drawn fill shapes, and single-undo apply.
- **A mode setup wizard** that builds fully linked Conquest and Breakthrough scaffolding click by click, then checks its own work.
- **Validate** that catches broken links, windings, duplicate IDs, and upload-size problems before an upload fails.
- **Real camera tooling**: live picture-in-picture previews, set-from-view, and flying a camera directly.
- **Tab-in editing** for groups and blocks, with the rest of the world ghosted out.
- **Assign mode** for linking objects, with fading, markers, and assignment lines instead of hunting through a node tree.
- **A visual object library** with thumbnails, search, drag-to-place with a live model preview, and orbitable previews, instead of a name list.
- **In-editor hints** that teach the SDK's concepts as you hover, and a controls panel that follows your selection and mode.
- **One-click self-updates** from inside the editor, and a managed SDK that downloads and updates itself the same way.
- **Import of existing experiences**, including minified ones and Godot `.tscn` projects, straight back to an editable state.

Beyond the feature list, Unreal is the industry-standard world editor: a faster viewport, better gizmos, dependable undo everywhere, and a tooling ecosystem the stock Godot SDK does not match. That foundation is what lets this project spend its time on creator features instead of basics, and it is where the planned high-poly add-on will render the real game assets instead of placeholder models. Same export, same Portal upload, a better place to build.

## Requirements

- Unreal Engine 5.8
- Windows
- Microsoft Visual C++ Redistributable x64, version 14.50 or newer. If launch warns that your redistributable is outdated or a module failed to load, run `Engine\Extras\Redist\en-us\vc_redist.x64.exe` from your UE 5.8 install folder (or grab [the same installer from Microsoft](https://aka.ms/vs/18/release/vc_redist.x64.exe)) and relaunch.
- About 10 GB of disk space for the Portal SDK the tool downloads and the converted data

You do not need Battlefield 6 installed to build maps, and you do not need to download the Portal SDK yourself - the tool fetches it for you.

## Install

Every [release](../../releases/latest) has two downloads. Which one you want:

| File | Who it's for |
| --- | --- |
| `BF6UnrealSDK_Project_*.zip` | **New users, start here.** The complete prebuilt project. No Visual Studio, no compiling. |
| `BF6UnrealSDK_Plugin_*.zip` | **Existing installs.** Just the plugin folder. The in-editor update button downloads and installs this one for you, so you rarely need it by hand. |

1. Install Unreal Engine 5.8 from the Epic Games Launcher.
2. Download the **Project** zip from the latest release and unzip it anywhere.
3. Open `BF6_Unreal_SDK.uproject`.
4. On first launch, click **Download the SDK for me**. The tool fetches the newest Portal SDK straight from EA's official download service (about 3 GB, resumable, with the community archive as backup), unpacks it in parallel, and converts all 9,700+ low poly models and every map mesh with multiple background workers, all with live progress and the install location shown on screen. This runs once; if the download will not work on your connection, the setup screen explains exactly where to drop a hand-unzipped SDK and verifies it before proceeding.
5. Pick a map and build.

From then on, new versions install themselves: the tool checks this repo on launch and offers a one-click update. When EA releases a new SDK the tool offers that update too, and only changed content reconverts - your maps and blocks always carry over.

**Building from source instead:** clone the repo and open `BF6_Unreal_SDK.uproject`; Unreal compiles the plugin on first open (requires Visual Studio 2022 with the C++ game development workload).

## Workflow

1. Pick a map in the selector (or resume a save, or import a `.spatial.json`).
2. The base map is read-only. Type a name in the bottom right and hit **Create** to start your custom map.
3. Aim and press **Space** to place objects. Escape or the center of the radial cancels.
4. **Save** as you go. **Export** when you are ready for Portal.
5. Drop the exported `.spatial.json` into your SDK mod folder and package it with the SDK's experience exporter.

## Roadmap

- Model colors and materials, so the low poly models look as good as they do in the Godot SDK
- A wide-screen hotkey build mode for faster placement
- High-poly overlay as a separate add-on plugin: full-detail meshes, materials, and extended terrain decoded from the game itself

## Testing and feedback

The tool is in open beta and every report helps.

- **Bugs**: open an [issue](../../issues). Include what you did, what happened, and the newest `.log` from `Saved/Logs` (attach the file, or the last 50 lines). If the editor crashed, the crash folder under `Saved/Crashes` is gold.
- **Ideas and suggestions**: open an issue with the feature request template, or start a [discussion](../../discussions).
- **Updates**: the tool checks this repo's releases on launch. When a new version is out you can install it from inside the editor.

## Credits

Built and maintained by TabbedScamper.

Thanks to dfanz0r for PortalSpatialMinifier, which the exporter's minification mirrors.

The scene tree uses icons from the Godot Engine, used under the MIT licence; the licence text ships with them in the plugin's `Resources/GodotIcons`.

Battlefield 6 and Battlefield Portal are trademarks of EA Digital Illusions CE AB. This project is not endorsed by or affiliated with EA or DICE.
