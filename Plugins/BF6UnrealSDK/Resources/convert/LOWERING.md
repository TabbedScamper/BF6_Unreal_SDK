# LOWERING

How each TypeScript construct the converter cannot yet handle becomes Portal blocks.

Research only. Nothing here is implemented. Every corpus claim carries a file and line
under `C:\PortalSDK_1.4.2.0\GodotProject\User_Created\projects\`.

---

## 0. Facts this design rests on

Measured, not assumed. Each has a citation.

**Portal has an inline conditional.** `mod.IfThenElse(condition, trueValue, falseValue): Any`
(`Battleship/node_modules/bf6-portal-mod-types/index.d.ts:2410`), block `IfThenElse`
with three inputs (`catalog.json`). Its doc carries a warning that decides several
lowerings below: "Both `trueValue` and `falseValue` will be evaluated if they are
derived from function calls." **IfThenElse does not short circuit.**

**Portal has a definedness test.** `mod.IsUndefined(value: Any): boolean` (`index.d.ts:2416`,
"Returns whether a value is undefined, such as when a function cannot return a valid
value") and `mod.IsValid(value: Any): boolean` (`index.d.ts:2419`, "Returns whether a
value is defined and if object reference is valid for object values"). Both are blocks.

**Arrays cannot nest.** `AppendToArray` doc, `index.d.ts:2208`: "Note: It is not possible
for an array to contain arrays. Attempting to append an array to an array will
concatenate them instead." This kills every array-of-structs encoding. It does not kill
arrays held in *variables*, only arrays held in array *elements*.

**Object variables exist for every object type.** `mod.ObjectVariable(ownerObject: mod.Object,
variableIndex: number): Variable` (`index.d.ts:2203`). The `GetObjId` block's parameter list
(`catalog.json`) enumerates the owner types: AreaTrigger, Bomb, CapturePoint,
EmplacementSpawner, FixedCamera, HQ, InteractPoint, LootSpawner, MapSpecificFeature, MCOM,
Player, RingOfFire, Sector, SFX, SpatialObject, Spawner, SpawnPoint, Team, Vehicle,
VehicleSpawner, VFX, VO, WaypointPath, WorldIcon. Every one of those has an
id-to-object getter (`GetPlayer`, `GetVehicle`, `GetSpatialObject`, `GetInteractPoint`,
`GetVFX`, `GetSFX`, ... all in `catalog.json`).

**Recursion is allowed and is capped at 512 frames.** Discord corpus,
`research/_tools/discord/topics/cand-scripting.md:9693`, a Portal author reporting on
their own workspace: "i'm trying to get a subroutine to run on a loop every second (at
the end it Waits 1 and then Calls itself) but it dies after a while with the error
`Callstack reached depths of 512 that exceeds max allowed depth of 512`". See section 12.

**Message takes at most three substitutions.** `mod.Message(msg, msgArg0, msgArg1, msgArg2)`
(`index.d.ts:2833`), with overloads down to one argument (`:2852`, `:2869`, `:2881`).
Placeholders in `strings.json` are `{}`
(`BF6-Undead-Ground-Zero/src/strings.json`, `"deployedOnMap": "You are {} on {}"`).

**Time is available in seconds.** `mod.GetMatchTimeElapsed(): number`, doc "the amount of
time elapsed (seconds) in the current gamemode" (`index.d.ts:2263`).

**Budget.** 128 variables per scope across Global, Player and Team; expression nesting
capped at 64. Both read out of the Portal editor bundle and recorded in
`convert.js:217` as `{ maxDepth: 64, maxGlobalVariables: ue ?? 128, maxObjectVariables: ce ?? 128 }`.

**The converter already owns four mechanisms this design builds on** rather than
replacing:

| mechanism | where | what it does |
| --- | --- | --- |
| `program.fieldSlots` | `convert.js:1721` | a program-wide field-name to array-index table, graph coloured so non-conflicting names share an index |
| `recordToIr` | `convert.js:4736` | an object literal becomes a flat array laid out by `fieldSlots` |
| `packGlobals` | `convert.js:3434` | scalar Globals fold into 256-wide array packs when the scope exceeds 128 |
| `notePoolable` / frame vars | `convert.js:3957` | function locals share slots with locals that cannot be live at the same time |

---

## 1. Cross-module calls (644 sites)

Rank 1. Highest sites-per-unit-risk in the whole list, by a wide margin.

### Detection

Today `convert.js:4198` routes a call on a namespace: `mod` and its aliases, `rt`,
`modlib`, `subs` are handled, and everything else falls to
`unconvertibleStmt(..., 'call through ' + ns)` at `convert.js:4203`. That single line is
the 644 sites.

The AST test is `ts.isCallExpression(e) && ts.isPropertyAccessExpression(e.expression) &&
ts.isIdentifier(e.expression.expression)`, then resolve `e.expression.expression.text`.

### Lowering

**Do not write a module resolver. Convert the bundle.**

Every project in the corpus already ships a link step that flattens the module graph.
`BF6-Undead-Ground-Zero/package.json:13`:

```
"build:raw": "bf6-portal-bundler --entrypoint ./src/index.ts --outDir ./dist"
```

The output, `BF6-Undead-Ground-Zero/dist/bundle.ts`, is 17,344 lines, carries 39 sources,
and contains **zero** residual `import` statements (verified: `grep -n "^import " dist/bundle.ts`
returns nothing). Each source is fenced by a marker the bundler emits:

```
// --- SOURCE: src\systems\amp-arsenal.ts ---
```

at `dist/bundle.ts:12097`, so the original file and line are recoverable by counting
from the marker. The converter should take `dist/bundle.ts` as input, keep a
marker-to-offset table, and report against original coordinates.

After bundling, `AmpArsenal.onRoundStart()` at `src/event-handlers.ts:1262` is a call to a
static method of a class declared at `src/systems/amp-arsenal.ts:236` in the same file.
Namespace resolution collapses to a single-file symbol lookup: find the
`ClassDeclaration` / `namespace` / `const` named `AmpArsenal`, find the member
`onRoundStart`, emit `{ k:'sub', name:'AmpArsenal_onRoundStart', args:[] }`.

Note that `AmpArsenal` is **not imported** in `src/event-handlers.ts` at all (checked:
no import line names it). A source-tree resolver would have had to guess. The bundler
already answers it. This is the whole argument.

Worked example, `src/event-handlers.ts:1262`:

```ts
try { AmpArsenal.onRoundStart(); } catch (e) {}
```

becomes, after bundling and after the existing try/catch rule at `convert.js:4230`
unwraps the empty handler:

```
{ k:'sub', name:'AmpArsenal_onRoundStart', args:[] }
```

one `subroutineInstanceBlock` with `SUBROUTINE_NAME = "AmpArsenal onRoundStart"`.

**Name mangling.** Two modules may both export `init`. The bundler does not rename
(it relies on the sources not colliding, and `dist/bundle.ts` compiles), so the flat
file is already collision free by construction. Where the converter synthesises a
subroutine name from a class member it must qualify: `<Owner>_<member>`. Portal
subroutine names are free text (field `SUBROUTINE_NAME`, `catalog.json:subroutineBlock`),
so `AmpArsenal onRoundStart` is legal as-is.

**`import * as X`.** One site in the whole corpus: `Hospital-Trailer/src/index.ts:14`,
`import * as modlib from "../modlib/index.ts"`, and `modlib` is already a handled
namespace at `convert.js:4198`. The other 582 imports are named and 15 are type-only
(measured over 629 import declarations). Namespace imports are a non-problem.

**Re-exports.** None appear in the flattened bundle by definition. If the converter is
ever pointed at raw sources, a re-export is one extra hop in the same lookup; but the
recommendation is not to point it at raw sources.

**Helper modules that are not user code.** `Timers` (section 5), `Log`, `Sounds`,
`Raycast`, `Events` all bundle in from `node_modules/bf6-portal-utils`. Their bodies are
now visible in the bundle, so they convert like any other code, and the converter gets
their semantics for free instead of having to model them. `Timers` at
`dist/bundle.ts:894` is 79 lines and is built on `mod.Wait`.

### Cost

Zero variables. Zero depth. A subroutine call is one block.

### Fidelity

Exact, with one caveat: a subroutine call is not an expression unless the subroutine
returns. `procedures_callreturn` exists in the catalogue and the brief confirms it in a
real workspace, so value-returning calls are available; a call used as a value becomes
`{ k:'subCall' }`, which `convert.js:660` and `:4935` already emit.

### Impossible

A call through a value rather than a name (`handlers[i]()`, `cb()` where `cb` is a
parameter) has no block. There is no function value in Portal. The corpus has these only
inside the Timers and CallbackHandler helpers, which section 5 inlines away.

---

## 2. null, undefined, `??`, and prefix unary (540 + 74 + 115 + 80 sites)

Rank 2. 809 sites, most of them mechanical.

### Detection

Measured breakdown of `NullKeyword` by parent node over the whole corpus:

| parent | count | runtime? |
| --- | --- | --- |
| `LiteralType` (`T \| null`) | 608 | no, type position |
| binary `=` | 252 | yes |
| `VariableDeclaration` initializer | 190 | yes |
| `ReturnStatement` | 135 | yes |
| binary `!==` | 109 | yes |
| `PropertyDeclaration` initializer | 78 | yes |
| `PropertyAssignment` | 74 | yes |
| binary `===` | 51 | yes |
| `ConditionalExpression` | 31 | yes |
| `AsExpression` | 27 | mixed |
| binary `??` | 16 | yes |
| binary `\|\|` | 11 | yes |

The first rule is therefore: **a null in a type position is not a site.** Test
`ts.isLiteralTypeNode(n.parent)` or any ancestor that `ts.isTypeNode`. That removes
608 of them before any lowering is written.

Prefix unary, measured: `!` 1872, `-` 544, `++` 42, `+` 2, `--` 1. `!` maps to the `Not`
block and `-` on a numeric literal is already folded at `convert.js:4022`. The 80
unconvertible `PrefixUnaryExpression` sites are `-x` where `x` is not a literal, and `++`
/ `--`. Detection: `ts.isPrefixUnaryExpression(e)` with operator `MinusToken` and a
non-literal operand, or `PlusPlusToken` / `MinusMinusToken`.

### Lowering

**`-x`** where `x` is not a literal becomes `Subtract(0, x)`. One extra block, depth +1.

**`++x` / `x++` as a statement** becomes `SetVariable(x, Add(GetVariable(x), 1))`. As an
*expression* it is unconvertible; there is no assignment expression block. All 42 sites
in the corpus are statements or `for` update clauses, which `forFromTs` already reads.

**`!x`** is `Not(x)`. Already handled.

**null itself.** There is no null literal block and no Null value producer. The sentinel
is chosen per static type, and the choice is what avoids colliding with a legitimate 0
or false:

| the null-holding thing is | absent is spelled | test for absent |
| --- | --- | --- |
| an object handle (Player, Vehicle, SpatialObject, InteractPoint, UIWidget, VFX, SFX) | never written; the slot is left unset, or set from a getter that cannot resolve | `Not(IsValid(v))` |
| a number, boolean, string, or enum | a paired presence slot, see below | `Not(GetVariable(present))` |

The object case is the large one and it is exact, because `IsValid` is documented as
"whether a value is defined **and if object reference is valid for object values**"
(`index.d.ts:2419`). `session.ticker !== null` at `Battleship/src/preview/index.ts:187`,
`AmpArsenal.vipBot && mod.IsPlayerValid(AmpArsenal.vipBot)` at
`BF6-Undead-Ground-Zero/src/event-handlers.ts:1115` are both already this shape; the mod
author wrote the validity test by hand next to the null test.

The scalar case needs the pair, because there is no value in the number domain that is
guaranteed absent. `-1` is not safe: `mod.ForcePlayerToSeat(player, vehicle, -1)`
(`index.d.ts:2190`) documents -1 as a meaningful seat number, and
`Battleship/src/testing/index.ts:514` reads `_cursor.get(id) ?? -1`, so -1 is a live
value in the corpus. `0` is obviously unsafe. So:

```
declare  nullable scalar x  ->  two slots:  x        (the value)
                                            x__has   (Boolean)

x = null        ->  SetVariable(x__has, false)
x = e           ->  SetVariable(x, e); SetVariable(x__has, true)
x !== null      ->  GetVariable(x__has)
x === null      ->  Not(GetVariable(x__has))
```

**`a ?? b`.** Lower to `IfThenElse(<a is present>, <a>, b)` where `<a is present>` is the
row above. Because IfThenElse evaluates both arms (`index.d.ts:2405`), `b` must be pure.
Measured LHS shapes over 436 `??` sites:

| LHS shape | count | present-test |
| --- | --- | --- |
| optional chain (`a?.b?.c`) | 212 | see below |
| `map.get(k)` | 78 | the map's own present-test, section 3 |
| plain identifier or property | 75 | the pair, or `IsValid` |
| `opts.X` | 48 | resolved at compile time, section 8 |
| other | 23 | reported |

An optional chain `a?.b?.c ?? d` is a guarded read. Lower it to a nested IfThenElse over
the `IsValid` of each link. Depth grows by 2 per link. Two links is the corpus norm
(`Battleship/src/debug-tool/index.ts:12`, `options?.staticLogger?.visible ?? false`).

**`a || b`** where `a` is boolean is `Or(a, b)` and already works.
`playerCash.get(playerId) || 0` at `BF6-Undead-Ground-Zero/src/helpers.ts:455` is the
falsy-coalescing idiom and is **not** the same as `??`: it also replaces a legitimate
`0`. In this one case the author meant it, but the converter must not silently treat
`||` as `??`. Emit `IfThenElse(<truthy test>, a, b)` with the truthy test being
`Not(Equals(a, 0))` for a number, and report the site so the author can confirm.

### Cost

One extra Boolean slot per nullable scalar. Over the whole corpus that is bounded by the
number of `PropertyDeclaration`/`VariableDeclaration` initialisers set to null with a
non-object type, which is a subset of 268 and mostly Timer ids (see the samples:
`Timers.TimerID | null` appears at `Battleship/src/fleet/index.ts:186`,
`Battleship/src/index.ts:26`, `Battleship/src/perf/index.ts:197`,
`Battleship/src/roles/index.ts:193`). Section 5 removes those, because a timer id
becomes a generation counter that has a natural absent value.

Depth: `+1` for `Not`, `+2` per optional-chain link, `+2` for a `??`.

### Fidelity

Exact for objects. Exact for scalars with the pair. Approximate for `||`, where the
author is told. The author would notice nothing, except that `??` with a side-effecting
right operand now runs that side effect unconditionally; the converter must refuse to
lower a `??` whose right operand contains a call it cannot prove pure, and report it.

### Impossible

`null` as a *value carried through an array element*. Because the pair lives in a
variable, an array of nullable scalars has nowhere to put the presence bit without a
second parallel array. Doable but not worth it; report.

---

## 3. Collections: Map and Set (1,553 sites)

Rank 3 by ratio, rank 1 by absolute count. The hard one.

### 3.1 What is actually there

454 collection declarations across the corpus. Shape distribution, measured:

| shape | count |
| --- | --- |
| `Set<number>` | 139 |
| `Map<number, number>` | 85 |
| `Map<number, mod.Vector>` | 11 |
| `Map<number, boolean>` | 9 |
| `Map<number, mod.UIWidget>` | 7 |
| `Map<number, mod.Player>` | 6 |
| `Map<number, Set<number>>` | 5 |
| `Set<string>` | 4 |
| `Map<string, ...>` | 13 across five value types |
| `Map<number, <a named record type>>` | about 150, each distinct |

Per project: `BF6-Undead-Ground-Zero` 200 (103 Map, 97 Set), `BF_Undead` 68,
`FFA-Gunmaster` 60, `Battleship` 44, `bf6-Deadlock` 26.

**The key is an object id 83% of the time.** Over 2,209 first arguments to
`.get/.set/.has/.delete/.add`, normalising identifiers: `playerId` 753, `id` 380,
`undeadId` 134, `pid` 99, `mod.GetObjId(x)` 93, `zombieId` 48, `victimId` 38,
`barrierId` 29, `botId` 23, `zoneId` 11. 1,835 of 2,209 match an id-shaped name.

**The id usually has a live object next to it.** 360 of 674 `GetObjId` calls in the five
largest projects are exactly `let <name>Id = mod.GetObjId(<expr>)`, for example
`BF6-Undead-Ground-Zero/src/event-handlers.ts:17`, `:127`, `:368`, `:373`. A single-
assignment reaching-definition pass inside one function recovers `<expr>` from the id
local.

**266 of 426 distinct receivers call `.delete`. 141 call `.clear`.** Erasure is not
optional and it is what forces the presence bit.

### 3.2 Detection

Three tiers, in order, and the first that answers wins:

1. **Declared type.** Walk to the declaration through `ts.isVariableDeclaration`,
   `ts.isPropertyDeclaration`, `ts.isParameter`; read `.type`; match
   `/^(Readonly)?(Map|Set|WeakMap|WeakSet)</`. This resolves 439 of 454 declarations in
   the corpus because the projects annotate. `Map` gives `<K, V>`, `Set` gives `<T>`.

2. **Initializer.** No annotation but `= new Map<K, V>()` or `= new Set<T>()`. The other
   15. Read the type arguments off the `NewExpression`.

3. **Method fingerprint,** when the declaration is out of reach (a parameter of an
   untyped helper, `@ts-nocheck` files, `any`). Collect every method called on that
   identifier across the whole flattened bundle and decide:
   - has `.add` and `.has` and no `.get` -> Set
   - has `.get` and `.set` with two arguments -> Map
   - has `.push` or numeric-index access -> Array
   - `.set` with one argument, or `.forEach` only -> ambiguous, report, do not guess

   The discriminator that matters in practice is `.set` arity: `Map.set(k, v)` is two
   arguments, `Array` has no `.set`, and the UI wrappers in this corpus use `.set(v)`
   with one. Second discriminator: `.add` never appears on a Map or an Array.

   Note the false friend: `.delete` is also a UI-widget method. `text.delete()` shows up
   14 times as a receiver in the scan and is `UIText.delete()`, not `Map.delete(k)`.
   Arity again: `Map.delete` takes one argument, the widget method takes none.

### 3.3 The three representations

**Rep A: object slot.** The collection becomes one variable index in the object scope,
addressed as `mod.ObjectVariable(<the key object>, <slot>)`.

```
Set<objId>                slot s, Boolean
  s.add(k)          ->    SetVariable(ObjectVariable(K, s), true)
  s.delete(k)       ->    SetVariable(ObjectVariable(K, s), false)
  s.has(k)          ->    Equals(GetVariable(ObjectVariable(K, s)), true)
  s.clear()         ->    ForVariable i over AllPlayers:
                            SetVariable(ObjectVariable(ValueInArray(AllPlayers(), i), s), false)

Map<objId, V>             slot s (value), slot s+1 (present, Boolean)
  m.set(k, v)       ->    SetVariable(ObjectVariable(K, s),   v)
                          SetVariable(ObjectVariable(K, s+1), true)
  m.get(k)          ->    GetVariable(ObjectVariable(K, s))
  m.has(k)          ->    Equals(GetVariable(ObjectVariable(K, s+1)), true)
  m.delete(k)       ->    SetVariable(ObjectVariable(K, s+1), false)
  m.get(k) ?? d     ->    IfThenElse(Equals(GetVariable(ObjectVariable(K, s+1)), true),
                                     GetVariable(ObjectVariable(K, s)), d)
```

`K` is the object, recovered by the reaching-definition pass above, or synthesised from
the id by the matching getter (`GetPlayer(k)`, `GetSpatialObject(k)`, ...) chosen from
the collection's inferred owner type.

A Map whose values are all objects can drop the presence slot and use
`IsValid(GetVariable(...))` instead, at a cost of one slot saved and a small fidelity
loss: an entry explicitly set to an object that has since despawned reads as absent.
That is usually what the mod wanted anyway.

**Rep B: dense id-indexed array.** One Global array variable; the id is the index.
Applies when the key is a small dense integer that is *not* an object handle: barrier
ids 1..N (`BF6-Undead-Ground-Zero/src/systems/barriers.ts:181`), zone ids, switch ids,
spawner slots.

```
  m.set(k, v)       ->    SetVariableAtIndex(arr, k, v)
  m.get(k)          ->    ValueInArray(arr, k)
  m.has(k)          ->    Not(IsUndefined(ValueInArray(arr, k)))
  m.delete(k)       ->    SetVariableAtIndex(arrPresent, k, false)
  s.add / has / delete   same over a Boolean array
```

The array must be pre-sized at round start, because a write past the end has nowhere to
land. `convert.js:3580` already knows this for packs and says so in its own comment. Emit
the pre-size as N statements, not one nested expression (section 3.5).

**Rep C: parallel key and value arrays.** Two Global array variables, `keys` and `vals`.
For string keys, sparse or unbounded numeric keys, and any collection whose `.size`,
`.forEach`, `.keys()` or `.values()` is used over a non-player key domain.

```
  idx = IndexOfArrayValue(keys, k)                   (block, or IndexOfFirstTrue + Equals)
  m.has(k)          ->    ArrayContains(keys, k)
  m.get(k)          ->    ValueInArray(vals, IndexOfArrayValue(keys, k))
  m.set(k, v)       ->    If Not(ArrayContains(keys, k)):
                            SetVariable(keys, AppendToArray(GetVariable(keys), k))
                            SetVariable(vals, AppendToArray(GetVariable(vals), v))
                          Else:
                            SetVariableAtIndex(vals, IndexOfArrayValue(keys, k), v)
  m.delete(k)       ->    SetVariable(keys, RemoveFromArray(GetVariable(keys), k))
                          and the same index removed from vals, which needs the index
                          captured to a temp first because RemoveFromArray on keys
                          invalidates it
  m.clear()         ->    SetVariable(keys, EmptyArray()); SetVariable(vals, EmptyArray())
  m.size            ->    CountOf(keys)
  m.forEach(f)      ->    ForVariable i from 0 to Subtract(CountOf(keys), 1):
                            <f inlined with ValueInArray(keys,i), ValueInArray(vals,i)>
```

`ArrayContains`, `IndexOfArrayValue` and `RemoveFromArray` are block types with no `mod.*`
function (they are absent from `index.d.ts`; `catalog.json` lists them with a single
`VALUE-1` input and no `params`/`ret`, unlike the real functions). `model.md` already
documents that the converter emits them as runtime helpers on the TypeScript side and
that the site's own exporter desugars them into `IsTrueForAny` / `IndexOfFirstTrue` /
`FilteredArray` with an `Equals` lambda. Both spellings normalise to the same tree.

### 3.4 Which representation, and the defence of the choice

**Recommendation: A first, B second, C last.** Selection procedure, applied per
collection after the whole bundle is read:

```
if key type is an object id  and  the owner object type is known
    and  (.clear() is never called  or  the owner type is Player)
    and  .size / .keys() / .values() / .forEach() is never called
                                      over a non-Player domain:
        Rep A
else if key is a number, every observed key is a literal or a bounded loop counter,
        and the observed maximum is under 256:
        Rep B
else:   Rep C
```

The defence is the variable budget, and it only works out one way.

`BF6-Undead-Ground-Zero` declares 200 collections. Rep C costs two Global slots each:
400 slots against a ceiling of 128. It does not fit, not even close, and `packGlobals`
cannot help because packing explicitly skips array-holding variables
(`convert.js:3437`, `arrayish`). Rep B is the same 400, in the same scope.

Rep A puts them in a *different* scope. The 97 Sets and the ~85 player-keyed scalar Maps
land in the Player scope, which has its own 128, and each one is one or two slots there:
97 Sets at one slot plus 85 Maps at two is 267, still over 128, but the real figure is
much lower because that 200 counts backup directories
(`src/systems/_backup_targeting_v3.7/barriers.ts:180` is a byte copy of
`src/systems/barriers.ts:180`) and counts the same logical collection redeclared in
several functions (`unlockedZones` is declared four times, at
`src/event-handlers.ts:170`, `src/systems/barriers.ts:280`, `:1933`,
`src/systems/spawner.ts:172`, `:755`, `:840`). De-duplicating on the flattened bundle is
the first thing to do and it is a large reduction.

Rep A is also the only one that is O(1). Rep C makes `.has` a linear array scan; the
corpus calls `.has` 449 times, much of it inside per-tick Ongoing rules
(`BF6-Undead-Ground-Zero/src/systems/spawner.ts:623`, `activeUndeadIds` with 24 `.has`
calls). Turning a per-tick hash lookup into a per-tick linear scan over every zombie is
a performance change the author would notice immediately.

Rep A's cost is that the object must be reachable at the call site, which the
reaching-definition pass gives us 360 times out of 674, and the id-to-object getters give
us the rest as long as the owner type is known. Where it is not known, fall to C and say so.

Rep B earns its place for exactly the barrier/zone/switch family: `barrierId` 29 keys,
`zoneId` 11, `switchId` 10, `lightId` 5. These are dense, small, and are not objects.

### 3.5 Worked example

`BF6-Undead-Ground-Zero/src/helpers.ts:451-455`:

```ts
let playerId = mod.GetObjId(player);
if (!playerCash.has(playerId)) {
    playerCash.set(playerId, STARTING_CASH);
}
return playerCash.get(playerId) || 0;
```

`playerCash` is declared `Map<number, number>` at `src/game-state.ts:46`. `.delete` is
called on it at `src/event-handlers.ts:39`, so it needs the presence bit. Key provenance:
`playerId` is single-assigned from `mod.GetObjId(player)` on the line above, so the owner
object is `player`, type Player. Rep A, Player scope, slots `s` and `s+1`.

IR:

```
{ k:'if', branches:[{
    cond: { k:'call', fn:'Not', args:[
             { k:'call', fn:'Equals', args:[
                 { k:'getVar', ref:{ scope:'Player', slot:s+1, object:{k:'arg',index:0} } },
                 { k:'bool', v:true } ] } ] },
    body: [
      { k:'setVar', ref:{ scope:'Player', slot:s,   object:{k:'arg',index:0} },
                    value:{ k:'getVar', ref:STARTING_CASH } },
      { k:'setVar', ref:{ scope:'Player', slot:s+1, object:{k:'arg',index:0} },
                    value:{ k:'bool', v:true } }
    ]}], elseBody: null }

return value:
{ k:'call', fn:'IfThenElse', args:[
    { k:'call', fn:'Equals', args:[
        { k:'getVar', ref:{ scope:'Player', slot:s, object:{k:'arg',index:0} } },
        { k:'num', v:0 } ] },
    { k:'num', v:0 },
    { k:'getVar', ref:{ scope:'Player', slot:s, object:{k:'arg',index:0} } } ] }
```

Blocks: an `If` with a `Not(Equals(GetVariable(ObjectVariable(GetArgument(0), s+1)), true))`
condition, two `SetVariable` statements inside, then an `IfThenElse` for the `|| 0`.

Second worked example, the nested case. `src/game-state.ts:132`,
`let playersInZones: Map<number, Set<number>> = new Map()`, used at
`src/event-handlers.ts:1061-1064`:

```ts
if (!playersInZones.has(playerId)) {
    playersInZones.set(playerId, new Set());
}
playersInZones.get(playerId)!.add(triggerId);
```

An array cannot hold an array (`index.d.ts:2208`), so Rep C cannot represent this at all.
Rep A can, because a *variable* may hold an array: one Player-scoped slot holding a plain
array of zone ids.

```
has(playerId)     ->  Not(IsUndefined(GetVariable(ObjectVariable(player, s))))
set(playerId, {}) ->  SetVariable(ObjectVariable(player, s), EmptyArray())
.add(triggerId)   ->  If Not(ArrayContains(GetVariable(ObjectVariable(player,s)), triggerId)):
                        SetVariable(ObjectVariable(player, s),
                          AppendToArray(GetVariable(ObjectVariable(player, s)), triggerId))
```

and the delete at `:1096` is `SetVariable(ObjectVariable(player, s),
RemoveFromArray(GetVariable(ObjectVariable(player, s)), triggerId))`.

**A `Map<A, Set<B>>` where B is the object and A is not should be inverted instead.** The
same relation read the other way round becomes a B-scoped array of A. `playersInZones`
happens to be keyed the convenient way already. `_occupied: Map<Grid.BoardId, Set<number>>`
at `Battleship/src/fleet/index.ts:175` is keyed the inconvenient way and would invert.

### 3.6 Cost

| representation | Global slots | object slots | depth added per op | op cost |
| --- | --- | --- | --- | --- |
| A, Set | 0 | 1 | 3 (`Equals(GetVariable(ObjectVariable(o,s)), true)`) | O(1) |
| A, Map | 0 | 2 | 3 read, 4 for a `?? d` | O(1) |
| A, `.clear()` | 0 | 0 extra | 4 inside a loop | O(players) |
| B | 1 or 2 | 0 | 2 read, 3 for `has` | O(1), plus an N-statement pre-size |
| C | 2 | 0 | 4 for `get`, 5 for a guarded `set` | O(n) per op |

None of these approaches the depth ceiling of 64 on its own. The ceiling is reached by
composition, and by array literals; see 3.7.

### 3.7 The 79-deep expression

`convert.js:5003` lowers an array literal to nested `AppendToArray`, one level per
element. `BF6-Undead-Ground-Zero/src/config.ts:401` declares
`const EMERGENCY_LIGHT_IDS: number[]` with 78 elements. `EmptyArray` plus 78
`AppendToArray` is an expression 79 deep against a ceiling of 64. That is the reported
case, and it is a real refusal, not a warning.

The fix already exists inside the same file for a different purpose. `convert.js:3583`
carries the reasoning verbatim: "ONE STATEMENT PER ELEMENT, NOT ONE EXPRESSION PER PACK.
[...] The same array built by assigning to itself repeatedly is a flat list of
statements, each two deep, and has no depth limit at all."

So: an array literal in a **module-level initialiser** becomes a variable plus N
statements at round start, exactly as `packGlobals` does. Each statement is
`SetVariable(v, AppendToArray(GetVariable(v), <elem>))`, depth 3, and there is no limit
on how many. An array literal in an **expression position** keeps the nested form, but
the converter must count depth as it builds and spill to a temp
(`tempRef`, `convert.js:3973`) once the running depth passes about 48, leaving headroom
for whatever the literal is nested inside.

Whether `SetVariableAtIndex` can grow an array past its current length is not settled by
the typings (`index.d.ts:345` says only "Sets the value at the specified index"). If it
can, Rep B's pre-size and this whole spill are cheaper by a factor of three. That is the
experiment in section 13.

### 3.8 Fidelity

Rep A is exact for `set`, `get`, `has`, `delete` and, on a Player key domain, for `clear`
and `forEach`. Insertion order is lost: a Map in TypeScript iterates in insertion order,
Rep A iterates in `AllPlayers()` order. 120 `.forEach` sites are affected. In every one I
read the body is order independent (`src/systems/classes.ts:79`,
`src/helpers.ts:257`, `src/systems/power-ups.ts:300`), but the converter cannot prove
that and should say so once per collection rather than once per site.

Rep C preserves insertion order exactly and is the fallback when order matters.

`.sort` (12 sites in the unconvertible set, 18 in the corpus) maps onto `SortedArray`,
which takes a *number* to sort by, ascending, with `CurrentArrayElement` standing for the
element (`index.d.ts:2232`). A comparator of the form `(a, b) => a.x - b.x` lowers to
`SortedArray(arr, <x of CurrentArrayElement>)`. Any other comparator does not lower.

`.splice` (17 sites): only the two-argument delete form maps, onto `ArraySlice` twice plus
a concatenation. The insert form does not; report it.

### 3.9 Impossible

- A Map keyed by an object that is neither a Portal object nor a number nor a string.
  `Map<any, mod.Vector>` at `src/game-state.ts:139`, `spawnedFxPositions`, keyed by the
  spawned FX handle itself. If the handle is a real Portal object this is Rep A; if it is
  a JS object it has no id and no encoding. Report and let the author retype it.
- `Map<number, Map<number, number>>` at `bf6-Deadlock/src/index.ts:2345`,
  `damageContributors`. Two levels of association where neither level is a Player.
  Arrays cannot nest, and one object slot can hold one array, not an array of arrays.
  The only encoding is Rep C with a composite string key, which is slow and loses the
  ability to enumerate one contributor set. Name it and refuse.
- `WeakMap` / `WeakSet`. No occurrences in the corpus, but if one appears there is no
  weak reference in Portal; it becomes a strong Map and leaks.

---

## 4. Classes and `new` (465 sites, of which roughly 50 are not the UI library)

Rank 7. Low sites, high risk, but unavoidable for two projects.

### 4.1 What is there

66 class declarations. 772 `new` sites, distributed:

| constructor | sites | disposition |
| --- | --- | --- |
| `Map` | 301 | section 3 |
| `Set` | 185 | section 3 |
| `UIText`, `UIContainer`, `UITextButton`, `UI.Text`, `UI.Container` | 176 | the UI library, another agent |
| `Logger` | 26 | debug only, see 4.5 |
| `Promise` | 9 | impossible, see 4.5 |
| `Proxy`, `Error`, `Array` | 8 | impossible / trivial |
| everything else | 67 | this section |

The 67 split into two populations that need different treatment.

**Singletons.** `new SceneManager()` (`Hospital-Trailer/src/index.ts:944`),
`new DirectorManager()` (`:1367`), `new CoreAI_Perception()`
(`TDM/src/index.ts:527`): constructed once, stored in one module-level binding. 15 classes
in the corpus have exactly one `new` site and no map of instances.

**Multi-instance.** `Barrier` (`BF6-Undead-Ground-Zero/src/systems/barriers.ts:180`, 23
fields, 18 methods, held in `private static _instances: Map<number, Barrier>` at `:181`,
keyed by `barrierId`), `BotBrain` (`bf6-Deadlock/src/bot-ai/brain.ts:59`, 14 fields, held
in `Map<number, BotBrain>` at `:458` keyed by bot id), `PlayerProfile`
(`BF6-Undead-Ground-Zero/src/player-profile.ts:9`, 29 fields, `Map<number, PlayerProfile>`
at `:11` keyed by player), `PowerUp`, `UndeadProfile`, `CountdownUI`, `TeamHealthUI`.

The multi-instance ones are the interesting case and they all have the same shape:
**a class plus a static Map from an id to the instance.** That is not a coincidence; it is
what people write when the identity of the thing is an engine object.

### 4.2 Detection

- `ts.isClassDeclaration(n)`. Members split by `ts.isPropertyDeclaration` /
  `ts.isMethodDeclaration` / `ts.isConstructorDeclaration`, and by whether
  `n.modifiers` contains `StaticKeyword`.
- Instance count: find every `ts.isNewExpression` whose `expression` resolves to this
  class in the flattened bundle. If the count is one and the result is stored in a
  module-level binding that is never reassigned, it is a singleton.
- Instance addressing: find the static property whose declared type is
  `Map<K, ThisClass>` and whose `.set` is called from inside the constructor
  (`barriers.ts:207`, `Barrier._instances.set(barrierId, this)`). `K` is the instance id
  type and the constructor parameter that flows into that `.set` is the id.

### 4.3 Lowering

**Singleton.** Every instance field becomes a Global variable named `<Class>_<field>`.
Every method becomes a subroutine `<Class>_<method>`. `this.x` is a plain read of
`<Class>_x`. `this` never appears as a value. Cost: one Global per field. This is exactly
what the existing `NOT CONVERTED` suggestion in `model.md` already tells the author to do
by hand ("flatten into subroutines plus scoped variables"); we are automating it.

**Multi-instance: struct of arrays, addressed by the instance id.**

Do not try to make an instance a value. An instance cannot be a value: an array cannot
hold an array (`index.d.ts:2208`), so a record-of-records is unrepresentable, and
`Barrier` has an `undeadInZone: Set<number>` field (`barriers.ts:191`) that is itself a
collection.

Instead, **one variable per field, and the instance id is the index or the object**:

```
class Barrier, instance id = barrierId, 1..BARRIER_COUNT, dense, not an object
    -> Rep B addressing.  One Global ARRAY variable per field:

       Barrier__planksIntact      array, index = barrierId
       Barrier__breakProgress     array, index = barrierId
       Barrier__position          array, index = barrierId
       Barrier__undeadInZone      array of arrays  <-- NOT REPRESENTABLE, see below
       ...

    this.planksIntact         ->  ValueInArray(Barrier__planksIntact, GetArgument(0))
    this.planksIntact = e     ->  SetVariableAtIndex(Barrier__planksIntact, GetArgument(0), e)
    Barrier.get(id).method()  ->  subroutine Barrier_method with argument 0 = id
    new Barrier(id)           ->  subroutine Barrier_ctor(id), which writes every field
                                  its default
```

```
class PlayerProfile, instance id = a player object
    -> Rep A addressing.  One PLAYER-SCOPED variable per field, no arrays at all:

    this.cash                 ->  GetVariable(ObjectVariable(GetArgument(0), s_cash))
    this.cash = e             ->  SetVariable(ObjectVariable(GetArgument(0), s_cash), e)
```

The addressing scheme in one line: **`this` is not stored, it is passed.** Every method
becomes a subroutine whose argument 0 is the instance id (Rep B) or the instance object
(Rep A), and every field access is one indexed read against a per-field variable. The
existing `fieldSlots` machinery (`convert.js:1721`) is the wrong tool here, because it
lays fields out *within one array*, which is the record encoding; classes need the
transpose.

**Static fields** are plain Globals, not arrays. `Barrier._initialized` (`barriers.ts:182`)
is one Boolean Global.

**Static methods** are subroutines with no instance argument. `AmpArsenal` has 78 static
members and zero instances (`systems/amp-arsenal.ts:83`); it is a namespace wearing a
class, and it lowers exactly like section 1.

**Inheritance.** No `extends` in the corpus outside `node_modules`. If one appears, flatten
the base fields into the derived class's field set and duplicate the base methods per
derived class. There is no dynamic dispatch and there can be none.

### 4.4 Cost

This is the expensive family and the numbers should be stated plainly.

`Barrier` has 23 instance fields. Rep B is 23 Global array variables, plus the pre-size
statements. `PlayerProfile` has 29 fields; Rep A is 29 Player slots out of 128.
`CountdownUI` has 21 fields, `TeamHealthUI` 25, `BotBrain` 14, `PowerUp` 25.

Two classes with 25 fields each in the Player scope is 50 of 128, and section 3 already
wants most of the rest. **A project with more than about three multi-instance classes
does not fit and the converter must say so before it starts, not after.**

Mitigation that is worth doing: a field never written after construction and initialised
from a constant is a compile-time constant, not a variable. `Barrier.config`
(`barriers.ts:184`, `readonly config: typeof BARRIER_CONFIG[0]`) and `Barrier.zone`
(`:183`) are both derived from `BARRIER_CONFIG` by id, so both fold into a constant
lookup array shared by all instances. That is 2 of 23 recovered on one class, and the
`readonly` keyword makes the analysis trivial.

Depth: `+2` per field read (Rep B) or `+3` (Rep A). Nothing dangerous.

### 4.5 Fidelity and impossible

- **A field whose type is itself a collection is not representable in Rep B.**
  `Barrier.undeadInZone: Set<number>` and `Barrier.playersInZone: Set<number>`
  (`barriers.ts:191-192`), and the four array fields `plankObjects`,
  `originalPositions`, `originalRotations`, `originalScales` (`:186-189`). An array of
  arrays is forbidden. The only route is to invert: `undeadInZone` keyed by barrier
  becomes `barrierOfUndead` keyed by the undead player, one Player slot, and the
  membership test flips from `barrier.undeadInZone.has(u)` to
  `Equals(GetVariable(ObjectVariable(u, s_barrier)), barrierId)`. That inversion is
  exact for a partition (each undead is in at most one barrier zone) and wrong for an
  overlap. `plankObjects` is a fixed-length list of spatial objects per barrier and
  inverts to one SpatialObject-scoped slot holding the owning barrier id.
- **`new Promise`** (9 sites). No block. Every one in the corpus is inside the bundled
  `bf6-portal-utils` helpers and disappears when those helpers are inlined against
  `mod.Wait`. If a user-written Promise appears, it is impossible; say so.
- **`new Proxy`** (3 sites). No block, no workaround. Impossible.
- **`new Error`** (2 sites). There is no throw. The `throw` disappears with the try/catch
  and the Error construction is dead. Drop silently.
- **`new Logger`** (26 sites) and `DebugTool` (12 sites, one per project, all byte
  identical at `<project>/src/debug-tool/index.ts:6`). These are development scaffolding
  behind a compile-time flag. **Recommendation: give the converter a `--strip-debug` mode
  that treats a class named in a debug allowlist as dead and elides every call to it.**
  That closes 38 `new` sites and a long tail of method calls for no semantic risk, and it
  is honest because the released mod runs with the flag off anyway.
- **An instance escaping its id.** If an instance is passed as an argument, stored in a
  field of another instance, or compared by identity, the flattening breaks, because the
  instance has no value. Detect it (a `NewExpression` result or a `this` flowing anywhere
  other than a field access or an id-keyed map) and refuse rather than emit something
  that half works.

---

## 5. Timers.setTimeout and setInterval (96 + 55 sites)

Rank 4. Not in the brief's family list by name, but 96 of the 644 cross-module calls are
`Timers.setTimeout` and they behave nothing like an ordinary cross-module call, so they
need their own lowering.

### Detection

`ts.isCallExpression` with callee `Timers.setTimeout` / `setInterval` / `clearTimeout` /
`clearInterval` / `clear`, where `Timers` resolves to the bundled
`bf6-portal-utils/timers` namespace (`BF6-Undead-Ground-Zero/dist/bundle.ts:894`).

### Lowering

The helper's own source, quoted from
`BF_Undead/_reference/night-of-the-undead/SOURCE/node_modules__bf6-portal-utils__timers__index.ts`,
is 50 lines and is built entirely on `mod.Wait` and a `Set<number>` of live ids:

```ts
export function setTimeout(callback, ms): number {
    const id = nextId++
    ACTIVE_IDS.add(id)
    executeTimeout(id, callback, ms < 0 ? 0 : ms)
    return id
}
async function executeTimeout(id, callback, ms) {
    await mod.Wait(ms / 1_000)
    if (!ACTIVE_IDS.has(id)) return
    ACTIVE_IDS.delete(id)
    callback()
}
```

So the lowering is a direct transcription, not an invention:

```
Timers.setTimeout(() => BODY, MS)

  ->  a subroutine  timer_<n>  containing:
        Wait(MS / 1000)
        If Equals(GetVariable(<gen slot>), <the generation captured at scheduling>):
            BODY
      and at the call site:
        SetVariable(<gen slot>, Add(GetVariable(<gen slot>), 1))
        <subroutine call timer_<n>>
```

The cancellation token is a **generation counter**, not a live-id set: one Number slot
per timer site. `clearTimeout(id)` becomes `SetVariable(<gen slot>, Add(GetVariable(<gen slot>), 1))`,
which makes every outstanding wait fail its guard. That removes the `Timers.TimerID | null`
nullable entirely (section 2), because "no timer" is just "the guard will not match".

`setInterval` is the same with a `While` around the body and the guard re-tested each
iteration:

```
  ->  subroutine interval_<n>:
        While Equals(GetVariable(<gen slot>), <captured generation>):
            Wait(MS / 1000)
            If Not(Equals(GetVariable(<gen slot>), <captured>)): Break
            BODY
```

Worked example, `BF6-Undead-Ground-Zero/src/systems/perks.ts:84-89`:

```ts
Timers.setTimeout(() => {
    if (spawnedFxObjects.has(explosionFx)) {
        spawnedFxObjects.delete(explosionFx);
        safeUnspawn(explosionFx);
    }
}, 2000);
```

becomes a subroutine `timer_perkExplosion` taking one argument (the FX object, because
the closure captures `explosionFx`):

```
Wait(2)
If Equals(GetVariable(ObjectVariable(GetArgument(0), s_spawnedFx)), true):
    SetVariable(ObjectVariable(GetArgument(0), s_spawnedFx), false)
    <sub safeUnspawn>(GetArgument(0))
```

no generation slot at all, because nothing ever cancels this one. The converter should
only allocate the generation slot when a matching `clearTimeout` / `clearInterval` exists,
which is measurable: 21 `clearTimeout` and 53 `clearInterval` sites against 202 and 55
schedulings, so most timers need no token.

**Captured variables become subroutine arguments.** This is the general rule for closures
and it is what makes them convertible at all. See section 7.

### Cost

One subroutine per call site. Zero or one Number slot per site, depending on
cancellability. Depth +1 for the guard.

Subroutine count matters: 202 `setTimeout` sites in the corpus would be 202 subroutines.
There is no documented subroutine ceiling, but the Portal error message quoted in section
0 references a `SubroutineIndexInJson`, and one Discord author reports shrinking a
workspace by 110 KB purely by shortening block ids
(`research/_tools/discord/topics/cand-ai.md:1615`), so workspace size is a real limit even
if the subroutine count is not. Deduplicate identical timer bodies.

### Fidelity

**A `Wait` inside a subroutine does not release the caller's stack frame.** This is the
single most important fidelity note in this document and it follows directly from the
512-frame recursion cap in section 0: the author who wrote "at the end it Waits 1 and
then Calls itself" hit the cap after roughly 512 seconds. A JavaScript `setTimeout` returns
immediately and the callback runs on a fresh stack; a Portal subroutine that waits keeps
its frame. Any timer that reschedules itself, and any interval, accumulates depth.

The `While` form for `setInterval` above is deliberate for exactly this reason: it is a
loop in one frame, not a chain of frames, so it does not accumulate. **A self-rescheduling
`setTimeout` must be rewritten into that `While` form, or it will die after about 512
iterations.** Detect it: the timer body contains a `setTimeout` whose target is the same
body.

Ordering also differs. In TypeScript, `setTimeout(f, 0)` runs after the current
synchronous block. Here the subroutine call runs inline at the scheduling point until it
hits its `Wait`, so `Wait(0)` yields at a different moment. `mod.Wait(0)` is legal
(`index.d.ts:57`, "can be fractional") but its yield semantics are not documented; treat
`setTimeout(f, 0)` as a plain inline call and report.

### Impossible

`Timers.getActiveTimerCount()` has no equivalent. Zero sites in the corpus.

---

## 6. Template expressions (107 sites)

Rank 5. Cheap, mechanical, well bounded.

### Detection

`ts.isTemplateExpression(e)`, with `e.head`, `e.templateSpans[].literal.text` and
`e.templateSpans[].expression`. `ts.isNoSubstitutionTemplateLiteral` is already a plain
string at `convert.js:4028` and is not a site.

Measured substitution counts over 519 template expressions in the corpus:
1 sub 278, 2 subs 161, 3 subs 60, 4 subs 15, 5 subs 5.

### Lowering

Portal's string system is a table plus positional placeholders. `strings.json` holds
`"deployedOnMap": "You are {} on {}"` (`BF6-Undead-Ground-Zero/src/strings.json`) and
`mod.Message(key, arg0, arg1, arg2)` fills the `{}` in order (`index.d.ts:2833`).

So a template becomes **a new strings.json entry plus a Message call**:

```
`sank ${sunk} markers ${SINK_DEPTH}m`          Battleship/src/calibration/index.ts:143

  -> strings.json:  "gen_0041": "sank {} markers {}m"
  -> Message(stringkeys.gen_0041, <sunk>, <SINK_DEPTH>)
```

The interned string table already exists: `model.md` documents `strings.json` as "the
interned string table, `s0..sN` in order of first appearance, the same scheme the site
emits". Template lowering writes into the same table.

Two templates that differ only in their substitutions share one entry, which matters
because the corpus repeats debug prefixes: `` `[BotBrain] ${msg}` `` at
`bf6-Deadlock/src/bot-ai/brain.ts:29` and `` `[CountdownUI] ${msg}` `` at
`gunfight/ui/countdown-ui.ts:35` are different entries, but the same template inside a
loop is one.

### Cost

Zero variables. Depth: one `Message` block with up to four inputs, so `+1` plus whatever
the substitution expressions cost. One new strings.json row per distinct shape.

### Fidelity

Exact for 1 to 3 substitutions, which is 499 of 519 sites.

Substitution values must be `string | number | Player` (`index.d.ts:2833`). A substitution
that is a boolean, a Vector, or an object is not accepted. `String(raw !== undefined && raw !== null)`
at `Battleship/src/diagnostics/index.ts:81` is a boolean stringified by hand and must
become `IfThenElse(cond, "true", "false")`.

Nested method calls inside a substitution are fine as long as they lower:
`` `${Grid.nameOf(cell)} measured ${off.toFixed(2)}m ...` `` at
`Battleship/src/calibration/index.ts:76` needs `Grid.nameOf` (section 1, fine) and
`Number.prototype.toFixed` (no block, see below).

### Impossible

- **More than three substitutions.** 20 sites (15 with four, 5 with five). `Message` caps
  at three and `msgArg` is not itself a `Message`, so they cannot nest. The honest
  options are to split into two messages, or to drop the least important substitution and
  report. Do not invent a concatenation: there is no `mod.*` string concatenation
  function at all. `catalog.json` does carry `text_join`, but that is one of the stock
  Blockly blocks the catalogue dump picked up alongside `lists_*`, `math_*` and
  `variables_*`, it has no `params`/`ret` entry the way every real Portal block does, and
  nothing in the corpus or in the site's own export uses it. Treat it as unavailable
  until the experiment below says otherwise.
- **`.toFixed(n)`, `.padStart`, `.toUpperCase`** and the rest of `String.prototype`. No
  `mod.*` function. `catalog.json` lists `text_changeCase`, `text_trim`,
  `text_getSubstring` and friends under the same stock-Blockly caveat. Report.

  **Experiment worth running before writing either of these off.** Place a `text_join`
  and a `text_changeCase` block in a scratch workspace and deploy it. The Portal site
  type checks uploads, so a rejection is a definite answer and an acceptance opens up
  string handling considerably, including the four-substitution templates above. This is
  cheap and nobody has tried it.

---

## 7. Closures with a statement body (48 sites)

Rank 8. Small, and smaller than it looks.

### 7.1 The measurement changes the problem

618 statement-bodied closures in the corpus. Where they are passed:

| callee | count |
| --- | --- |
| `Timers.setTimeout` | 160 |
| unclassified parent (not a direct call argument) | 150 |
| `Timers.setInterval` | 41 |
| an array method (`filter`/`map`/`some`/`every`/`find`/`findIndex`/`sort`/`forEach`/`reduce`) | 119 |
| `Events.*.subscribe` | 30 |
| everything else | 118 |

**Only 119 of 618 go to an array method at all**, and of those, 89 have exactly one
statement, which is almost always `return <expr>;` and reduces to an expression with no
work. The genuinely hard population is **30 closures**: 18 with two statements, 7 with
three, 2 with four, and one each with five, six and eleven.

The 201 timer closures are section 5. The 30 subscribe closures are rule bodies and are
already the converter's core case.

### 7.2 Detection

`(ts.isArrowFunction(n) || ts.isFunctionExpression(n)) && ts.isBlock(n.body)`, then
classify by `n.parent`: if `ts.isCallExpression(n.parent)` and the callee is a property
access whose name is in the array-method set and whose receiver is array-typed
(section 3.2), it is an array lambda.

Reduce first, always: a block body whose only statement is `ts.isReturnStatement` becomes
that expression. That is 89 of 119 for free.

### 7.3 Lowering: the accumulate loop

`FilteredArray` and `MappedArray` take one expression evaluated per element with
`CurrentArrayElement` standing for the element (`catalog.json`, and `model.md` on
`LAMBDA_ARG`). A multi-statement predicate has no expression to give them.

**Yes, it rewrites as an explicit loop.** The rewrite is mechanical:

```
const OUT = SRC.filter(EL => { S1; S2; return E; });

  ->  SetVariable(OUT, EmptyArray())
      ForVariable i from 0 to Subtract(CountOf(SRC), 1):
          SetVariable(EL, ValueInArray(SRC, i))
          S1
          S2
          If E:
              SetVariable(OUT, AppendToArray(GetVariable(OUT), GetVariable(EL)))
```

An early `return false` inside the predicate becomes `Continue`; an early `return true`
becomes the append followed by `Continue`. `.map` is the same shape with the projection
appended instead of the element. `.some` / `.every` become a Boolean accumulator plus
`Break`. `.find` becomes the element plus `Break`. `.findIndex` becomes the counter plus
`Break`. `.forEach` is the loop with no accumulator.

Worked example, `BF6-Undead-Ground-Zero/src/event-handlers.ts:973-977`:

```ts
let availableUndead = Array.from(undeadInZones).filter(id => {
    if (trackedUndead.has(id)) return false;
    let state = undeadBarrierStates[id];
    return state && state.phase === "hunting";
});
```

`undeadInZones` is `Set<number>` at `src/game-state.ts:84`, `trackedUndead` is `Set<number>`
at `:83`. Both are keyed by an undead player id, so both are Rep A, Player scope, one
Boolean slot each. `Array.from(aSet)` over a Player-keyed Rep A set is
`FilteredArray(AllPlayers(), <the set's slot is true for CurrentArrayElement>)`.

```
SetVariable(availableUndead, EmptyArray())
SetVariable(src, FilteredArray(AllPlayers(),
                   Equals(GetVariable(ObjectVariable(CurrentArrayElement(), s_inZones)), true)))
ForVariable i from 0 to Subtract(CountOf(GetVariable(src)), 1):
    SetVariable(el, ValueInArray(GetVariable(src), i))
    If Equals(GetVariable(ObjectVariable(GetVariable(el), s_tracked)), true):
        Continue
    SetVariable(state, ValueInArray(GetVariable(undeadBarrierStates), GetObjId(GetVariable(el))))
    If And(Not(IsUndefined(GetVariable(state))),
           Equals(ValueInArray(GetVariable(state), <fieldSlots.phase>), "hunting")):
        SetVariable(availableUndead,
                    AppendToArray(GetVariable(availableUndead), GetVariable(el)))
```

That is a faithful, readable block program. Note that the whole thing became a
*statement sequence*, which means it can only appear where a statement can appear. A
filter used inside a larger expression must be hoisted to a temp above the statement it
occurs in, which is the standard three-address lowering and is what `tempRef`
(`convert.js:3973`) exists for.

### 7.4 Cost

Three variables per loop: the source, the element, and the output. All three are frame
variables and go through `notePoolable` (`convert.js:3963`), so they share slots with any
other loop that cannot be live at the same time. Nested filters need distinct sets.

Depth: the body statements keep their own depth; the loop adds nothing. This lowering
*reduces* depth compared with a nested-lambda form.

### 7.5 Fidelity

Exact, with one exception. `SortedArray` takes a sort *key*, not a comparator
(`index.d.ts:2232`), and there is no way to write a sort as an accumulate loop without
implementing a sort by hand in blocks. A comparator of the form `(a, b) => a.k - b.k`
lowers to `SortedArray(arr, <k of CurrentArrayElement>)`; anything else does not lower.
12 `.sort` sites in the unconvertible set.

`.reduce` (4 sites) is an accumulate loop with an explicit accumulator variable and works
the same way.

### 7.6 Impossible

- A closure stored in a variable and called later. `Map<number, Array<() => void>>` at one
  site in the corpus. No function values.
- A closure that captures a variable it also mutates, where the closure outlives the
  enclosing function. The captured variable becomes a subroutine argument, which is
  by-value, so the mutation is lost. Detect the write-to-captured pattern and report.

---

## 8. Expression statements and bare call expressions (201 + 110 sites)

Rank 6. Mostly not a real family.

### Detection

`convert.js:4204` returns `unconvertibleStmt(..., 'call expression')` for any
`ts.isCallExpression` whose callee is neither `mod.*`, `rt.*`, `modlib.*`, `subs.*`, nor a
bare identifier. `convert.js` reports `expression statement` for a
`ts.isExpressionStatement` whose expression is not a call at all.

### What they actually are

- Calls through a namespace: **section 1 closes these** and they are already counted in
  the 644, so there is double counting between the families.
- Calls on an options-object helper: `GameUI.text(...)`,
  `BF6-Undead-Ground-Zero/src/systems/perks.ts:229`, 62 sites. `GameUI` is a class of
  static methods declared at `src/ui/interact-ui.ts:29` whose parameters are option bags
  with `??` defaults for every field (`:60` and `:68-82`). Every call site passes an object
  *literal*, so **every default resolves at compile time**. Inline the method with the
  literal's fields substituted and the missing fields folded to their defaults, and both
  the `??` and the option bag vanish. This is a static-shape specialisation and it is
  exact.

  Worked example, `src/systems/perks.ts:229-234`:

  ```ts
  const text = GameUI.text("upgraded_hud_text_" + playerId + "_" + uid, ampedMessage, {
      pos: GameUI.vec(0, 0), size: GameUI.vec(250, 28),
      anchor: mod.UIAnchor.Center, parent: container,
      fontSize: 14, color: GameUI.rgb(0, 0, 0),
      textAnchor: mod.UIAnchor.Center, receiver: player
  });
  ```

  inlines to one `AddUIText(...)` call plus one `FindUIWidgetWithName(...)`, with
  `visible = true`, `padding = 0`, `bgColor = (0,0,0)`, `bgAlpha = 0`,
  `fill = UIBgFill.None`, `alpha = 1`, `depth = UIDepth.AboveGameUI` all folded in from
  the `??` defaults at `src/ui/interact-ui.ts:68-82`.

- Assignment expressions used as statements. `model.md` already lists these
  ("there is no assignment block, only SetVariable"). `x = e` where `x` is a workspace
  variable is `SetVariable`; `x.f = e` where `x` is a record is `SetVariableAtIndex`;
  `a[i] = e` is `SetVariableAtIndex`. All three are straightforward and none is
  implemented today. `obj.f = e` where `obj` is a class instance is section 4.
- `console.log` (42 sites). 15 in `BF6-Undead-Ground-Zero`, and in `bf6-Deadlock` every
  one is guarded: `if (DEBUG_BRAIN) console.log(...)` at `src/bot-ai/brain.ts:29`,
  `if (DEBUG_COUNTDOWN)` at `gunfight/ui/countdown-ui.ts:35`,
  `if (DEBUG_ELIMINATION)` at `gunfight/ui/elimination-ui.ts:14`. There is no console
  block. **Drop them under `--strip-debug`** (section 4.5); with the flag off, map to
  `DisplayHighlightedWorldLogMessage(Message(...))` and report the change of medium.

### Cost, fidelity, impossible

Inlining an options helper costs nothing and is exact. Assignment lowering costs nothing.
`console.log` with a rest parameter (`...args`, section 10) cannot carry its arguments
and loses them.

---

## 9. Date.now (41 sites)

Rank 9 by count, rank 1 by ratio if you only count the ratio. Do it first because it takes
an hour.

### Detection

`ts.isCallExpression(e) && e.expression.getText() === 'Date.now'`. Also catch
`new Date().getTime()`; zero sites in the corpus but it is the same thing.

### Lowering

`Date.now()` -> `Multiply(GetMatchTimeElapsed(), 1000)`.

`mod.GetMatchTimeElapsed()` is documented as "the amount of time elapsed (seconds) in
the current gamemode" (`index.d.ts:2263`). Date.now is milliseconds since the epoch, so
the scale factor is 1000 and the epoch differs.

### Cost

Zero variables, depth +2.

### Fidelity

**Exact for differences, wrong for absolutes.** Every use in the corpus is a difference or
a deadline:

- `BF6-Undead-Ground-Zero/src/helpers.ts:62`, `:86`: `const now = Date.now();` then a
  subtraction against a stored earlier `now`.
- `src/systems/perks.ts:923`: `m.cooldownUntil = Date.now() + 10000;` then a later
  comparison against `Date.now()`.
- `Battleship/src/perf/index.ts:97`, `:129`, `:137`, `:140`: interval measurement.

The epoch offset cancels in every one of those, so the lowering is exact for all 41.

It resets. `GetMatchTimeElapsed` is scoped to the current gamemode, so a value stored in
round N and compared in round N+1 goes negative. In practice the state that holds these
is cleared at round start anyway, but say it.

Resolution is whatever the engine's per-frame time gives; it is not a millisecond clock,
so two `Date.now()` calls in the same tick return the same value. Any code relying on a
strictly increasing timestamp within a tick breaks. None in the corpus.

### Impossible

Wall-clock date, `toISOString`, anything calendar shaped. Zero sites.

---

## 10. Spread (68 sites)

Rank 10.

### Detection

`ts.isSpreadElement(n)` (array and call argument positions) and
`ts.isSpreadAssignment(n)` (object literal).

### Lowering by position

| position | example | lowering |
| --- | --- | --- |
| `[...a]` alone | `[..._aboard]`, `Battleship/src/round/index.ts:542` | the array itself, a copy is a no-op because Portal arrays are values (`AppendToArray` doc: "Returns a **copy** of an array") |
| `[...a, ...b]` | `[...(team1 ?? []), ...(team2 ?? [])]`, `bf6-Deadlock/src/gunfight/ui/countdown-ui.ts:291` | `AppendToArray(a, b)`, because appending an array concatenates (`index.d.ts:2208`); this is the one place the no-nesting rule helps |
| `[...a, x]` | `[...(_plans.get(id) ?? []), ...lengths]`, `Battleship/src/fleet/index.ts:68` | `AppendToArray(a, x)` |
| `arr.push(...b)` | `remaining.push(...source)`, `bf6-Deadlock/src/gunfight/loadout.ts:405` | `SetVariable(arr, AppendToArray(GetVariable(arr), b))` |
| `[...aSet]` | `[..._sessions.values()]`, `Battleship/src/preview/index.ts:204` | the collection's own iteration form, section 3 |
| `Math.max(...a)` | `Math.max(...targets.map(_ring))`, `Battleship/src/wall-animator/index.ts:209` | a fold loop, section 7 |
| `{ ...o, k: v }` | none in the corpus outside node_modules | record layout is fixed by `fieldSlots`; a spread has no fixed field set, which is exactly why `recordToIr` returns null at `convert.js:4746`. Impossible |
| `f(...args)` in a rest-forwarding wrapper | `console.log(msg, ...args)`, `bf6-Deadlock/src/bot-ai/brain.ts:29` | impossible; no variadic call |

### Cost

Zero variables for the array cases. Depth +1 per concatenation.

### Fidelity

Exact for the array cases, and pleasingly so, because Portal array append is already
copy-on-write. Impossible for object spread and for variadic forwarding.

---

## 11. Catch clause with a body (196 sites)

Rank last. **Do not implement this. It is already correct.**

`convert.js:4230` handles try/catch today and handles it right. The comment at
`convert.js:4210` states the reasoning and it holds: an empty catch means "run this and
carry on if it fails", blocks carry on regardless, so emitting the try block alone is
faithful. A catch with a body is a promise that runs only on failure, and there is no
failure to test for, so the body and the finally are kept and the handler is dropped with
a report.

The measurement supports leaving it alone: of 1,898 catch clauses in the corpus,
**1,572 have an empty body**. 288 have one statement, 29 have two, 7 have three, 2 have
four. The empty ones are already handled at zero cost. The 326 non-empty ones are almost
all a log call (`Log.error(...)`, `console.log(...)`) that section 8 elides anyway.

Anything you could build here would be a guess at what the author wanted to happen on a
failure that cannot occur. **The report is the right output. Leave it.**

The only worthwhile change is to stop counting these as "unconvertible". They are
converted, with a documented loss. Move them to `report.warnings` so the 9,297 headline
drops by 196 without a line of lowering being written.

---

## 12. Recursion: settled

**A Portal subroutine may call itself. The call stack is capped at 512 frames and a
`Wait` does not unwind it.**

Evidence, and it is direct rather than inferred. A Portal author describing their own
running workspace, `research/_tools/discord/topics/cand-scripting.md:9693`:

> i'm running into an issue where i'm trying to get a subroutine to run on a loop every
> second (at the end it Waits 1 and then Calls itself) but it dies after a while with the
> error "Callstack reached depths of 512 that exceeds max allowed depth of 512". is there
> a workaround to get it to keep running?

Three facts follow from that one message. The self-call was *accepted* by the editor and
by the runtime, so recursion is not rejected. It *ran* for a long time before failing, so
each iteration worked. And it failed at exactly 512 frames after roughly 512 seconds of
one-second waits, so **the awaited call did not return and did not release its frame**.

The negative half of this search has a control. The same corpus discusses subroutines
constantly (`cand-ai.md:1615`, `:1769`, `:3989`, `:4601`, `:4648`, `:6636`, `:9231`,
`:10290`, `:12662`, `:13121`; `cand-perf.md:1481`), so a search that returns nothing for
"recursive subroutine" as a *phrase* is a real absence of discussion, not an absence of
coverage. Nobody in the corpus discusses recursion as a technique, and one person hit its
limit by accident.

### Consequences for this design

- Bounded recursion up to a few hundred frames is safe. Nothing in the corpus recurses
  deliberately.
- Any lowering that produces a self-rescheduling chain is a bug. Section 5's
  `setInterval` lowering uses `While` inside one frame for this reason, and a
  self-rescheduling `setTimeout` must be detected and rewritten to the same shape.
- Mutual recursion between generated subroutines has the same cap. The converter should
  build the subroutine call graph it already builds for `needsEventInfo`
  (`convert.js:385`, a fixed point over `subCall`) and warn on any cycle.

### What would settle it further

Not needed for the design, but if a hard number for a non-waiting recursion is ever
wanted: build a workspace with one subroutine that takes a Number argument, decrements it,
and calls itself while the argument is above zero, with no `Wait`. Bisect the starting
value against the deploy-time type check and the runtime error. The Portal site type
checks uploads, so an illegal shape would be refused at deploy rather than at run, which
distinguishes "not allowed" from "allowed but capped".

---

## 13. Open experiments

Two questions this design would like answered and does not need answered.

**Can `SetVariableAtIndex` grow an array past its current length?** `index.d.ts:345` says
only "Sets the value at the specified index". `convert.js:3580` assumes not, and pre-sizes
its packs. If it can grow, Rep B drops its pre-size entirely and array-literal
construction becomes one statement per element at depth 1 instead of depth 3. Experiment:
one rule that does `SetVariable(v, EmptyArray())`, `SetVariableAtIndex(v, 5, 42)`, then
displays `CountOf(v)` and `ValueInArray(v, 5)`.

**Is there a value expression that yields a genuine `undefined`?** `mod.IsUndefined` is
documented as testing for the result of a function "that cannot return a valid value"
(`index.d.ts:2416`), which implies `GetPlayer(-1)` or similar returns undefined. If it
does, Rep A's Map delete drops from two slots to one:
`SetVariable(ObjectVariable(K, s), GetPlayer(-1))` erases, and `IsUndefined` tests. That
halves the object-scope cost of every Map in the corpus, which is the tightest budget in
this whole design. Experiment: `SetVariable(v, GetPlayer(-1))` then display `IsUndefined(v)`.

Both are cheap and both should be run before the collection work starts, because the
second one changes the slot arithmetic in section 3.4.

---

## 14. Ranked order of work

Ratio is sites closed divided by implementation risk on a 1 to 5 scale, where 1 is a
mechanical rewrite with a clear spec and 5 is a design that can be wrong in ways that
compile.

| # | family | sites | risk | ratio | note |
| --- | --- | --- | --- | --- | --- |
| 1 | **Convert the bundle, not the tree** (section 1) | 644 | 1 | 644 | run `bf6-portal-bundler`, keep the SOURCE markers for reporting. Closes the whole cross-module family and most of section 8 as a side effect |
| 2 | **Reclassify catch-with-body** (section 11) | 196 | 1 | 196 | move to warnings; already converted correctly |
| 3 | **null, `??`, prefix unary** (section 2) | 809 | 2 | 405 | strip type-position nulls first, that is 608 of them; then `IsValid` / presence pair / `IfThenElse` |
| 4 | **Array-literal depth spill** (section 3.7) | small but blocking | 1 | high | one project already refuses at 79 deep, `BF6-Undead-Ground-Zero/src/config.ts:401`. The fix is already written for packs at `convert.js:3583` |
| 5 | **Date.now** (section 9) | 41 | 1 | 41 | one line, exact for every corpus use |
| 6 | **Template expressions** (section 6) | 107 | 2 | 54 | strings.json entry plus `Message`; 20 sites over three substitutions are impossible |
| 7 | **Options-bag inlining** (section 8) | 62 plus the `??` it removes | 2 | 31 | `GameUI.*` specialisation, exact because every call site passes a literal |
| 8 | **Timers** (section 5) | 151 | 3 | 50 | transcribe the helper; the `While` form for intervals is mandatory, not a choice |
| 9 | **Collections, Rep A only** (section 3) | roughly 1,100 of 1,553 | 3 | 367 | object-keyed sets and scalar maps, which is 83% of keys. Ship this before Rep B or C |
| 10 | **Closures with statement bodies** (section 7) | 48 | 3 | 16 | 89 of the 119 array lambdas reduce to an expression for free; only about 30 need the loop |
| 11 | **Spread** (section 10) | 68 | 3 | 23 | array cases only |
| 12 | **Collections, Rep B and C** (section 3) | the remaining 450 | 4 | 112 | needed for string keys and dense non-object ids |
| 13 | **Classes** (section 4) | roughly 50 non-UI `new` sites | 5 | 10 | struct of arrays. Do the singletons and the all-static classes first, which are free; leave multi-instance until the budget analysis says a given project fits |

### What is not worth doing

- **Catch-with-body recovery.** Section 11. There is no failure to recover from.
- **`new Promise`, `new Proxy`, variadic forwarding, object spread, function values.**
  Named as impossible in their sections. Each has under ten corpus sites and no honest
  encoding.
- **`String.prototype` methods.** `toFixed` and friends. The `text_*` entries in
  `catalog.json` are stock Blockly, carry no Portal type signature, and are used nowhere
  in the corpus. Run the deploy experiment in section 6 before spending anything here.
- **`Map<number, Map<number, number>>`.** One site,
  `bf6-Deadlock/src/index.ts:2345`. Two levels of association, neither of them a Player.
  Tell the author to key it by player and be done.
- **Multi-instance classes in a project that is already near the variable ceiling.** The
  converter should compute the budget and refuse up front rather than emit 23 array
  variables and let the upload fail. `BF6-Undead-Ground-Zero` is that project: 200
  collection declarations before a single class field is allocated.

### The single largest lever

Steps 1, 2 and 3 together close roughly 1,650 of the 9,297 sites, are all low risk, and
none of them requires a decision that could be wrong. Step 9 closes another 1,100 and is
the first place real judgement is needed. Everything after that is a long tail.
