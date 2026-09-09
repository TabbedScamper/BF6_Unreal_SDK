// Run with Node 24+: node Resources/loot/test/pickup.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { stripTypeScriptTypes } from 'node:module';
let inventory, actions, step, living;
globalThis.mod = {
    InventorySlots: { PrimaryWeapon: 0, SecondaryWeapon: 1 },
    SoldierStateBool: { IsAlive: 0 },
    IsPlayerValid: () => true, GetSoldierState: () => living, GetObjId: () => 7,
    HasEquipment: (_, item) => inventory.includes(item),
    CreateNewWeaponPackage: () => [], AddAttachmentToWeaponPackage: (a, p) => p.push(a),
    RemoveEquipment: (_, slot) => { actions.push(['remove', slot]); inventory[slot] = null; },
    AddEquipment: (_, item, pkg, slot) => { actions.push(['grant', slot, item, [...pkg]]); inventory[slot] = item; },
    Wait: async () => { const fn = step; step = null; if (fn) fn(); }
};
const source = stripTypeScriptTypes(fs.readFileSync(new URL('../pickup-runtime.ts', import.meta.url), 'utf8'));
const api = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
const preset = { item: 2, attachments: [11, 12] }, before = { primary: 0, secondary: 1 };
function reset() { inventory = [0, 1]; actions = []; living = true; step = null; api.cancelLootPickup(7); }
let passed = 0;
async function test(name, run) { reset(); await run(); ++passed; console.log('PASS ' + name); }
for (const slot of [0, 1]) await test('replaces only pickup slot ' + slot, async () => {
    step = () => { inventory[slot] = 2; };
    assert.equal(await api.watchLootPickup(7, before, preset, 0.2), true);
    assert.deepEqual(actions, [['remove', slot], ['grant', slot, 2, [11, 12]]]);
    assert.equal(inventory[1 - slot], 1 - slot);
});
await test('duplicate baseline is rejected', async () => { inventory = [0, 0]; assert.equal(await api.watchLootPickup(7, {primary: 0, secondary: 0}, preset), false); assert.deepEqual(actions, []); });
await test('unknown slot is rejected', async () => { assert.equal(await api.watchLootPickup(7, {primary: null, secondary: 1}, preset), false); assert.deepEqual(actions, []); });
await test('already owned weapon is not replaced', async () => { inventory = [2, 1]; assert.equal(await api.watchLootPickup(7, before, preset), false); assert.deepEqual(actions, []); });
await test('two simultaneous losses are ambiguous', async () => { step = () => { inventory = [2, 3]; }; assert.equal(await api.watchLootPickup(7, before, preset), false); assert.deepEqual(actions, []); });
await test('other inventory change invalidates baseline', async () => { step = () => { inventory = [3, 1]; }; assert.equal(await api.watchLootPickup(7, before, preset), false); assert.deepEqual(actions, []); });
await test('death cancels a pending pickup', async () => { step = () => { inventory[0] = 2; living = false; }; assert.equal(await api.watchLootPickup(7, before, preset), false); assert.deepEqual(actions, []); });
await test('explicit cancellation prevents replacement', async () => { step = () => { inventory[0] = 2; api.cancelLootPickup(7); }; assert.equal(await api.watchLootPickup(7, before, preset), false); assert.deepEqual(actions, []); });
await test('timeout leaves inventory intact', async () => { assert.equal(await api.watchLootPickup(7, before, preset, 0.2), false); assert.deepEqual(inventory, [0,1]); assert.deepEqual(actions, []); });
await test('direct hook rejects gadget slots', () => { inventory[0] = 2; assert.equal(api.replaceKnownPickup(7, 5, preset), false); assert.deepEqual(actions, []); });
console.log(`${passed} pickup tests passed`);
