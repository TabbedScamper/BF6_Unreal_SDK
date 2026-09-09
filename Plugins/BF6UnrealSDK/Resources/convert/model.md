# BF6Convert intermediate model

Both directions map through one intermediate model. Blocks and TypeScript are
each a lossless projection of it for the subset described here; everything
outside the subset is reported, never dropped in silence.

    blocks JSON  <->  Program  <->  TypeScript

`blocksToIr` and `tsToIr` build a `Program`. `blocksToTs` and `irToBlocks`
consume one. A round trip in either direction is a fixed point for the covered
subset (test cases b and d prove it).

---

## Program

```
Program {
    variables:   Variable[]
    rules:       Rule[]          // workspace order, the order they run in
    subroutines: Subroutine[]
    orphans:     RawBlock[]      // free floating canvas blocks, carried verbatim
    layout: { mod: { x, y, id } }
}
```

### Variable

```
Variable {
    id:    string      // Blockly variable id, null when read from TypeScript
    name:  string      // the name the author typed, e.g. "FX-Reseting"
    scope: 'Global' | 'Player' | 'Team'
    slot:  number      // index within its scope, assigned in declaration order
    ident: string      // TypeScript identifier: sanitize(name) + scope + 'Var'
}
```

`ident` follows the site's own convention so a script we produce and a script the
site produces address the same names: `GameModeStarted` in Global scope becomes
`GameModeStartedGlobalVar`.

The Portal API has one variable primitive per scope, so `variables.ts` declares:

- Global: `export const XGlobalVar = mod.GlobalVariable(slot);` (type `mod.Variable`)
- Player and Team: `export const XPlayerVar = slot;` (a plain slot index)

An object scoped variable is then addressed exactly as the site addresses it,
`mod.ObjectVariable(object, XPlayerVar)`. Confirmed against
`mod.index.d.ts`: `GlobalVariable(number): Variable`,
`ObjectVariable(object: Object, number: number): Variable`,
`GetVariable(variable: Variable): Any`, `SetVariable(variable: Variable, arg: Any): void`.
There is no per object map in the API and none is invented.

### Rule

```
Rule {
    name:       string        // the NAME field, free text, e.g. "AI Capture Point Logic"
    event:      string        // EVENTTYPE, e.g. "Ongoing" or "OnPlayerDeployed"
    objectType: string        // OBJECTTYPE, e.g. "Global", "Player", "Team", "HQ"
    eventKey:   string        // "Ongoing" + objectType, else event
    fnName:     string        // eventKey + '_' + sanitize(name), de-duplicated with a digit
    isOngoing:  boolean
    index:      number        // position in the workspace rule stack
    conditions: Expr[]        // combined with AND
    actions:    Stmt[]
    comment:    string | null
}
```

`eventKey` is the exported Portal handler the rule hangs off: `OngoingGlobal`,
`OngoingPlayer`, `OnPlayerDeployed`, and so on. The handler names and their typed
parameters come from `events.json`, generated from
`bf6-portal-utils/events/index.ts`, which implements every Portal event handler
once. Parameter naming matches the site: `event` + type name, with a second
parameter of the same type becoming `eventOther<Type>` (`OnPlayerDied` gives
`eventPlayer, eventOtherPlayer, eventDeathType, eventWeaponUnlock`).

### Subroutine

```
Subroutine {
    name:   string
    fnName: string             // sanitize(name)
    params: [{ name, type }]   // type is a Portal type name: Player, Number, Boolean, ...
    x, y:   number             // canvas position, preserved
    conditions: Expr[]         // a guard: when false the subroutine returns
    actions:    Stmt[]
    needsEventInfo: boolean    // reads an event value, see "Event context" below
}
```

---

## Statements

```
{ k:'call',     fn, args }                       mod.<fn>(args)
{ k:'sub',      name, args }                     a subroutine call
{ k:'setVar',   ref, value }                     mod.SetVariable(ref, value)
{ k:'setVarAt', ref, index, value }              mod.SetVariableAtIndex(...)
{ k:'wait',     seconds }                        await mod.Wait(seconds)
{ k:'if',       branches:[{cond, body}], elseBody }
{ k:'for',      ref, from, to, step, body }      ForVariable
{ k:'while',    cond, body }                     While
{ k:'control',  word:'break'|'continue' }        Break / Continue
{ k:'comment',  text }                           a note, no runtime effect
{ k:'raw',      text }                           verbatim TypeScript, blocks side reports it
```

`Break` and `Continue` blocks become native `break;` and `continue;`, matching the
site's export. `mod.Wait` and `mod.WaitUntil` are the suspending calls: a function
whose own body contains one is emitted `async` and the call is awaited.

## Expressions

```
{ k:'num'|'str'|'bool', v }
{ k:'enum',   enumName, member }                 mod.<Enum>.<Member>, from a <Enum>Item block
{ k:'call',   fn, args }                         mod.<fn>(args)
{ k:'getVar', ref }                              mod.GetVariable(ref)
{ k:'varRef', name, scope, slot, ident, object } an identifier, or mod.ObjectVariable(object, ident)
{ k:'event',  name }                             eventInfo.eventPlayer and friends
{ k:'element' }                                  the current array element
{ k:'arg',    index, name }                      a subroutine parameter
{ k:'subCall', name, args }                      a subroutine used as a value
{ k:'lambda', body }                             the per element expression of an array block
```

Blocks have no lambdas. `FilteredArray`, `SortedArray`, `MappedArray`,
`IsTrueForAll`, `IsTrueForAny` and `IndexOfFirstTrue` take a second input that is
evaluated once per element with `CurrentArrayElement` standing for the element.
In TypeScript that input becomes `(currentArrayElement: mod.Any) => <expr>` and
the call goes to a generated runtime helper rather than to `mod`, because the
native functions take the expression by value and the TypeScript form needs a
real closure. `runtime.ts` implements each helper over `mod.CountOf`,
`mod.ValueInArray`, `mod.AppendToArray` and `mod.EmptyArray`.

`ArrayContains`, `IndexOfArrayValue` and `RemoveFromArray` are block types with no
`mod.*` function. They are emitted as runtime helpers with the same names. The
site's exporter instead desugars them into `IsTrueForAny` / `IndexOfFirstTrue` /
`FilteredArray` with an `Equals` lambda; the semantic comparator normalises both
spellings to the same tree, and also treats `Not(Equals(a, b))` and
`NotEqualTo(a, b)` as one.

---

## Rule semantics

A Portal rule is edge triggered: the actions run on the tick the conditions
become true, not on every tick they stay true. Each rule becomes three functions,
the same shape the site's own exporter produces:

```ts
function <fn>_Condition(eventInfo: any): boolean { return <conditions ANDed>; }
function <fn>_Action(eventInfo: any): void { ... }

export function <fn>(conditionState: rt.ConditionState, eventInfo: any): void {
    const newState = <fn>_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    <fn>_Action(eventInfo);
}
```

`ConditionState.update` returns true only on a false to true transition, so an
Ongoing rule with no conditions runs exactly once, which is what an `Initialise`
rule is for. There is no polling loop: `OngoingGlobal`, `OngoingPlayer` and the
other `Ongoing<ObjectType>` functions are Portal event handlers the engine calls
per tick, and the state lives across ticks.

State lifetime differs by rule kind:

- Ongoing Global: `rt.getGlobalCondition(ruleIndex)`, one persistent state per rule.
- Ongoing on an object: `rt.getObjectCondition(object, ruleIndex)`, one persistent
  state per (object, rule), keyed by `mod.GetObjId`.
- Event rules: `rt.getEventCondition()`, a fresh state per event. An event rule's
  conditions are a plain guard evaluated once when the event fires, so it must be
  able to fire again on the next event. The site's exporter routes event rules
  through the same persistent helper as Ongoing rules; that is the one place this
  converter deliberately differs, and the call shape is unchanged.

Rule indices are unique across the whole program rather than restarting per
handler, so two rules can never share a condition state.

Conditions are combined with `mod.And`, folded into nested two argument calls
because `And(boolean, boolean)` takes exactly two operands. The site emits a
variadic `mod.And(a, b, c)`, which does not type check.

## Event context

Blocks let a subroutine read `EventPlayer` and friends: the subroutine inherits
the calling rule's event context implicitly. TypeScript cannot, so a subroutine
that reads an event value takes a trailing `eventInfo: any` parameter, and the
need propagates to every caller by fixed point. That trailing parameter is not
part of the block signature and is stripped on the way back. The site's exporter
emits bare `eventInfo` inside such subroutines, which does not compile.

---

## Output organisation

`blocksToTs` returns a file set, not one file:

| file | contents |
| --- | --- |
| `index.ts` | every exported Portal event handler, one per event key, each listing its rules in workspace order |
| `rules-round-flow.ts` | `OnGameMode*`, `OnTimeLimitReached`, `OngoingGlobal` |
| `rules-players.ts` | `OnPlayer*`, `OngoingPlayer`, `OngoingTeam`, `OnMandown`, `OnRevived`, gadget and raycast events |
| `rules-objectives.ts` | capture points, sectors, HQs, MCOMs, interact points, area triggers, ring of fire, waypoints |
| `rules-vehicles.ts` | anything naming a vehicle |
| `rules-ui.ts` | `OnPlayerUIButtonEvent` |
| `rules-ai.ts` | `OnAI*`, `OnAutoPlayer*`, spawners |
| `rules-other.ts` | everything else |
| `subroutines.ts` | every subroutine |
| `variables.ts` | every variable |
| `runtime.ts` | `ConditionState`, the condition state stores, the array helpers |
| `strings.json` | the interned string table, `s0..sN` in order of first appearance, the same scheme the site emits |

Group files carry a header comment listing the rules inside; each rule carries a
comment with its original block name. Only files whose group has rules are
written. `familyOf(eventKey)` is exported so an editor's outline can use the same
grouping.

## Metadata that keeps a round trip faithful

Generated TypeScript carries three machine readable comments so names, order and
canvas layout survive:

```
// portal:rule {"name":"AI Capture Point Logic","event":"OnPlayerEnterCapturePoint","objectType":"Global","index":11}
// portal:subroutine {"name":"SectorToggle","x":6980,"y":5148,"params":[{"name":"Enable","type":"Boolean"}]}
// portal:var {"name":"FX-Reseting","scope":"Global","slot":42}
```

Without them a rule name is reconstructed from the function name by replacing
underscores with spaces and stripping a de-duplication digit, which is what the
site's export forces. That heuristic recovers every rule name in the fixture but
cannot recover a name containing a character the identifier rules do not allow
(`FX-Reseting` comes back as `FX_Reseting`).

---

## What has no block equivalent

`tsToBlocks` reports each of these as `{file, line, construct, suggestion}` and,
where a statement cannot convert, attaches a `NOT CONVERTED (...)` comment to the
preceding block so the loss is visible in the editor rather than silent.

| TypeScript construct | why | what the report suggests |
| --- | --- | --- |
| `class`, `new X()`, methods | blocks have no object model | flatten into subroutines plus scoped variables |
| closures with a statement body | array blocks take one expression | reduce to one expression or move into a subroutine |
| module imports other than `./runtime.ts`, `./variables.ts`, `./subroutines.ts`, `./rules-*.ts`, `modlib` | blocks have no module system | inline the helper as a subroutine, or keep the file TypeScript only |
| generics | blocks have no type parameters | give the subroutine concrete parameter types |
| local `const` / `let` inside a function | blocks have no locals, only workspace variables | store it in a Global or object scoped variable |
| module level bindings not named `<Name><Scope>Var` | not a workspace variable | rename, or move inside a subroutine |
| top level statements other than event subscriptions | blocks have no module init phase | move into an Ongoing Global rule |
| assignment expressions | there is no assignment block, only `SetVariable` | use `mod.SetVariable` |
| template literals with substitutions | `Text` holds a plain string | use `mod.Message` with arguments |
| element access (`a[b]`) and arbitrary property access | values are reached through `mod.*` and variables | use `mod.ValueInArray` or the matching accessor |
| `interface`, `type`, `enum` declarations | type level only | dropped, runtime behaviour unaffected |
| `for` loop without `mod.SetVariable(<var>, <counter>)` as its first body statement | a `ForVariable` block stores its counter in a workspace variable | add that first statement |
| `mod.ObjectVariable(obj, 8)` with a raw slot number | the variable name is not in the source | use the `<Name>PlayerVar` constant |

Recognised and converted: exported event handlers, `Events.<Event>.subscribe(fn)`
subscriptions, `<Event>_<Name>_Condition` / `_Action` / glue triples, `mod.*`
calls, runtime and `modlib` helper calls, subroutine calls, number / string /
boolean literals, `mod.<Enum>.<Member>`, `eventInfo.event*` and bare `event*`
identifiers, `currentArrayElement`, subroutine parameters, `if` / `else if` /
`else`, `for` in the `ForVariable` shape, `while`, `break`, `continue`,
`await mod.Wait` and `await mod.WaitUntil`, `mod.GetVariable` /
`mod.SetVariable` / `mod.SetVariableAtIndex` / `mod.ObjectVariable`, and the
comparison and arithmetic operators, which map onto `Equals`, `NotEqualTo`,
`LessThan`, `LessThanEqualTo`, `GreaterThan`, `GreaterThanEqualTo`, `Add`,
`Subtract`, `Multiply`, `Divide`, `Modulo`, `And`, `Or` and `Not`.

## Layout on the way back

The mod block keeps its recorded position, or defaults to (100, 100). Rules chain
under its `RULES` input in file order, so Blockly lays the stack out itself.
Subroutines are free floating: a recorded `x, y` is kept, otherwise they are
placed in columns 1400 px to the right of the mod block, each advanced by an
estimated height of 30 px per block plus 120 px of padding, wrapping to a new
column once a column passes the height of the rule stack. Block ids are fresh and
deterministic (`c0001`, `c0002`, ...) so two runs over the same input produce
byte identical JSON.

## API

```js
BF6Convert.blocksToTs(workspaceJson, options) -> { files: { name: source }, report, program }
BF6Convert.tsToBlocks(sources, options)       -> { workspace, report, program }
BF6Convert.summarize(workspaceOrProgram, opts) -> a comparable semantic summary
BF6Convert.diffSummaries(a, b, labelA, labelB) -> string[]
BF6Convert.familyOf(eventKey) -> group id      BF6Convert.familyTitle(id) -> display name
BF6Convert.setCatalog(json)  BF6Convert.setEvents(json)  BF6Convert.setTypeScript(ts)
```

`sources` is either a single source string or a map of file name to source.
Node loads `catalog.json`, `events.json` and the TypeScript compiler
automatically; a browser host injects them with the three setters.
