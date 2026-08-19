# BF6 Unreal SDK

Build Battlefield 6 Portal maps inside Unreal Engine.

This is a community-made tool. It is not affiliated with EA or DICE. It recreates the Portal SDK editing workflow inside Unreal Engine 5, using the same low-poly assets, gameplay objects, and map data the official Godot-based SDK ships with, and exports maps in the same `.spatial.json` format the SDK's experience exporter packages for Portal.

## What it does

Open the project and the editor becomes a map builder:

- **Map selector**: a full-screen picker styled after the Portal site's choose-maps page. Every map shows its key art, name, and placeable object count. Click a map to open its base, or resume one of your saved projects on it.
- **Base setup**: every map loads with its shipped gameplay layout, including HQs, spawn points (with correct facings), deploy cameras, and the combat area volume, plus the low-poly terrain and asset meshes so you always know where you are.
- **Placement**: aim anywhere on the map and press **Space**. A radial menu opens with the object categories (Architecture, Props, Nature, Gameplay, and so on). Pick a category, search it, hover an object for a live 3D preview of its model, and double click to place it right where you aimed. All 9,700+ SDK low-poly models are supported.
- **Physics budget**: a live budget bar sits on top of the viewport, tracking the same per-map physics cost limit the SDK's memory tool enforces. It fills from blue to red as you build and can be hidden.
- **Gameplay editing**: select any gameplay object and press **Space** for its context radial. Edit the full attribute suite (HQ teams, MCOM settings, combat area timers, vehicle spawner types, and more, with real dropdowns from the SDK data), link spawn points and volumes with a pick mode, and edit zone polygons Godot-style: points appear on selection, drag to move, right click an edge to add.
- **Save / resume**: name your custom map, save it, and pick it back up later from the map selector. Sessions autosave every 60 seconds and the tool asks before you leave unsaved work.
- **Export**: writes your map to `<map>.spatial.json` in the SDK's Portal format, verified against shipped Portal experiences. Export with readable names, or minified ([PortalSpatialMinifier](https://github.com/dfanz0r/PortalSpatialMinifier)-style) so it fits Portal's upload size limits. Drop the file into an SDK mod folder and run the SDK's experience exporter to package it for Portal.
- **Import**: load any `.spatial.json` (your own exports or SDK samples) straight back into the editor as an editable project. The tool detects which map the file belongs to and opens it there.

## Requirements

- Unreal Engine 5.8
- Windows
- The latest [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe). If launch pops an error about a missing module or C++ redistributable, install this and relaunch; the copy Unreal's installer ships is often too old for this plugin.
- The official Battlefield 6 Portal SDK download (free from the Portal site), unzipped anywhere

You do not need Battlefield 6 installed to build maps.

## Install

Every [release](../../releases/latest) has two downloads. Which one you want:

| File | Who it's for |
| --- | --- |
| `BF6UnrealSDK_Project_*.zip` | **New users, start here.** The complete prebuilt project. No Visual Studio, no compiling. |
| `BF6UnrealSDK_Plugin_*.zip` | **Existing installs.** Just the plugin folder. The in-editor update button downloads and installs this one for you, so you rarely need it by hand. |

1. Install Unreal Engine 5.8 from the Epic Games Launcher.
2. Download the **Project** zip from the latest release and unzip it anywhere.
3. Open `BF6_High_Poly.uproject`.
4. On first launch the tool asks for your unzipped Portal SDK folder. Point it at the SDK and hit **Import SDK data**. The tool copies the catalogue, parses every map's base setup, and converts all 9,700+ low poly models and every map mesh by driving the SDK's own bundled Godot in the background, with live progress. This runs once and takes a while; grab a coffee.
5. Pick a map and build.

From then on, new versions install themselves: the tool checks this repo on launch and offers a one-click update.

**Building from source instead:** clone the repo and open `BF6_High_Poly.uproject`; Unreal compiles the plugin on first open (requires Visual Studio 2022 with the C++ game development workload).

When a new SDK version drops, unzip it and run **SDK Setup** again from the map screen. The import skips everything already converted, so a re-sync only processes what changed.

## Workflow

1. Pick a map in the selector (or resume a save, or import a `.spatial.json`).
2. The base map is read-only. Type a name in the bottom right and hit **Create** to start your custom map.
3. Aim and press **Space** to place objects. Escape or the center of the radial cancels.
4. **Save** as you go. **Export** when you are ready for Portal.
5. Drop the exported `.spatial.json` into your SDK mod folder and package it with the SDK's experience exporter.

## Roadmap

- Model colors and materials, so the low poly models look as good as they do in the Godot SDK
- Drag objects straight from the library into the world
- A wide-screen hotkey build mode for faster placement
- High-poly overlay as a separate add-on plugin: full-detail meshes, materials, and extended terrain decoded from the game itself

## Testing and feedback

The tool is in open beta and every report helps.

- **Bugs**: open an [issue](../../issues). Include what you did, what happened, and the log from `Saved/Logs/BF6_High_Poly.log` (attach the file, or the last 50 lines). If the editor crashed, the crash folder under `Saved/Crashes` is gold.
- **Ideas and suggestions**: open an issue with the feature request template, or start a [discussion](../../discussions).
- **Updates**: the tool checks this repo's releases on launch. When a new version is out you can install it from inside the editor.

## Credits

Built and maintained by TabbedScamper.

Thanks to dfanz0r for PortalSpatialMinifier, which the exporter's minification mirrors.

Battlefield 6 and Battlefield Portal are trademarks of EA Digital Illusions CE AB. This project is not endorsed by or affiliated with EA or DICE.
