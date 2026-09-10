'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const {spawnSync} = require('node:child_process');
const exporter = require('./portal-export.cjs');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bf6-portal-export-'));
const toolchainDir = process.env.BF6_EXPORT_TOOLCHAIN || path.join(os.tmpdir(), 'bf6-portal-export-toolchain');
const npmCli = path.join(path.dirname(process.execPath), 'node_modules/npm/bin/npm-cli.js');
const outputDir = path.join(root, 'uploads');
let passed = 0;
function check(name, fn) { fn(); ++passed; console.log('PASS ' + name); }
function job(name, files) { const dir=path.join(root,name); fs.mkdirSync(dir); return exporter.build({files,outputDir,toolchainDir,npmCli},dir); }

(async () => {
  for (const name of ['../escape.ts','src/../escape.ts','C:/escape.ts','/escape.ts','src//escape.ts'])
    check('reject ' + name, () => assert.throws(() => exporter.sourceFiles({'index.ts':'export {};',[name]:'export {};'})));
  check('case collision', () => assert.throws(() => exporter.sourceFiles({'index.ts':'export {};','src/INDEX.ts':'export {};'})));
  check('entry required', () => assert.throws(() => exporter.sourceFiles({'rules.ts':'export {};'})));
  check('extended paths retained', () => assert.equal(exporter.sourceFiles({'src/index.ts':'export {};'} )['src/index.ts'],'export {};'));
  check('conflicting strings are rejected before a build',()=>assert.throws(()=>exporter.sourceFiles({'index.ts':'export {};','strings.json':'{"label":"One"}','src/ui/strings.json':'{"label":"Two"}'}),/Conflicting string key/));
  check('malformed strings are rejected before a build',()=>assert.throws(()=>exporter.sourceFiles({'index.ts':'export {};','strings.json':'{'}),/Invalid strings JSON/));
  const built = await job('modules', {
    'index.ts': "import * as state from './state.ts'; export function OnGameModeStarted(): number { state.increment(); return state.count; }\n",
    'state.ts': 'export let count = 0; export function increment(): void { count++; }\n',
    'strings.json': '{"greeting":"Hello"}'
  });
  const packageDir = fs.realpathSync(path.join(built.sourceDir,'node_modules'));
  const ts = require(path.join(packageDir,'typescript'));
  const text=fs.readFileSync(built.script,'utf8');
  const context={exports:{}}; vm.createContext(context);
  vm.runInContext(ts.transpileModule(text,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText,context);
  check('namespace imports and live state survive bundling', () => {
    assert.equal(context.exports.OnGameModeStarted(),1); assert.equal(context.exports.OnGameModeStarted(),2);
  });
  check('strings accompany script', () => assert.equal(JSON.parse(fs.readFileSync(built.strings)).greeting,'Hello'));
  const semantics=await job('named-and-types', {
    'index.ts': "import { count as value, increment, Item } from './state.ts'; import './init.ts'; export async function OnGameModeStarted(step: number): Promise<number> { const item: Item = {value}; function shadow(value:number){return value+1;} increment(step); return item.value + value + shadow(10); }",
    'state.ts': 'export interface Item { value:number; } export let count=0; export function increment(step:number){count+=step;}',
    'init.ts': "import {increment} from './state.ts'; increment(2);"
  });
  const ctx={exports:{}}; vm.createContext(ctx);
  vm.runInContext(ts.transpileModule(fs.readFileSync(semantics.script,'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText,ctx);
  assert.equal(await ctx.exports.OnGameModeStarted(3),18); ++passed;
  assert.equal(await ctx.exports.OnGameModeStarted(1),22); ++passed;
  const convert=require('../convert/convert.js'); convert.setTypeScript(ts);
  for (const source of [
    'export async function OnPlayerDeployed(p:mod.Player){const id=mod.GetObjId(p);await mod.Wait(1);mod.SetGameModeTargetScore(id);}',
    'function visit(n:number){const copy=n;if(n>0)visit(n-1);mod.SetGameModeTargetScore(copy);}export function OnGameModeStarted(){visit(2);}'
  ]) check('unsafe native conversion is blocked',()=>assert.ok(convert.nativeImportProblems(convert.tsToBlocks({'index.ts':source},{}).report).length));
  const sample=JSON.parse(fs.readFileSync(path.join(__dirname,'../blocks/snippets/ui_text_panel.json')));
  const workspace={mod:{blocks:{blocks:[{type:'modBlock',inputs:{RULES:{block:sample.blocks.blocks[0]}}}]}}};
  const generated=convert.blocksToTs(workspace,{});
  assert.equal(generated.report.unconvertible.length,0);
  let elapsed=0, waits=[];
  const runtime={exports:{},mod:{Wait:async seconds=>{elapsed+=seconds;waits.push(seconds);}}}; vm.createContext(runtime);
  vm.runInContext(ts.transpileModule(generated.files['runtime.ts'],{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText,runtime);
  await runtime.exports.WaitUntil(1,()=>elapsed>=0.4);
  check('WaitUntil rechecks a changing condition',()=>assert.equal(elapsed,0.4));
  elapsed=0; waits=[]; await runtime.exports.WaitUntil(0.25,()=>false);
  check('WaitUntil preserves fractional timeout',()=>assert.equal(elapsed,0.25));
  elapsed=0; waits=[]; await runtime.exports.WaitUntil(1,()=>false);
  check('WaitUntil does not yield an extra tick for floating-point residue',()=>assert.equal(waits.length,5));
  elapsed=0; await runtime.exports.WaitUntil(1,()=>true);
  check('WaitUntil completes immediately for a true condition',()=>assert.equal(elapsed,0));
  await assert.rejects(runtime.exports.WaitUntil(-1,()=>false)); ++passed;
  const real=await job('actual-blocks',generated.files);
  check('real Blockly UI example produces one upload script',()=>assert.ok(fs.statSync(real.script).size>100));
  const extended=require('../blocks/extended/compiler.js').compile({blocks:{blocks:[{type:'bf6x_event',id:'start',fields:{EVENT:'OnGameModeStarted'}}]}},{events:{OnGameModeStarted:{params:[]}}});
  assert.equal(extended.ok,true,JSON.stringify(extended.diagnostics));
  const extBuilt=await job('extended',extended.files); ++passed;
  if (process.env.BF6_EXPORT_FIXTURE) {
    const full=convert.blocksToTs(JSON.parse(fs.readFileSync(process.env.BF6_EXPORT_FIXTURE)),{});
    fs.writeFileSync(path.join(root,'fixture-report.json'),JSON.stringify(full.report,null,2));
    assert.equal(full.report.unconvertible.length,0);
    await job('full-mode',full.files); ++passed;
  }
  const before=fs.readdirSync(outputDir);
  await assert.rejects(job('bad-type',{'index.ts':'export const bad: number = "wrong";'})); ++passed;
  check('failed build publishes nothing', () => assert.deepEqual(fs.readdirSync(outputDir),before));
  check('failed build preserves earlier upload', () => assert.equal(fs.readFileSync(built.script,'utf8'),text));
  await assert.rejects(job('missing-module',{'index.ts':'import { missing } from "./absent.ts"; export function OnGameModeStarted(){ missing(); }'})); ++passed;
  const broken=path.join(built.sourceDir,'dist/broken.ts');
  fs.writeFileSync(broken,'// @ts-nocheck\nexport function OnGameModeStarted(){ return unresolvedModule.count; }');
  check('final validation catches errors hidden by ts-nocheck',()=>assert.throws(()=>exporter.validateBundle(ts,built.sourceDir,broken),/unresolvedModule/));
  const circular={'index.ts':"import './other.ts';export function OnGameModeStarted(){}",'other.ts':"import './index.ts';"};
  await assert.rejects(job('circular',circular),/circular dependency/); ++passed;
  const wrapper=path.join(__dirname,'checked-build.cjs');
  const badBuild=spawnSync(process.execPath,[wrapper,built.sourceDir,npmCli],{encoding:'utf8',windowsHide:true});
  check('Script editor rejects a successful but broken community bundle',()=>{assert.notEqual(badBuild.status,0);assert.match(badBuild.stderr,/Combined script validation failed/);});
  const goodBuild=spawnSync(process.execPath,[wrapper,extBuilt.sourceDir,npmCli],{encoding:'utf8',windowsHide:true});
  check('Script editor accepts a valid single-file mode',()=>assert.equal(goodBuild.status,0,goodBuild.stderr));
  console.log(passed + ' checks passed. Artifacts: ' + root);
})().catch(error => { console.error(error); process.exitCode=1; });
