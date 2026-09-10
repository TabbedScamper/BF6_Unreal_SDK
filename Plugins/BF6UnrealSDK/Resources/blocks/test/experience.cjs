// Host experience opens, map rotation and update snapshots in real Chromium.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {pathToFileURL}=require('node:url');
const {chromium}=require(process.env.BF6_PLAYWRIGHT||'playwright');
const root=path.resolve(__dirname,'..'),read=n=>JSON.parse(fs.readFileSync(path.join(root,'offline',n+'.json'),'utf8'));
const fixture=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
(async()=>{
 const browser=await chromium.launch({headless:true,executablePath:process.env.BF6_CHROMIUM,args:['--allow-file-access-from-files']});
 try{
  const p=await browser.newPage();const errors=[];p.on('pageerror',e=>errors.push(e.message));
  await p.goto(pathToFileURL(path.join(root,'editor.html')).href);await p.waitForFunction(()=>window.BF6UI?.ws);
  await p.evaluate(d=>BF6Blocks.recv(d),{op:'defs',definitions:read('definitions'),synthesized:read('definitions_synth'),toolbox:read('toolbox'),options:read('options'),tooltips:read('tooltips'),icons:read('icons'),helpUrls:read('help_urls')});
  await p.evaluate(()=>{window.messages=[];const chunks={};window.ue={bf6blocks:{msg:s=>{const m=JSON.parse(s);if(m.op!=='chunk'){messages.push(m);return;}const c=chunks[m.cid]||(chunks[m.cid]=[]);c[m.i]=m.part;if(c.filter(x=>x!==undefined).length===m.n){messages.push(JSON.parse(c.join('')));delete chunks[m.cid];}}}};});
  const settled=()=>p.waitForFunction(()=>!BF6UI.pendingProject&&!BF6UI.loadingDoc&&!BF6UI.applying&&!BF6Blocks.state.loading,null,{timeout:180000});
  const open=async(project,revision,json)=>{await p.evaluate(s=>BF6Blocks.recv(JSON.parse(s)),JSON.stringify({op:'projectWorkspace',project,revision,name:project,json}));await settled();};
  await open('experience-A','original',fixture);
  const count=await p.evaluate(()=>BF6UI.ws.getAllBlocks(false).length);assert.ok(count>5000);
  assert.equal(await p.evaluate(()=>messages.filter(m=>['replaceTop','deleteTop','variables'].includes(m.op)).length),0,'Opening published edits');
  const before=await p.evaluate(()=>{
   const b=BF6UI.ws.getTopBlocks(false).find(b=>b.type==='Text');b.setFieldValue('Unsaved author edit','TEXT');
   BF6UI.ws.setScale(.8);window.messages=[];return {id:b.id,x:BF6UI.ws.scrollX,y:BF6UI.ws.scrollY};
  });
  await open('experience-A','original',fixture);
  assert.deepEqual(await p.evaluate(id=>({text:BF6UI.ws.getBlockById(id).getFieldValue('TEXT'),x:BF6UI.ws.scrollX,y:BF6UI.ws.scrollY}),before.id),{text:'Unsaved author edit',x:before.x,y:before.y});
  const empty={blocks:{languageVersion:0,blocks:[]},variables:[]};
  await open('experience-B','empty',empty);
  assert.equal(await p.evaluate(()=>BF6UI.ws.getAllBlocks(false).length),0);
  const recovery=JSON.parse(await p.evaluate(()=>JSON.stringify(messages.find(m=>m.op==='projectRecovery'&&m.project==='experience-A'))));
  assert.ok(recovery?.json,'Switch lost the previous complete workspace');
  await open('experience-A','original',fixture);
  assert.equal(await p.evaluate(id=>BF6UI.ws.getBlockById(id).getFieldValue('TEXT'),before.id),'Unsaved author edit');
  // The native recovery copy must work even when browser storage is empty.
  await open('experience-B','empty',empty);
  await p.evaluate(()=>{BF6UI.projectWorkspaces.clear();sessionStorage.clear();messages=[];});
  await p.evaluate(s=>BF6Blocks.recv(JSON.parse(s)),JSON.stringify({op:'projectWorkspace',project:'experience-A',revision:'original',json:fixture,recovery}));await settled();
  assert.equal(await p.evaluate(id=>BF6UI.ws.getBlockById(id).getFieldValue('TEXT'),before.id),'Unsaved author edit');
  await p.evaluate(()=>{messages=[];BF6Blocks.recv({op:'prepareUpdate',token:'update-1'});});
  const checkpoint=JSON.parse(await p.evaluate(()=>JSON.stringify(messages.find(m=>m.op==='updateWorkspace'))));
  assert.ok(checkpoint.json&&!checkpoint.error);assert.equal(checkpoint.token,'update-1');
  assert.equal(await p.evaluate(s=>BF6Blocks.detectFormat(s).format,JSON.stringify(checkpoint)),'workspace','Update backup cannot be imported');
  // Queue a different experience while a large load is in progress.
  await p.evaluate(s=>{BF6Blocks.recv({op:'projectWorkspace',project:'experience-C',revision:'original',json:JSON.parse(s)});BF6Blocks.recv({op:'prepareUpdate',token:'busy'});BF6Blocks.recv({op:'projectWorkspace',project:'experience-D',revision:'empty',json:{blocks:{languageVersion:0,blocks:[]},variables:[]}});},JSON.stringify(fixture));
  await settled();assert.equal(await p.evaluate(()=>BF6UI.hostProject),'experience-D');
  assert.equal(await p.evaluate(()=>BF6UI.ws.getAllBlocks(false).length),0);
  assert.ok(await p.evaluate(()=>messages.some(m=>m.token==='busy'&&m.error&&!m.json)),'Busy update snapshot was accepted');
  assert.equal(await p.evaluate(()=>messages.filter(m=>['replaceTop','deleteTop','variables'].includes(m.op)).length),0,'Project switching published deletions');
  assert.deepEqual(errors,[]);console.log('Experience loading, rotation, queued switches, recovery and update snapshots passed ('+count+' blocks).');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
