'use strict';
// Execute the real browser import handlers. Failed conversion must leave the
// current workspace in place, including when entered through folder import.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const ts=require(process.env.BF6_TS || 'typescript');
const converter=require('../convert.js'); converter.setTypeScript(ts);
const source=fs.readFileSync(path.join(__dirname,'../../blocks/editor_ui.js'),'utf8');
const sf=ts.createSourceFile('editor_ui.js',source,ts.ScriptTarget.Latest,true,ts.ScriptKind.JS);
const wanted=new Set(['importScriptExport','importScriptFolder','acceptNativeImport','showConvertReport']);
const functions=[];
function visit(node){if(ts.isFunctionDeclaration(node)&&node.name&&wanted.has(node.name.text))functions.push(node.getText(sf));ts.forEachChild(node,visit);}
visit(sf);assert.equal(functions.length,wanted.size);
let replacements=0,status='';
const context={window:{BF6Convert:converter},UI:{},ensureTypeScript:fn=>fn(ts),$:()=>null,
  status:text=>{status=text;},loadWorkspaceDoc:()=>{replacements++;},hasAuthoredLayout:()=>true,trace:()=>{}};
vm.createContext(context);vm.runInContext(functions.join('\n'),context);
let passed=0;
for(const code of [
  'export async function OnPlayerDeployed(p:mod.Player){const id=mod.GetObjId(p);await mod.Wait(1);mod.SetGameModeTargetScore(id);}',
  'function visit(n:number){const copy=n;if(n>0)visit(n-1);mod.SetGameModeTargetScore(copy);}export function OnGameModeStarted(){visit(2);}',
  'class Unsupported {run(){}} export function OnGameModeStarted(){}'
])for(const kind of ['file','folder']){
  if(kind==='file')context.importScriptExport(code,'index.ts');else context.importScriptFolder({'index.ts':code},'project');
  assert.equal(replacements,0);assert.match(status,/Conversion stopped/);passed++;
}
const valid='export function OnGameModeStarted(){mod.SetGameModeTargetScore(10);}';
context.importScriptExport(valid,'index.ts');assert.equal(replacements,1);passed++;
context.importScriptFolder({'index.ts':valid},'project');assert.equal(replacements,2);passed++;
console.log(passed+' native import checks passed.');
