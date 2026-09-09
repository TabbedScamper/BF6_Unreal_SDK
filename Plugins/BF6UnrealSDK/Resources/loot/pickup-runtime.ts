// Shared by generated loot presets. Import this module once per mode.
// Portal reports inventory membership, not which floor item was collected.
// Arm a watcher from your proximity/buy interaction with a known slot snapshot,
// or call replaceKnownPickup from an existing inventory tracker.
export type WeaponSlots = { primary: mod.Weapons | null; secondary: mod.Weapons | null };
export type LootPreset = { item: mod.Weapons; attachments: readonly mod.WeaponAttachments[] };
const active = new Map<number, object>();

export function weaponPackage(preset: LootPreset): mod.WeaponPackage {
    const pkg = mod.CreateNewWeaponPackage();
    for (const attachment of preset.attachments) mod.AddAttachmentToWeaponPackage(attachment, pkg);
    return pkg;
}

function alive(player: mod.Player): boolean {
    return mod.IsPlayerValid(player) && mod.GetSoldierState(player, mod.SoldierStateBool.IsAlive);
}

// The caller must have identified the slot of this pickup. Removal and grant
// deliberately share one turn so another pickup cannot occupy the empty slot
// while an awaited delay is running. No unrelated inventory slot is touched.
export function replaceKnownPickup(player: mod.Player, slot: mod.InventorySlots, preset: LootPreset): boolean {
    if (slot !== mod.InventorySlots.PrimaryWeapon && slot !== mod.InventorySlots.SecondaryWeapon) return false;
    if (!alive(player) || !mod.HasEquipment(player, preset.item)) return false;
    const pkg = weaponPackage(preset); // Validate/build before removing anything.
    mod.RemoveEquipment(player, slot);
    mod.AddEquipment(player, preset.item, pkg, slot);
    return true;
}

export function cancelLootPickup(player: mod.Player): void { active.delete(mod.GetObjId(player)); }

// Conservative convenience detector for replacement pickups. Both previous
// slots must be known and occupied. Existing ownership, duplicate weapons,
// empty slots and simultaneous losses need your mode's own inventory tracker.
// Call only while the player can collect this particular configured item.
export async function watchLootPickup(player: mod.Player, before: WeaponSlots,
    preset: LootPreset, seconds = 20): Promise<boolean> {
    if (!alive(player) || before.primary === null || before.secondary === null || before.primary === before.secondary
        || before.primary === preset.item || before.secondary === preset.item
        || mod.HasEquipment(player, preset.item)
        || !mod.HasEquipment(player, before.primary) || !mod.HasEquipment(player, before.secondary)) return false;
    const id = mod.GetObjId(player), token = {};
    active.set(id, token); // Re-arming cancels an older claim for this player.
    try {
        const ticks = Math.ceil(Math.min(60, Math.max(0, seconds)) / 0.1);
        for (let tick = 0; tick < ticks; tick++) {
            await mod.Wait(0.1);
            if (active.get(id) !== token || !alive(player)) return false;
            const primaryLost = !mod.HasEquipment(player, before.primary);
            const secondaryLost = !mod.HasEquipment(player, before.secondary);
            if (mod.HasEquipment(player, preset.item)) {
                if (primaryLost === secondaryLost) return false;
                return replaceKnownPickup(player, primaryLost ? mod.InventorySlots.PrimaryWeapon : mod.InventorySlots.SecondaryWeapon, preset);
            }
            // Another inventory change invalidates the baseline.
            if (primaryLost || secondaryLost) return false;
        }
        return false;
    } finally { if (active.get(id) === token) active.delete(id); }
}
