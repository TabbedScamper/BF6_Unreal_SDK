'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const UI = require('../uibuilder/lib/bf6ui.js');
const raw = fs.readFileSync(path.join(__dirname, 'weapon-card.design.json'), 'utf8')
    .replaceAll('$LOOT_KEY', 'loot_test_2').replaceAll('$LOOT_TITLE', 'M4A1');
const design = UI.normalise(JSON.parse(raw));
const image = UI.findByName(design.widgets, 'loot_test_2_weapon');
assert.equal(image.type, 'WeaponImage');
image.attachments = ['Scope_A_P2_175x'];
const exported = UI.exportAll(design);
assert.ok(exported.typescript.includes('Scope_A_P2_175x'), 'card export retains configured attachments');
assert.deepEqual(UI.auditAll(design), [], 'default card fits every supported aspect ratio without HUD overlap');
console.log('Weapon card template and attachment export passed.');
