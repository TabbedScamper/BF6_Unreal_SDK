/* The four save bugs, as executable checks against the real function bodies. */
const fs=require('fs');
const F='C:/Users/mwalt/Documents/Unreal Projects/BF6_High_Poly/Plugins/BF6UnrealSDK/Resources/script/editor.js';
const src=fs.readFileSync(F,'utf8');

function grab(name){
  const at=src.indexOf('function '+name+'(');
  if(at<0) throw new Error('no '+name);
  let d=0,i=src.indexOf('{',at);
  for(let j=i;j<src.length;j++){ if(src[j]==='{')d++; else if(src[j]==='}'){d--; if(!d) return src.slice(at,j+1);} }
  throw new Error('unbalanced '+name);
}
/* unresolvedOpen is the FIN-01 guard both save paths consult: while it is set,
 * the host's project is unknown and a rel path names a file in a project we can
 * only guess at. It is declared here rather than stubbed so the real
 * writesAreAllowed body is the one under test. */
const body=['var unresolvedOpen=null;',grab('writesAreAllowed'),
  grab('saveOne'),grab('saveAllDirty')].join('\n');

function makeEnv(opts){
  const env={ files:{}, project:{path:'P1'}, saidErr:[], scheduled:0, drew:0,
    call:(op,a)=>opts.write(op,a), say:(t,l)=>{ if(l==='e') env.saidErr.push(t); },
    drawTabs:()=>env.drew++, scheduleAutosave:()=>env.scheduled++,
    refreshStringKeys:()=>{}, $:()=>({textContent:''}) };
  const f=new Function('files','project','call','say','drawTabs','scheduleAutosave',
    'refreshStringKeys','$','Promise','Error',
    body+'; return {saveOne:saveOne, saveAllDirty:saveAllDirty, get project(){return project;},'
        +' hostProjectUnknown:function(p){unresolvedOpen=p?{path:"P2",from:"P1"}:null;}};');
  const api=f(env.files, env.project, env.call, env.say, env.drawTabs, env.scheduleAutosave,
              env.refreshStringKeys, env.$, Promise, Error);
  return {env, api};
}
const model=v=>({ v, getValue(){return this.v;} });
let pass=0, fail=0;
const ok=(n,c)=>{ if(c){pass++;console.log('  PASS  '+n);} else {fail++;console.log('  FAIL  '+n);} };

(async()=>{
  /* CORE-02: a failed write must reject, not resolve */
  {
    const {env,api}=makeEnv({write:()=>Promise.reject({why:'locked'})});
    env.files['a.ts']={model:model('x'),dirty:true};
    let rejected=false;
    await api.saveAllDirty(true).then(()=>{},()=>{rejected=true;});
    ok('CORE-02 a failed save rejects so build cannot follow', rejected);
    ok('CORE-02 the file stays dirty', env.files['a.ts'].dirty===true);
  }
  /* CORE-03: a late ack must not clean newer text */
  {
    let release;
    const {env,api}=makeEnv({write:()=>new Promise(r=>{release=r;})});
    const m=model('A');
    env.files['a.ts']={model:m,dirty:true};
    const p=api.saveOne('a.ts');
    m.v='B';                    // typed while the write was in flight
    release({});
    const r=await p;
    ok('CORE-03 newer text is NOT marked clean', env.files['a.ts'].dirty===true);
    ok('CORE-03 another save is scheduled', env.scheduled===1 && r.superseded===true);
  }
  /* and the ordinary case still cleans */
  {
    const {env,api}=makeEnv({write:()=>Promise.resolve({})});
    env.files['a.ts']={model:model('A'),dirty:true};
    await api.saveOne('a.ts');
    ok('unchanged text is marked clean', env.files['a.ts'].dirty===false);
  }

  /* FIN-01. A write is only meaningful against a project. While the tool has
   * not said which project it has open, a rel path names a file in a project we
   * would be guessing at, and the host resolves it against whatever it has.
   * That is how one project's text was written into another. Nothing may leave
   * here in that window, by either path. */
  {
    let calls=0;
    const {env,api}=makeEnv({write:()=>{calls++; return Promise.resolve({});}});
    env.files['a.ts']={model:model('A'),dirty:true};
    api.hostProjectUnknown(true);
    let heldOne=false, heldAll=false;
    await api.saveOne('a.ts').then(()=>{},e=>{heldOne=!!e.held;});
    await api.saveAllDirty(true).then(()=>{},e=>{heldAll=!!e.held;});
    ok('FIN-01 saveOne refuses while the host project is unknown', heldOne);
    ok('FIN-01 saveAllDirty refuses too, so BUILD and PUSH stop', heldAll);
    ok('FIN-01 and nothing at all was sent', calls===0);
    ok('FIN-01 the file stays dirty, so it is saved once the project is known',
      env.files['a.ts'].dirty===true);
    api.hostProjectUnknown(false);
    await api.saveAllDirty(true);
    ok('FIN-01 and it does save once the tool has said which project it is in',
      calls===1 && env.files['a.ts'].dirty===false);
  }
  console.log('');
  console.log(pass+' passed, '+fail+' failed');
  process.exitCode=fail?1:0;
})();
