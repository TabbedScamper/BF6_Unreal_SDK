# The project contract

Owner: the host plugin. Everything else adapts to this.

The editors share the existing `BF6Project` structure. The host owns the output
target, file layout and bindings so every authoring view uses the same contract.

## 1. Output target and authoring view are different things

This is the distinction the whole contract rests on, and getting it wrong is what makes
these tools feel like three separate programs.

- **Output target** is what the project ships. Exactly one is active:
  - `blocks` - a native Portal workspace, uploaded as blocks, editable on the site.
  - `typescript` - a DeLuca template project, built to `dist/bundle.ts` and pushed
    through the existing Script route.
- **Authoring views** are how a person edits it: the UI Builder, Blockly, Monaco, and
  later a graph. Any view may be used with either target where it makes sense.

**Choosing the TypeScript target must not require writing TypeScript.** A beginner can
build a complete supported mode from the UI Builder and blocks alone and still ship
`typescript`. The target says where the project goes, never who is allowed to author it.

## 2. Why a target has to exist at all

Native Portal blocks have no locals. Two invocations of one handler share a slot, so a
value held across a `Wait` is overwritten by the next player through the same handler.
That is not a bug to be fixed in the converter; it is the shape of the native block model.
The extended language exists to emit ordinary TypeScript, where a local is a real local,
which is why it clears the cases native cannot. See `Resources/blocks/extended/SEMANTICS.md`
and the behaviour suite at `Resources/convert/tests/behavior.js`.

So a project has to say which world it is in, because the same blocks mean different things
in each, and a workspace mixing them cannot be compiled as either.

## 3. Roots must not be mixed

A compiled workspace is rejected when it holds both native-only and extended-only roots.
The check already exists: `BF6Extended.canExportNative(doc)`, wrapped as
`BF6ExtendedIntegration.requireNative(doc)`.

- Target `blocks`: every root must be native. Any `bf6x_` block is a hard error naming the
  blocks responsible.
- Target `typescript`: extended blocks, native blocks, UI designs and handwritten
  TypeScript modules all coexist. Native blocks are lowered through the existing
  converter; extended blocks through `BF6Extended.compile`.

Guard every outbound native route, not the button the user happened to press: Send to
site, push to page, packing, and any automatic sync. Already wired in `editor_ui.js` as
`nativeRouteAllowed`.

## 4. What lives where, and who owns it

Extending the existing project directory rather than inventing a parallel tree:

| Path | Owner | Regenerated? |
|---|---|---|
| `bf6project.json` | host | no - the project record, including `target` |
| `blocks/<name>.workspace.json` | Blockly | no - authored |
| `ui/<name>.design.json` | UI Builder | no - authored |
| `src/strings.json` | host, merged | rewritten from authored sources |
| `src/generated/**` | compilers | YES - never hand edit |
| `src/**` (everything else) | the user | no - handwritten, never rewritten |
| `dist/**` | bundler | YES |

**One owner per file.** Regenerating a UI layout must never rewrite a handwritten handler
or a block body. If a generated file has been edited by hand, say so and make the user
choose; do not silently overwrite and do not silently keep.

## 5. Identity

Everything that can be referred to gets an ID that never changes:

- **Widget**: design ID plus an immutable widget ID. Not the visible label, not the
  editable name. Renaming or moving a button keeps its behaviour.
- **Handler**: a stable ID plus an explicit owner, `blocks` or `typescript`. A TypeScript
  handler resolves through its module and export symbol.
- **Feature**: a versioned pack ID, so updating a recipe preserves the user's edits.

A rename updates the mapping through the language service or produces a broken-reference
diagnostic. It never guesses. Deleting a widget lists every binding it breaks, as
something the user can act on.

## 6. Per-player instances

The same shop shown to two players is two sets of widget handles. Cache by instance and
owner, clear on destruction and on the player leaving, and mark shared UI explicitly. One
permanent global name-to-handle cache is not sufficient and is the same mistake as a shared
global standing in for a local.

## 7. Readiness is evidence, not memory

A project is send-ready only when the current content compiles. Tie readiness to a hash of
the compiled source. A previously successful build must never make a changed or failing
project send-ready, and the presence of the word `subscribe` is not evidence of anything.

## 8. Sequence

1. Project record with `target`, and the mixed-root rejection. **Host.**
2. UI Builder to Blockly to Monaco bindings over that record.
3. Guided controls writing canonical block fields and string entries, never regenerating
   over a customised body.
4. Feature packs as previewed change sets.
5. A graph view last, over the same intermediate representation, once the bindings,
   generation and navigation are proven.
