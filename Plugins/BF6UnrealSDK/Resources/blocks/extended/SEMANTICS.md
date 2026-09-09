# Extended language contract — 0.1.0

The saved Blockly JSON is the authoring model. This compiler implements a deliberately bounded visual language. It does not claim that a finite set of blocks can express or reverse-engineer every TypeScript construct.

## Document and ordering

```json
{
  "bf6x": { "version": 1, "strings": { "welcome": "Welcome" } },
  "mod": { "blocks": { "languageVersion": 0, "blocks": [] } }
}
```

Every block needs a unique string ID. Root blocks are extended events, functions, and shared-state declarations. An enabled native root cannot be mixed with these. At least one enabled event is required. Disabled statement bodies are skipped; execution continues at the next enabled statement. A disabled required value produces an error. Disabled blocks remain in the saved project.

Shared initializers execute in serialized root order and may refer only to earlier shared declarations. Calls to async user functions are forbidden in these initializers. Root position can influence Blockly's serialization order: arrange dependency declarations before dependents. A future host should expose explicit declaration/event ordering if it supports independent canvas rearrangement.

Multiple blocks for the same event execute sequentially, including waits, in serialized root order. A forever loop in the first handler prevents later handlers for that same invocation from running. This is an explicit language choice; it is not an assertion about native Portal rule scheduling. Distinct event invocations retain separate locals.

## Storage and functions

- Shared declarations become module-level `let`; locals become lexical `let` scoped to their enclosing body. The generated names include stable per-compilation prefixes to avoid identifier collisions.
- Shared state is intentionally shared by concurrent events. This compiler does not make shared updates atomic across waits. It supplies no persistence, automatic player cleanup, networking layer, locks, or transactions.
- All custom functions return `Promise<T>`. All visual calls await them. Parameter names are local bindings; event parameters must be passed explicitly to functions. Values are passed with JavaScript's ordinary value/reference behavior.
- Early returns are supported in functions. `void` functions may fall through; this version's Return block takes a value. Arbitrary callback values, closures, classes, exceptions, background tasks, promises as values, decorators, dynamic imports, and raw source blocks are not built in.
- Accepted type notation: `number`, `boolean`, `string`, `unknown`, function return `void`, supported Portal types such as `Player`, and nested `List<T>`, `Map<K,V>`, `Record<string,V>` / `Record<number,V>`. `List<T>` emits `Array<T>`. There is no `any` type escape hatch in the built-in type parser. The real TypeScript checker remains authoritative where local inference is incomplete.

## Expressions, control flow and collections

- Function arguments, binary operands, record fields and list elements evaluate left to right. Conditional branches and `&&`, `||`, `??` preserve JavaScript's lazy evaluation. An extension generator must preserve those rules too.
- Counting loops evaluate `FROM`, `TO`, `STEP` once, in that order. They have an exclusive end and support positive/negative steps. Bounds must be finite; step cannot be zero. They do not implicitly yield. Extremely small floating-point steps can fail to advance a large counter; no universal termination analysis is provided.
- While loops re-evaluate their condition each iteration. Each loops use JavaScript `for...of` over the selected iterable; mutation follows JavaScript semantics. Add Wait explicitly where recurring gameplay work must yield. Invalid wait durations reject the handler at runtime.
- List values are actual JavaScript arrays, including nested arrays. List lookup uses `.at(index)`: negative indexes count from the end and missing indexes return `undefined`. Use the `??` binary operator with a fallback when a definite value is required. TypeScript rejects passing a possibly absent list result into an API requiring a number.
- Map lookup uses `.get(key) ?? fallback`, so fallback runs lazily. Zero and false are preserved. Nullish values use the fallback. Object keys use object identity; use an explicit numeric ID when that is the intended lifetime.
- Records emit computed property keys. The TypeScript build validates property access/mutation. Strings blocks refer to `mod.stringkeys`; string text accompanies the source and bundle.
- Engine APIs keep their engine behavior. The compiler does not turn every `mod.*` call into an awaited operation; the Wait block and custom function calls are the explicitly awaited operations in this release. Add a dedicated generator when an engine API requires specialized async/control/callback semantics.

## Targets and unsupported input

`extended-typescript` is the new backend. `portal-compatibility` delegates native workspaces to the supplied existing `BF6Convert.blocksToTs`. Compatibility compilation does not repair legacy import loss, allocation, aliasing or native scheduling problems. It has no extended source-map or concurrency guarantee.

Native Portal can only consume its own block types. `canExportNative` / `requireNative` reject any document containing `bf6x_` blocks, including disabled blocks, because those records are still foreign to the native serializer. The new path must send the **bundled script and strings through Script**, not send custom Blockly JSON to the site's native editor.

An arbitrary TS project cannot be imported by this new compiler. Keep original source until a separately tested importer supports its constructs. For a generated project, reopen `blocks/workspace.json`; editing `src/index.ts` and importing it back is not supported here. The original UGZ audit belongs to the separate audit folder and is summarized in the handoff.

Traversal is limited to 100,000 block records and 1,000 nested/next traversal depth. Very large projects also need the existing host's incremental loading/virtualization work; this demo is not a 50,000-block performance qualification.

## Extensions

`compile(doc, {catalog, events, legacy, strings, extensions})` accepts trusted host-installed generators. Each entry has `kind: 'value' | 'statement'` and `emit(block, context)`. Value generators return `{code, type}`. Statement generators call `context.line(code)`; body generators may use `context.body(inputName)` for a nested scope. `context.value(name)` consumes/compiles a value input; `context.field(name, default)` reads a field; `context.typeOf(text)` parses a supported type.

Do not call `context.line` from a value generator: hoisting statements out of an expression can break lazy evaluation. If an input is used more than once in emitted code, capture it in a helper parameter or another correctly scoped expression so side effects occur only once. Unknown connected children remain errors even in extensions. Generators are application code and must be tested; the registry is not a sandbox for project-supplied JavaScript.

Register the matching Blockly definition separately, including serialization hooks for dynamic sockets. This API supports expressions and nested bodies; adding new declaration kinds, event registration strategies, module imports, or callback scopes requires an explicit compiler change.
