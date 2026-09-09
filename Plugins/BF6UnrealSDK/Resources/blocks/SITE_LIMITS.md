# Portal site limits, read out of the shipped site bundle

Research only. Every claim below is backed by a quote from a file on this machine, with a byte
offset you can reproduce with `grep -bo`.

## Where the bundle is

The plugin mirrors the live site's public assets under the Unreal project's `Saved` directory.
`BF6Blocks.cpp` names the two roots:

    C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\Plugins\BF6UnrealSDK\Source\BF6UnrealSDK\Private\BF6Blocks.cpp:96
        FString CacheDir()
        {
            return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("blockly"));
        }

    same file, line 498
            FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portalstyle")));

Which resolve to:

- **The site bundle (the primary evidence for this report):**
  `C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\Saved\BF6UnrealSDK\portalstyle\portal.battlefield.com\bf6\15249075\assets\index-6C8LwCJp.js`
  2,648,225 bytes, 1306 lines, minified. Site build id `15249075`. Mirrored 2026-09-06.
  Referred to below as **BUNDLE**.
- **The live capture of the server's block catalogue:**
  `C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\Saved\BF6UnrealSDK\portal\blockly\definitions.json`
  (4,176,215 bytes) and `options.json`, `renderer.txt` beside it. Referred to as **DEFS**.

The BUNDLE is the only file under `portalstyle` that contains `maxGlobalVariables`,
`maxObjectVariables` or `PortalRenderer`. `renderer.txt` in the blockly cache contains the single
word `portal`, and `sharedWorkspaceOptions` in the BUNDLE sets `renderer:"portal"`, so the site
build and the mirrored bundle agree.

Corroborating files used:
- `...\Plugins\BF6UnrealSDK\Resources\convert\catalog.json` (per-block input/return types, captured
  from the live site's own tooltips).
- `C:\Users\mwalt\Downloads\night_ops_breakthrough_workspace.json` (a genuine Portal workspace
  export, 5088 blocks, 104 variables).

---

## 1. What does `maxDepth` count?

**Answer: nothing. The client bundle never reads it.**

`maxDepth` appears exactly twice in the whole 2.6 MB bundle, and both occurrences are writes, not
reads. There is no traversal that computes a depth, because there is no consumer.

Occurrence 1, the constraint object itself, BUNDLE byte **593131**:

```js
me={maxDepth:64,maxGlobalVariables:ue??128,maxObjectVariables:ce??128},
```

with `ue` and `ce` sourced from server mutators immediately above it (BUNDLE byte **592430**,
function `getBlocklyDefinitions`):

```js
getBlocklyDefinitions=V=>{ ...
  J=...mutators...find(qe=>qe.name==="ModBuilder_MaxGlobalVariableCount"),
  ne=...mutators...find(qe=>qe.name==="ModBuilder_MaxObjectVariableCount"),
  ue=...J.kind.mutatorIntValues.availableValues.range.maxValue,
  ce=...ne.kind.mutatorIntValues.availableValues.range.maxValue,
  me={maxDepth:64,maxGlobalVariables:ue??128,maxObjectVariables:ce??128},
```

Occurrence 2, the zero-valued default used as the `reduce` seed, BUNDLE byte **593588**:

```js
constraints:{maxDepth:0,maxGlobalVariables:0,maxObjectVariables:0}
```

The object is then handed to Blockly and stored, BUNDLE byte **2380352**:

```js
Re.current.registerDefinitions(X,J,J.constraints,{isObjectInputWildcard:!0}),
```

and `registerDefinitions` only files it away, BUNDLE byte **2337990**:

```js
types:[],constraints:ne},this.definitions.types=[...new Set(ce)].filter(...),
this.blockImporterManager.init(this.rulesVersion,this.definitions),
this.subroutinesManager.init(this.definitions),
this.variablesManager.init(this.definitions)
```

The only code that ever reads a field off `definitions.constraints` is `BlocklyVariablesManager.init`
(section 2), and it reads only the two variable counts. Searching the bundle for `depth`, `Depth`,
`DEPTH` returns exactly seven hits: the two `maxDepth` writes above, and five occurrences of the
bundled markdown parser's heading depth (bytes 2206522, 2206572, 2206698, 2210066, 2213341, all of
the form `{type:"heading",depth:0,...}` / `tagName:"h"+X.depth`). There is no depth counter, no
depth guard on connect, no depth check on load, and no depth check in `JsonGenerator`.

For scale, the genuine `night_ops_breakthrough_workspace.json` export reaches a nesting depth of 14
(counting value inputs and statement inputs, not `next` siblings), well under 64, so it neither
confirms nor tests the number.

**Confidence: high** that the shipped client does not enforce `maxDepth`. **Unknown** what the
server or the game runtime does with 64, and unknown which of "nested value blocks" or "nested
statement blocks" the number was ever meant to describe. The value 64 is a hardcoded literal in the
client with no accompanying server mutator, unlike the other two, which suggests it is a mirror of
a runtime or compiler limit rather than an editor limit.

---

## 2. What do `maxGlobalVariables` and `maxObjectVariables` count?

**Answer: they gate the "create variable" dialog only. Global is a separate budget. All non-Global
object types share ONE budget of 128 between them, they do not get 128 each.**

`BlocklyVariablesManager`, BUNDLE bytes **2330488**, **2330575** and **2330693**:

```js
getGlobalVariableCount(){return this.mainWorkspace.getVariablesOfType("Global").length}
getObjectVariableCount(){return this.mainWorkspace.getAllVariables().filter(X=>X.type!=="Global"&&X.type!=="").length}
canCreateVariableOfType(X){return X==="Global"?this.getGlobalVariableCount()<this.maxGlobalVariables:this.getObjectVariableCount()<this.maxObjectVariables}
```

`getObjectVariableCount` filters `getAllVariables()` by "not Global and not blank" and returns one
number. Player, Team, CapturePoint, MCOM, Vehicle and every other object type all land in that one
count. So a workspace with 128 Player variables can create zero Team variables.

The budgets are loaded from the constraints, BUNDLE byte **2330139**:

```js
init(X){this.definitions=X,this.objectTypes=this.definitions.objects.map(J=>[getTranslatedDisplayName(J),J.type]),
this.maxGlobalVariables=this.definitions.constraints.maxGlobalVariables,
this.maxObjectVariables=this.definitions.constraints.maxObjectVariables,
this.createVariableTypesBlocks(this.objectTypes)}
```

The only consumer of `canCreateVariableOfType` is the create-variable dialog, BUNDLE byte
**2274801**:

```js
(V==null?void 0:V.getVariable(ge,Ae))?Ie.add("variable-already-exists"):Ie.delete("variable-already-exists"),
Ae&&!(X!=null&&X.canCreateVariableOfType(Ae))?Ie.add("too-many-variables"):Ie.delete("too-many-variables");
```

and the error set `Ie` only disables the Create button (`isDisabled:!ge||!Ae||!!Ie.size`). Nothing
re-checks the count on load, on import, or on save. A workspace JSON carrying 500 Player variables
loads without complaint from the client.

**What the client counts vs what it emits.** The compiled mod does not carry the dialog's numbers.
`createModBlockGenerators`, BUNDLE byte **2343346**, counts only variables actually *referenced* by
a `variableReferenceBlock`, and it emits object counts **per object type**, BUNDLE byte **2344085**:

```js
const me=J.workspace.getBlocksByType("variableReferenceBlock",!1),ge=new Set,Te={};
return me.forEach(Se=>{ ... if(ve==="Global")ge.add(Ie);else{
  const xe=X.objects.find(Ne=>Ne.type===ve)?.name; ...
  Te[xe]||(Te[xe]=new Set),Te[xe].add(Ie)}}),
{Settings:{GlobalVariableCount:ge.size,ObjectVariableCounts:Object.keys(Te).map(Se=>[Se,Te[Se].size])},Rules:ne,Subroutines:ue}
```

So the wire format is per-type, while the editor budget is aggregate. Two limits could exist
server-side (one per type) and the editor would still be the tighter of the two for a mixed
workspace. Treat 128 aggregate as the safe number.

Server origin of the numbers: both come from mutator max values (`ModBuilder_MaxGlobalVariableCount`
at BUNDLE byte **592634**, `ModBuilder_MaxObjectVariableCount` at **592779**), with `??128` as the
fallback when the mutator is missing. `definitions.json` in our cache, captured from a live session,
ends with:

    ...\portal\blockly\definitions.json (tail)
    "constraints":{"maxDepth":64,"maxGlobalVariables":128,"maxObjectVariables":128}

so the live server is currently serving 128/128, it is not just our fallback.

Corroboration from a real export: `night_ops_breakthrough_workspace.json` declares 66 `Global`, 33
`Player` and 5 `Team` variables, that is 66 global and 38 object, both under 128.

**Confidence: high** on "Global is separate", "object types share one budget", and "only the create
dialog enforces it". **Confidence: medium** that 128 is stable, since it is a server-sent mutator
value that can change per session.

---

## 3. Can an array hold another array?

**Answer: yes on the block side, with no check anywhere that would reject it.** This is the most
load-bearing answer in the report, so here is the whole chain.

**(a) The block definitions.** `AppendToArray` and `ValueInArray` are not defined in the bundle at
all, they are generated from the server catalogue. DEFS byte **29137**:

```json
{"name":"AppendToArray","category":"Arrays","functionSignatures":[{
  "returnType":"Array",
  "parameterTypes":[
    {"anyType":false,"parameterTypes":["Array"],"parameterName":"array"},
    {"anyType":true,"parameterTypes":[],"parameterName":"value"}]}]}
```

DEFS byte **146752**:

```json
{"name":"ValueInArray","category":"Arrays","functionSignatures":[{
  "parameterTypes":[
    {"anyType":false,"parameterTypes":["Array"],"parameterName":"array"},
    {"anyType":false,"parameterTypes":["Number"],"parameterName":"index"}]}]}
```

Note `AppendToArray` param 1 is `anyType:true`, and `ValueInArray` has **no** `returnType`.
`catalog.json` (captured independently from the site's own tooltips) agrees:

    ...\Resources\convert\catalog.json
    "AppendToArray":{"kind":"value","inputs":["VALUE-0","VALUE-1"],"params":["Array","Any Type"],"ret":"Array",...}
    "ValueInArray": {"kind":"value","inputs":["VALUE-0","VALUE-1"],"params":["Array","Number"],  "ret":"Any Type",...}

**(b) How `anyType` becomes a socket check.** BUNDLE byte **2291905**:

```js
function isParameterIndexOfAnyType(V,X){return!!V.find(J=>J.parameterTypes?.[X]?.anyType===!0)}
function getTypesForBlockParameterIndex(V,X,J){
  if(isParameterIndexOfAnyType(V,J))return null;
  const ne=filterFunctionSignaturesForBlock(V,X);
  return[...new Set(ne.flatMap(ue=>ue.parameterTypes?.[J]?.parameterTypes))].filter(ue=>ue!==void 0)}
function getTypesForBlockOutput(V,X){ ...
  J=[...new Set(ne.map(ue=>ue.returnType))].filter(ue=>ue!==void 0),J.length===0&&(J=null),J}
```

and the value-block factory, BUNDLE bytes **2314850** and **2314946**:

```js
getTypesForParameterIndex(Ae){return me?getTypesForBlockParameterIndex(me,this,Ae)??V.types:[]},
getTypesForOutput(){return me?getTypesForBlockOutput(me,this)??V.types:[]},
```

So `null` (anyType, or no returnType) becomes `V.types`, the full type list. Replaying
`registerDefinitions`' type-gathering (BUNDLE byte **2336132**) over our cached DEFS yields 116
types, and `"Array"` is among them. Therefore:

- `AppendToArray` socket `VALUE-1` has check = all 116 types, which **includes `Array`**.
- `AppendToArray` output check = `["Array"]`.
- `ValueInArray` output check = all 116 types, which **includes `Array`**, so reading a nested array
  back out and plugging it into an `Array` socket also connects.

**(c) The connection checker is stock Blockly.** `sharedWorkspaceOptions` (BUNDLE byte **2373479**)
registers no custom connection checker, and `doTypeChecks` is the unmodified Blockly implementation,
BUNDLE byte **1065491**:

```js
doTypeChecks(Q,oe){if(Q=Q.getCheck(),oe=oe.getCheck(),!Q||!oe)return!0;
for(let pe=0;pe<Q.length;pe++)if(oe.indexOf(Q[pe])!==-1)return!0;return!1}
```

`["Array"]` intersected with the 116-type list is non-empty, so the connection is allowed.

**(d) No other gate.** Searching the bundle for `AppendToArray` and `ValueInArray` returns zero
hits, so there is no hand-written special case for either. There is no `setCheck` anywhere that
excludes `Array`. The load-time validators (section 5) do not look at types at all. The
`JsonGenerator` array path is plain recursion with no type inspection, BUNDLE byte **2348067**:

```js
blockToObj(X){if(X){const J=this.blockGeneratorMap.get(X.type);
if(J){const ne=J(X);return this.scrubStatementBlock(X,ne)}
else return console.warn("JSON Generator does not know how to generate block type "+X.type),this.scrubStatementBlock(X,[])}return null}
```

**Confidence: high** that the editor accepts an array inside an array and serializes it without
complaint. **Not proven:** the genuine `night_ops_breakthrough_workspace.json` uses arrays heavily
(23 `EmptyArray`, 28 `AppendToArray`, 57 `ValueInArray`, plus `FilteredArray`, `SortedArray`,
`RandomizedArray`, `RemoveFromArray`, `IndexOfArrayValue`, `ArrayContains`) but contains **zero**
nested arrays, so we have no in-the-wild example of the runtime handling one. The block layer allows
it; whether the game's `Array` value type is genuinely recursive is not something this bundle can
answer.

---

## 4. May a subroutine call itself?

**Answer: the code shows no check.** I looked for one and did not find it. I am not claiming
recursion works, only that nothing in the client prevents it.

The flyout that offers call blocks lists every subroutine unconditionally, with no exclusion of the
one the user is currently inside, BUNDLE byte **2319614**:

```js
subroutinesFlyoutCallback(X){ ...
if(coreBrowserExports.Blocks.subroutineBlock&&coreBrowserExports.Blocks.subroutineInstanceBlock)
  for(const me of this.subroutines){
    const ge=...createElement("block");ge.setAttribute("type","subroutineInstanceBlock");
    const Te=...createElement("field");Te.setAttribute("name","SUBROUTINE_NAME");
    Te.appendChild(...createTextNode(me));ge.appendChild(Te);J.push(ge)}
return J}
```

The call block itself is an ordinary statement block with no `onchange` guard against its own
parent, BUNDLE byte **2323719**:

```js
.subroutineInstanceBlock={init(){this.setDeletable(!0),
this.appendDummyInput().appendField(new coreBrowserExports.FieldLabelSerializable(""),"SUBROUTINE_NAME"),
this.setPreviousStatement(!0,"actionBlock"),this.setNextStatement(!0,"actionBlock"),
this.setInputsInline(!0),this.setStyle("action-block-style"),this.updateShape()},
onchange(J){J instanceof PyriteBlocklyEventsSubroutineParametersChange&&this.updateShape()}, ...
```

A `subroutineBlock`'s `ACTIONS` slot has `setCheck("actionBlock")` (BUNDLE byte **2321572**) and the
call block sets `setPreviousStatement(!0,"actionBlock")`, so a call block drops into its own
subroutine's body with no complaint.

The generator is a flat lookup with no visited-set and no cycle detection, BUNDLE byte **2345288**:

```js
V.registerBlockGeneratorCallback("subroutineInstanceBlock",J=>{
  const ne=J.getFieldValue("SUBROUTINE_NAME"),ue=[ne];
  return X.getSubroutineParameters(ne).forEach((ge,Te)=>{ ... }),
  {Action:"CallSubroutine",Params:ue}}),
```

It emits a name, it never follows the call, so recursion cannot even hang the exporter.

I enumerated every identifier in the bundle matching `/[Rr]ecursi/`. There are 16 distinct ones and
not one of them is a workspace check:

- 13 React fiber internals (`recursivelyTraverseLayoutEffects` and friends, x101 total,
  `workInProgressRootDidIncludeRecursiveRenderUpdate` x4).
- `recursion` x1, byte **1755861**, inside react-fast-compare's circular-ref guard:
  `if((ne.message||"").match(/stack|recursion/i))return console.warn("react-fast-compare cannot handle circular refs"),!1`.
- `recursiveSearch` x2, bytes **2371319** and **2371932**, the toolbox search plugin walking toolbox
  categories with fuzzysort. Nothing to do with the workspace.
- `recursively` x2 outside React, bytes **688455** (an i18next warning about nested translation keys)
  and **923601** (`console.log("Trying to end a gesture recursively.")` in Blockly's gesture handler).

`cycle` appears only in CSS and animation contexts. There is no `procedures_callreturn` in this
bundle at all; Portal uses its own `subroutineInstanceBlock`, so Blockly's stock procedure recursion
warning does not apply here either.

**Confidence: high** that the client contains no recursion or cycle check. **Unknown** what the
server compiler or the game runtime does with a self-call, including whether there is a call-depth
budget. This must be tested by deploying.

---

## 5. What else does the site validate, on load and on save?

Everything I found, with locations. The short version: the block editor is remarkably permissive.
There is no schema validation, no structural validation, and no type re-check on load.

**5.1 Load-time, XML path. Two passes, both repairs rather than rejections.** BUNDLE byte
**2340190**:

```js
async validateWorkspaceDom(X){return X=fixBrokenXmlValidator(X),X=missingBlockValidator(X),X}
validateWorkspaceJson(X){return validateJsonWorkspace(X)}
```

`fixBrokenXmlValidator` (BUNDLE byte **2289302**) back-fills an empty `OBJECTTYPE` field on
`variableReferenceBlock` from the sibling `VAR` field's `variabletype` attribute.

`missingBlockValidator` (BUNDLE byte **2289638**) rewrites any block whose type is not registered:

```js
function missingBlockValidator(V){const X=V.getElementsByTagName("block");
for(let ne=0;ne<X.length;++ne){const ue=X[ne].getAttribute("type"),ce=X[ne].parentElement?.nodeName==="value";
ue&&coreBrowserExports.Blocks[ue]===void 0&&(ce?X[ne].setAttribute("type","missingValueBlockType_v1"):X[ne].setAttribute("type","missingActionBlockType_v1"),
X[ne].innerHTML=`<field name="DELETED_NAME">${ue}</field>\n`+X[ne].innerHTML)}return V}
```

**5.2 Load-time, JSON path. Same single rule.** BUNDLE bytes **2290091** and **2290601**:

```js
const validateJsonBlock=(V,X=!1)=>{const J=V.next?...validateJsonBlock(me)...:void 0;
if(coreBrowserExports.Blocks[V.type]===void 0)return{...V,type:X?"missingValueBlockType_v1":"missingActionBlockType_v1",inputs:void 0,fields:{...V.fields,DELETED_NAME:`${V.type}`},next:J};
const ne=V.inputs?Object.entries(V.inputs).reduce((ue,[ce,me])=>({...ue,[ce]:{block:validateJsonBlock(me.block,ce.startsWith("VALUE"))}}),{}):void 0;
return{...V,inputs:ne,next:J}},
validateJsonWorkspace=V=>({...V,blocks:{blocks:(V?.blocks.blocks.map(validateJsonBlock))??[]}}),
```

Unknown block type becomes a "DELETED" placeholder that keeps its name. Note the placeholder's
`inputs` are dropped, so an unknown block silently takes its whole subtree with it. That is the one
real hazard in the load path for a compiler that emits a block name the server catalogue does not
have.

**5.3 Orphan blocks are silently dropped at compile.** `JsonGenerator.compile`, BUNDLE byte
**2348323**:

```js
compile(X){const J=X.getTopBlocks(!1);for(const ne of J)if(ne.type==="modBlock")return asSingleValue(this.blockToObj(ne));return null}
```

Only the **first** top-level `modBlock` is compiled. A second `modBlock` is ignored. Top-level blocks
that are neither `modBlock` nor `subroutineBlock` are never visited and vanish from the output with
no warning. Subroutines are picked up separately, from the workspace, not from the mod block's tree,
BUNDLE byte **2343346**:

```js
V.registerBlockGeneratorCallback("modBlock",J=>{
  const ne=asArrayValue(V.blockToObj(J.getInputTargetBlock("RULES"))),ue=[],ce=J.workspace.getTopBlocks(!1);
  for(const Se of ce)if(Se.type==="subroutineBlock"){const Ie=asSingleValue(V.blockToObj(Se));Ie&&ue.push(Ie)}
```

So a `subroutineBlock` must be a **top-level** block. Nested anywhere else it is invisible to the
compiler.

An unknown block type reaching the generator only warns, it does not fail (quoted in 3(d) above:
`console.warn("JSON Generator does not know how to generate block type "+X.type)`).

**5.4 Statement and value socket checks (the real structural rules).** These are the connection
constraints the compiler must respect:

| Block | Socket | `setCheck` | BUNDLE byte |
|---|---|---|---|
| `modBlock` | `RULES` statement | `"ruleBlock"` | 2303515 |
| `ruleBlock` | `CONDITIONS` statement | `"conditionBlock"` | 2304391 |
| `ruleBlock` | `ACTIONS` statement | `"actionBlock"` | 2304391 |
| `conditionBlock` | `CONDITION` value | `"Boolean"` | 2303890 |
| `conditionBlock` | prev/next | `"conditionBlock"` | 2303890 |
| `subroutineBlock` | `CONDITIONS` / `ACTIONS` | `"conditionBlock"` / `"actionBlock"` | 2321572 |
| `If`, `While` | `VALUE-0` value | `"Boolean"` | 2307646 (If), 2313224 (While) |
| `If`, `While` | `DO` statement | `"actionBlock"` | 2307646 (If), 2313224 (While) |
| all action blocks | prev/next | `"actionBlock"` | 2297931 |
| `Break`, `Continue` | prev/next | `"actionBlock"` | 2306268 |

`modBlock` is `setDeletable(!1)`. There is no check that a `Break` sits inside a loop.

**5.5 Value-socket type checks are dynamic and can narrow after the fact.** For blocks with more
than one signature, the socket checks are recomputed on every relevant workspace event,
BUNDLE byte **2292392** (`filterFunctionSignaturesForBlock`) and the `onchange` handlers at
**2299221** (actions) and **2315723** (values):

```js
onchange(ce){if(isEventRelatedToBlock(ce,this)&&ue){
  for(let me=0;me<this.maxParametersCount();me++){const ge=this.getInput(`VALUE-${me}`),Te=this.getTypesForParameterIndex(me);ge?.setCheck(Te)}
  this.setTooltip(this.getTooltipText()),this instanceof coreBrowserExports.BlockSvg&&this.render()}}
```

So filling socket 0 of an overloaded block narrows what socket 1 will accept. Blockly does not
retroactively eject an already-connected child when a check tightens, but a *later* drag into that
socket can be refused. Worth knowing when generating multi-signature blocks.

**5.6 Name rules.**

- Variables are keyed by the pair (name, type). Duplicate check, BUNDLE byte **2274684**:
  `(V?.getVariable(ge,Ae))?Ie.add("variable-already-exists"):...`. The same name is legal on two
  different types.
- Subroutine names must be unique across all subroutines, checked on create and on rename,
  BUNDLE byte **2432078**:
  ```js
  fn=(bt,Bt)=>{if(ft?.subroutinesManager?.subroutines?.find(mn=>mn===bt)){zt(Te("rules.variable-already-exists"));return}kt.create(bt,Bt),...}
  vn=(bt,Bt,cn)=>{if(bt!==Bt&&ft?.subroutinesManager?.subroutines?.find(Tn=>Tn===Bt)){zt(Te("rules.variable-already-exists"));return}...}
  ```
- A 30-character maximum for subroutine and parameter names is **declared but never wired up**.
  The message map exists, BUNDLE byte **2283013**:
  ```js
  $e=new Map([["name",new Map([[VALIDATION_RULE.REQUIRED,t("rules.name-required")],[VALIDATION_RULE.MAX_LENGTH,t("rules.maximum-length-exceeded",{max:30})]])],
              ["parameter",new Map([[VALIDATION_RULE.REQUIRED,t("rules.parameter-required")],[VALIDATION_RULE.MAX_LENGTH,t("rules.maximum-length-exceeded",{max:30})]])]]),
  ```
  but both text fields pass only `validation:{required:!0}` (BUNDLE bytes **2284830** and
  **2286347**), and `TextInput` fires MAX_LENGTH only when a `maxLength` is supplied, BUNDLE byte
  **132199**:
  ```js
  const lt=({target:{value:St}})=>{(xe?.maxLength)!==void 0&&(St.toString().trim().length>xe.maxLength?gt.add(VALIDATION_RULE.MAX_LENGTH):gt.delete(VALIDATION_RULE.MAX_LENGTH),...)}
  ```
  So the enforced rule as shipped is "non-empty". Treat 30 as a soft target anyway, since it is the
  number the site's own authors wrote down.
- No character-set or identifier regex is applied to variable or subroutine names anywhere in the
  block editor.

**5.7 Warnings that mark a block without blocking anything.** The rule event block sets a warning
and a different style when its event or object type is not in the catalogue, or is deprecated,
BUNDLE byte **2317324** (end of the branch):

```js
xe?(this.setWarningText(null,"unsupportedRuleEvent"),Ne?.deprecated?this.setWarningText(getBlocklyTranslation("PYRITE_DEPRECATED_RULE_EVENT"),"deprecatedRuleEvent"):this.setWarningText(null,"deprecatedRuleEvent"),this.setStyle("value-block-style")):(this.setWarningText(getBlocklyTranslation("PYRITE_UNSUPPORTED_RULE_EVENT"),"unsupportedRuleEvent"),...,this.setStyle("unsupported-value-block-style"))
```

The blocks-to-TypeScript view emits advisory warnings only, never errors, BUNDLE bytes 2240000 to
2300000: `addWarning("unknown types",...)`, `addWarning("Unknown mod blocks",J.type)`,
`addWarning("Unknown object vars",...)`, `addWarning("No GetArgument parameter at index",...)`,
`addWarning("unknown event parameters using eventName",...)`. None of these gate a save.

**5.8 Import of a `.json` / `.xml` workspace file.** No structural validation before the load
validators run, BUNDLE byte **2431598**:

```js
kn=bt=>{bt.target.files&&bt.target.files[0]&&readFile({file:bt.target.files[0],onLoad:(Bt,cn,ln)=>{
  if(typeof Bt.target?.result=="string")try{
    const Kn=Bt.target?.result.startsWith("<")?Bt.target?.result:JSON.parse(Bt.target?.result);
    logger$5.debug("modRules JSON file import",Kn),Ft(Kn),Ut(cn??""),Ct(ln??0)}
  catch(Kn){logger$5.error("Failed to import workspace",Kn)}}})}
```

A leading `<` means XML, otherwise `JSON.parse`. That is the entire format check.

**5.9 Import of a whole exported experience.** `useImportExperience`, BUNDLE byte **1920043**.
It validates game mode and mutators (min, max, forbidden, type, unknown) and produces
`errors` / `warnings`, but it passes the Blockly workspace through untouched:

```js
Ye.length?{status:"fail",errors:Ye,warnings:Qe}:( ... ,Ie(it),Re(Xt),$e(!0),{status:"success",errors:Ye,warnings:Qe})
```

`Ie(it)` is `setAtom(workspaceAtom)(workspace)`. The workspace is never inspected here.

**5.10 Experience-level limits (not block-related, listed for completeness).** BUNDLE byte
**590455**:

```js
constraints:{maxNameSize:FALLBACK_EXPERIENCE_NAME_MAX_LENGTH,maxDescriptionSize:FALLBACK_EXPERIENCE_DESCRIPTION_MAX_LENGTH,maxSecretSize:512,maxMapsInRotation:32,maxMutators:256,maxConfigNameSize:128,maxConfigDescriptionSize:256}
```

with the fallbacks at BUNDLE byte **392115**:

```js
FALLBACK_EXPERIENCE_NAME_MAX_LENGTH=64,FALLBACK_EXPERIENCE_DESCRIPTION_MAX_LENGTH=256,MAP_ROTATION_MAX=20,
```

and a file-size cap on uploaded script and string attachments, BUNDLE byte **401601**:

```js
MAX_ATTACHMENT_SIZE_BYTES=2**21+2**20,
```

= 3,145,728 (3 MiB), enforced on the read string's `.length`, that is on characters, not bytes,
BUNDLE byte **2474100**:

```js
if(Ft?.length>MAX_ATTACHMENT_SIZE_BYTES)throw Re(),new Error(t("attachments.errors.filesize",{size:`${bytesToMbString(MAX_ATTACHMENT_SIZE_BYTES)}`}));
```

I did **not** find this cap applied to the Blockly workspace itself. The workspace travels as
`ModRulesDefinition.modBuilder` protobuf bytes (BUNDLE byte **371849**), on a separate field from
the attachments, and no client-side size check guards it.

**5.11 What the client does NOT validate.** No dangling-connection check. No "rule with no event"
check. No check that a `Break` is inside a loop. No check that a subroutine is ever called, or that
a called subroutine exists (a `subroutineInstanceBlock` naming a deleted subroutine still compiles
to `{"Action":"CallSubroutine","Params":["<name>"]}`). No check that `GetArgument` indices are in
range at save time (only a decompiler warning). No arity check on `subroutineInstanceBlock`
parameters at save. No duplicate-rule-name check.

**Confidence: high** on each individually quoted item. **Medium** on completeness: I swept the
bundle for `max*` / `MAX_*` / `*Limit` identifiers in the rules-editor byte range and for the
obvious validation vocabulary, but a minified bundle can always hide a limit behind a name I did not
think to search.

---

## 6. Is there a limit on array length, or on block, rule or subroutine counts?

**Answer: none in the client, for any of the four.**

**Blocks.** Blockly's own capacity mechanism is explicitly disabled. The workspace is constructed
with `maxBlocks` set to Infinity for editable workspaces, BUNDLE byte **2379516**:

```js
const Je={...sharedWorkspaceOptions,readOnly:ne||sharedWorkspaceOptions.readOnly,maxBlocks:ne?0:1/0};
Re.current=new PyriteBlockly(V.current,Je);
```

(`ne` is `isReadOnly`, so a read-only preview gets 0 and the editor gets Infinity.)
`sharedWorkspaceOptions` (BUNDLE byte **2373479**) sets no `maxBlocks` and no `maxInstances` of its
own. Blockly's capacity check is therefore inert, BUNDLE byte **1076263**:

```js
hasBlockLimits(){return this.options.maxBlocks!==1/0||!!this.options.maxInstances}
```

Corroborated by a live capture of the real site's workspace options,
`...\Saved\BF6UnrealSDK\portal\blockly\options.json`:

```json
"maxBlocks": null,
```

and by the genuine export `night_ops_breakthrough_workspace.json`, which contains **5088 blocks**.

**Rules.** No cap. `modBlock`'s `RULES` statement input is an ordinary Blockly statement stack.
Nothing counts `ruleBlock`s. Only `V.eventCounts` in the decompiler counts events, and that is for
naming generated TypeScript functions, not for limiting.

**Subroutines.** No cap. `get subroutines()` (BUNDLE byte **2319293**) just filters top blocks, and
the only guard on create is the uniqueness check in 5.6. No cap on the number of parameters per
subroutine either: the add-parameter button (BUNDLE byte **2283393**,
`we=()=>{Ie(ze=>[...ze,{name:"",types:new Set}])}`) has no ceiling.

**Array length.** No limit exists in the client because arrays are runtime values, not editor
objects. `EmptyArray`, `AppendToArray`, `ArraySlice`, `FilteredArray`, `MappedArray`,
`RandomizedArray`, `SortedArray`, `RandomValueInArray`, `ValueInArray`, `CurrentArrayElement` are
all opaque catalogue entries in DEFS with nothing but types on them. There is no length field, no
count, no cap, in the bundle or in `definitions.json`. Whatever limit exists lives in the game.

**Confidence: high** for blocks, rules and subroutines (three independent sources agree for blocks).
**High** that the *client* imposes no array-length limit; **no information at all** about the
runtime's.

---

## What we still cannot know without deploying

The bundle is the client. Everything below is on the far side of the upload.

1. **Whether `maxDepth: 64` is enforced anywhere at all, and what it counts.** The client never
   reads it. The 64 is a hardcoded literal with no matching server mutator, unlike the two variable
   caps. It may be a mirror of a game-runtime expression-depth limit, a compiler stack limit, or
   dead code. Only a deploy of a deliberately deep expression, and separately a deliberately deep
   statement nest, can distinguish these.
2. **Whether the server enforces object variables per type or in aggregate.** The client budget is
   one shared 128. The emitted `Settings.ObjectVariableCounts` is a per-type list, and the mutator is
   singular (`ModBuilder_MaxObjectVariableCount`). A deploy with, say, 120 Player and 120 Team
   variables would settle it. Until then, assume 128 aggregate.
3. **Whether nested arrays survive the server compiler and behave in the game.** The block editor
   accepts them, the exporter serializes them, and no in-the-wild workspace we have uses one. The
   runtime's `Array` may or may not be a genuinely recursive value type. This is the highest-value
   deploy test on the list, since the whole packing strategy rests on it.
4. **Whether a subroutine may call itself, and whether there is a call-depth budget.** No client
   check exists. Self-call, mutual recursion (A calls B calls A), and depth-limited recursion are
   three different questions and all three need a deploy.
5. **Any server-side size cap on the compiled mod.** The 3 MiB `MAX_ATTACHMENT_SIZE_BYTES` applies
   to uploaded script and string attachments, not to `modRules.modBuilder`. There may be a gRPC
   message cap or a stored-blob cap we cannot see.
6. **The full mutator catalogue.** `getBlocklyDefinitions` reads exactly two `ModBuilder_*` mutators
   by name. The server sends a whole mutator list that may carry more limits the current client
   simply ignores. Capturing `availableGameData.mutators` in full from a live session would extend
   this report without deploying anything.
7. **Server-side type checking on upload.** We know from prior work that the site type-checks
   uploaded TypeScript. Whether an equivalent pass runs over the Blockly `modBuilder` payload, and
   what it rejects, is not visible from the client bundle.
8. **Runtime array length, string length and message limits.** Nothing in the client.

---

*Compiled 2026-09-06 against site build `15249075`. All byte offsets reproduce with
`grep -bo "<literal>" index-6C8LwCJp.js` on the mirrored bundle named at the top of this file. If
the site rehashes its bundle, the offsets move but the search strings should still find the code.*
