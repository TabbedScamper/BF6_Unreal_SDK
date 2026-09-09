# Spawner loadouts and weapon cards

Install matching SDK and High Poly 0.8.0 releases. In a custom map, select a loot or soldier spawner and press **Space**, or open the **LOADOUT** pill. Choose an item and any supported attachments. Soldier previews also offer character, outfit, faction and pose choices. Preview choices are saved with your local map and can be undone.

The default loot preview is an M4A1 aligned to the SDK marker. The menu offers only attachment choices that can be joined to both the installed game catalogue and the public Portal enum. An empty list means no supported mapping is available; it does not mean the game has no customization for that weapon.

## Make a card

Choose **Card** to open a reusable design in the UI builder. Its weapon image contains the weapon and attachment package. Edit size, position, text and visibility there. Refreshing an existing card preserves its layout. The editor canvas uses a placeholder for the weapon image; Portal renders the configured weapon through `AddUIWeaponImage`.

Use your mode's rules to decide when to show the card: proximity, a buy station, a scrolling gun list or another interaction. Generating a card does not create those gameplay rules automatically.

## Make the pickup use attachments

Portal's base loot spawn does not accept the attachment package. Choose **Script** to generate the preset and pickup helpers. The **Blocks** action creates the base-loot spawn recipe; configured pickup behavior uses TypeScript.

The generated helper provides two integration routes:

- Call the direct replacement hook after your own tracker confirms the picked-up weapon and its primary/secondary slot. It removes that weapon and grants the configured version in the same slot.
- Arm the conservative watcher from your own interaction rule. It requires two distinct known weapons already equipped, with the target weapon not already owned. It proceeds only when exactly one known old weapon disappears and the new weapon appears. It stops on death, cancellation or timeout, and refuses ambiguous changes.

Empty-slot pickups and duplicate weapons need the direct hook plus your own slot tracking. The helper does not preserve the old pickup's ammunition. Playtest the integration in your mode before publishing it.

Generated files carry ownership markers. If you edit a generated helper, refreshing opens it for review instead of overwriting your changes.

## Preview limits

These are editor previews of installed assets, not a live game simulation. Vehicle assemblies may report unsupported mounts. Skin or attachment availability depends on the game catalogue. The native TS-to-Blocks converter also has limitations around recursion and overlapping function calls across waits; use the extended TypeScript target for those patterns and review conversion warnings.
