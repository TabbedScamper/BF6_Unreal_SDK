# BF6 Unreal SDK

Build Battlefield 6 Portal maps inside Unreal Engine.

This is a community-made tool. It is not affiliated with EA or DICE. It recreates the Portal SDK editing workflow inside Unreal Engine 5, using the same low-poly assets, gameplay objects, and map data the official Godot-based SDK ships with, and exports maps in the same `.spatial.json` format the SDK's experience exporter packages for Portal.

## What it does

Open the project and the editor becomes a map builder:

- **Map selector**: a full-screen picker styled after the Portal site's choose-maps page. Every map shows its key art, name, and placeable object count. Click a map to open its base, or resume one of your saved projects on it.
- **Base setup**: every map loads with its shipped gameplay layout, including HQs, spawn points (with correct facings), deploy cameras, and the combat area volume, plus the low-poly terrain and asset meshes so you always know where you are.
- **Placement**: aim anywhere on the map and press **Space**. A radial menu opens with the object categories (Architecture, Props, Nature, Gameplay, and so on). Pick a category, search it, hover an object for a live 3D preview of its model, and double click to place it right where you aimed. All 9,700+ SDK low-poly models are supported.
- **Physics budget**: a live budget bar sits on top of the viewport, tracking the same per-map physics cost limit the SDK's memory tool enforces. It fills from blue to red as you build and can be hidden.
- **Save / resume**: name your custom map, save it, and pick it back up later from the map selector.
- **Export**: writes your map to `<map>.spatial.json` in the SDK's Portal format, with [PortalSpatialMinifier](https://github.com/dfanz0r/PortalSpatialMinifier)-style name minification built in so it fits Portal's upload size limits. Drop the file into an SDK mod folder and run the SDK's experience exporter to package it for Portal.
- **Import**: load any `.spatial.json` (your own exports or SDK samples) straight back into the editor as an editable project. The tool detects which map the file belongs to and opens it there.

## Requirements

- Unreal Engine 5.8
- Windows
- The data packs (see below)

You do not need Battlefield 6 installed to build maps. You do need the official Portal SDK if you want to regenerate the data packs yourself.

## Setup

1. Clone the repo.
2. Add the data packs (the low-poly mesh libraries, about 7 GB total, too large for the repo):
   - `Plugins/BF6HighPoly/Source/ThirdParty/libbf6/data/objmodels/` : the SDK object models
   - `Plugins/BF6HighPoly/Source/ThirdParty/libbf6/data/mapmesh/` : per-map terrain and asset meshes

   These are generated from a local install of the official Portal SDK by the extraction pipeline, or grab them from a release when one is up.
3. Right click `BF6_High_Poly.uproject` and generate project files, then build, or just open the project and let it compile the plugin.
4. Open the project. The map selector greets you. Pick a map and build.

## Workflow

1. Pick a map in the selector (or resume a save, or import a `.spatial.json`).
2. The base map is read-only. Type a name in the bottom right and hit **Create** to start your custom map.
3. Aim and press **Space** to place objects. Escape or the center of the radial cancels.
4. **Save** as you go. **Export** when you are ready for Portal.
5. Drop the exported `.spatial.json` into your SDK mod folder and package it with the SDK's experience exporter.

## Roadmap

- Property panel for gameplay objects (spawner teams, objective IDs, volume points)
- Object links (HQ areas, spawn groups, capture zones)
- Volume editing
- SDK auto-update: point the tool at a new SDK version and it regenerates its data
- High-poly overlay as a separate add-on plugin: full-detail meshes, materials, and extended terrain decoded from the game itself

## Credits

Built and maintained by TabbedScamper.

Thanks to dfanz0r for PortalSpatialMinifier, which the exporter's minification mirrors.

Battlefield 6 and Battlefield Portal are trademarks of EA Digital Illusions CE AB. This project is not endorsed by or affiliated with EA or DICE.
