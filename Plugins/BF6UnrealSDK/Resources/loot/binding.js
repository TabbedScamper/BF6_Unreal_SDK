/* Shared, deterministic spawner recipe. No engine or filesystem dependencies. */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.BF6LootBinding = factory();
}(typeof window !== 'undefined' ? window : this, function () {
  'use strict';
  function validate(b) {
    if (!b || !Number.isSafeInteger(b.objId) || b.objId < 1 || b.objId > 2147483647) throw Error('Assign a positive, unique loot spawner ObjId first.');
    if (!/^(Weapons|Gadgets|AmmoTypes|ArmorTypes)$/.test(b.enumType) || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(b.member)) throw Error('Choose a supported Portal item.');
    return b;
  }
  function key(b) { validate(b); return 'bf6-loot-' + b.objId; }
  function rule(b) {
    var id = key(b);
    return {type:'ruleBlock', id:id, data:JSON.stringify({owner:'bf6-loot', version:1, objId:b.objId}),
      extraState:{isOngoingEvent:false}, fields:{NAME:'Spawn loot ' + b.objId, EVENTTYPE:'OnGameModeStarted'},
      inputs:{ACTIONS:{block:{type:'SpawnLoot', id:id+'-spawn', inputs:{
        'VALUE-0':{block:{type:'GetLootSpawner', inputs:{'VALUE-0':{block:{type:'Number', fields:{NUM:b.objId}}}}}},
        'VALUE-1':{block:{type:b.enumType+'Item', fields:{'VALUE-0':b.enumType, 'VALUE-1':b.member}}}
      }}}}};
  }
  function source(b) {
    validate(b);
    return '// Spawner ' + b.objId + '. Generated from its Loadout menu.\n' +
      '// Call this function from your chosen event handler. The matching block recipe uses OnGameModeStarted.\n' +
      'export function spawnLoot' + b.objId + '(): void {\n' +
      '  mod.SpawnLoot(mod.GetLootSpawner(' + b.objId + '), mod.' + b.enumType + '.' + b.member + ');\n}\n';
  }
  // Modify only the item socket we own. Conditions, event choice, following
  // actions and user labels survive. A moved/deleted/replaced action is a conflict.
  function apply(ws, Blockly, b) {
    var id = key(b), root = ws.getBlockById(id), action;
    if (ws.getAllBlocks(false).some(function(x){ return /^bf6x_/.test(x.type); })) throw Error('This native recipe needs a native block workspace.');
    if (root) {
      var owner; try { owner=JSON.parse(root.data); } catch (_) {}
      action = ws.getBlockById(id+'-spawn');
      if (!owner || owner.owner !== 'bf6-loot' || owner.objId !== b.objId || !action || action.type !== 'SpawnLoot') throw Error('The linked spawn action was changed. Restore it or use your edited logic.');
      var ancestor=action; while(ancestor && ancestor!==root) ancestor=ancestor.getParent();
      if (!ancestor) throw Error('The linked spawn action was moved to a different rule.');
      var spawner=action.getInputTargetBlock('VALUE-0'), number=spawner && spawner.getInputTargetBlock('VALUE-0');
      var item=action.getInputTargetBlock('VALUE-1');
      if (!spawner || spawner.type!=='GetLootSpawner' || !number || number.type!=='Number' || Number(number.getFieldValue('NUM'))!==b.objId || !item || !/^(Weapons|Gadgets|AmmoTypes|ArmorTypes)Item$/.test(item.type)) throw Error('The linked action now uses custom inputs. Edit those inputs in Blocks.');
      Blockly.Events.setGroup(true);
      try {
        if(item.type===b.enumType+'Item') item.setFieldValue(b.member,'VALUE-1');
        else {
          var replacement=Blockly.serialization.blocks.append({type:b.enumType+'Item',fields:{'VALUE-0':b.enumType,'VALUE-1':b.member}},ws);
          item.dispose(false); action.getInput('VALUE-1').connection.connect(replacement.outputConnection);
        }
      } finally { Blockly.Events.setGroup(false); }
    } else {
      if (ws.getBlockById(id+'-spawn')) throw Error('An action with this binding already exists outside its original rule.');
      Blockly.Events.setGroup(true);
      try {
        root=Blockly.serialization.blocks.append(rule(b),ws);
        var mod=ws.getTopBlocks(false).find(function(x){return x.type==='modBlock';});
        if(mod) {
          var input=mod.getInput('RULES'), last=input.connection.targetBlock();
          while(last && last.getNextBlock()) last=last.getNextBlock();
          (last ? last.nextConnection : input.connection).connect(root.previousConnection);
        }
      } finally { Blockly.Events.setGroup(false); }
    }
    root.select(); return root;
  }
  return {validate:validate,key:key,rule:rule,source:source,apply:apply};
}));
