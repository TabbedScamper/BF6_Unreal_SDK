const fs=require('fs'),assert=require('assert');
const path=require('path'); const root=path.resolve(__dirname,'../../blocks');
const B=require(root+'/test/style.js').loadBlockly(),BF6=require(root+'/editor.js');BF6.attach(B);
BF6.setStyle(JSON.parse(fs.readFileSync(root+'/site_style_default.json','utf8')));
BF6.installDefinitions(JSON.parse(fs.readFileSync(root+'/offline/definitions_synth.json','utf8')),'shipped');
const ui=require('../../uibuilder/lib/bf6ui.js');
const design=JSON.parse(fs.readFileSync(path.resolve(__dirname,'../weapon-card.design.json'),'utf8').replaceAll('$LOOT_KEY','loot_card_test').replaceAll('$LOOT_TITLE','M4A1'));
design.widgets[0].children.push({type:'WeaponImage',id:'configured_weapon',name:'configured_weapon',weapon:'Carbine_M4A1',attachments:['Scope_RO_S_125x'],anchor:'Center',size:[336,120]});
const ws=new B.Workspace(),recipe=ui.exportBlocks(ui.normalise(design));
const converted=BF6.fillPlaceholders(recipe,{variable:(name,type)=>ws.getVariableMap().createVariable(name,type).getId()});
assert.equal(converted.missing.length,0);
converted.json.variables=ws.getVariableMap().getAllVariables().map(v=>({id:v.getId(),name:v.name,type:v.type})); B.serialization.workspaces.load(converted.json,ws);
const image=ws.getAllBlocks(false).find(b=>b.type==='AddUIWeaponImage');assert(image,'weapon image block loads');
const imageInputs=image.inputList.filter(i=>i.connection && /^VALUE-[0-6]$/.test(i.name));assert.equal(imageInputs.length,7,'seven configured image arguments');
for(const i of imageInputs)assert(i.connection.targetBlock(),'connected argument '+i.name);
assert(ws.getAllBlocks(false).some(b=>b.type==='AddAttachmentToWeaponPackage'));
assert(ws.getAllBlocks(false).some(b=>b.type==='CreateNewWeaponPackage'));
assert.equal(ws.getVariableMap().getAllVariables().length,1);
const saved=B.serialization.workspaces.save(ws);const second=new B.Workspace();B.serialization.workspaces.load(saved,second);
assert.equal(second.getAllBlocks(false).length,ws.getAllBlocks(false).length);
console.log('PASS configured weapon card loads with 7 arguments, attachment package, one variable and complete round trip ('+ws.getAllBlocks(false).length+' blocks).');



