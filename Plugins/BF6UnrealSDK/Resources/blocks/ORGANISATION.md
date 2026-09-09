# Organising a converted workspace

How a converted TypeScript project should be laid out, labelled and navigated so
that a person who cannot read TypeScript can understand it.

Research and design only. Nothing in this file has been implemented. Every claim
below is followed by the evidence for it, and every number was measured during
this study rather than estimated.

---

## 1. The problem, in measured numbers

`C:\PortalSDK_1.4.2.0\GodotProject\User_Created\projects\BF6-Undead-Ground-Zero\src`

| measure | value | how it was measured |
| --- | --- | --- |
| TypeScript lines | 19,691 | `wc -l` over all `.ts`, backups excluded |
| top level functions | 450 | `grep -c "^(export )?(async )?function "` |
| source files | 33 | 30 hand written plus `helpers/index.ts`, `types.ts`, `index.ts` |
| event entry points | 21 | `BF6ScriptMap.buildGraph`, all 21 in `index.ts` |
| call edges | 916 | sum of `node.calls` |
| functions nothing calls | 67 | `node.dead` |
| graph nodes total | 986 | 467 functions, 498 module values, 21 handlers |

Against the genuine Portal export
`C:\Users\mwalt\Downloads\night_ops_breakthrough_workspace.json`:

| measure | value |
| --- | --- |
| total blocks | 5,088 |
| top level blocks | 58 |
| rules | 44 (43 inside the mod block, 1 parked outside it) |
| subroutines | 34 |
| variables | 104 |
| distinct block types | 188 |

So the largest human authored Portal workspace we have is 5,088 blocks. The
converted Undead project is roughly six times that. The reading problem is not
"a workspace is big", it is "a workspace six times larger than anything a person
has ever hand-authored, with no author-placed structure at all".

---

## 2. Research question 1: what organising devices exist, and which does the
site actually use

### 2a. What Blockly 10.3.0 persists

The vendored bundle is `Resources\blocks\vendor\blockly_compressed.js`, version
string `"10.3.0"` (grep). Two facts settle the whole question.

**Only two serializers are registered.** Searching the bundle for
`register$$module$build$src$core$serialization$registry("` returns exactly two
hits: `"blocks"` and `"variables"`. There is no third. `workspaceComments`
appears **zero** times in the entire 903 KB bundle.

That matches the genuine export byte for byte: the top level of
`night_ops_breakthrough_workspace.json` is `{ "mod": { "blocks", "variables" } }`
and nothing else.

**The exact set of keys a block can carry** comes from
`saveAttributes$$module$build$src$core$serialization$blocks`, read out of the
bundle:

```
a.isCollapsed()   && (b.collapsed = true)
!a.isEnabled()    && (b.enabled   = false)
!a.isOwnDeletable() && (b.deletable = false)
!a.isOwnMovable()   && (b.movable   = false)
!a.isOwnEditable()  && (b.editable  = false)
inputsInline !== default && (b.inline = a.inputsInline)
a.data && (b.data = a.data)
```

plus `type`, `id`, `x`, `y` (top level only), `extraState`, `icons`, `fields`,
`inputs`, `next`. `loadAttributes` reads every one of those back, including
`void 0 !== b.data && (a.data = b.data)`.

Icons are filtered by `isSerializable`, which is
`a.saveState !== undefined && a.loadState !== undefined`. The comment icon
defines both (`saveState(){return this.text ? {text, pinned, height, width} : null}`).
`WarningIcon` defines neither. So the comment icon persists and the warning icon
does not.

`loadState` on the comment icon ends with
`this.bubbleVisiblity = a.pinned ?? false; setTimeout(() => this.setBubbleVisible(...))`.
So `pinned: true` means the bubble is **open on load**, not a dot you have to
click.

**No colour key exists.** A census of all 5,088 blocks in the genuine export found
these keys and no others: `type, id, x, y, deletable, inputs, extraState, fields,
next, inline, icons, collapsed`. Block colour is a property of the block
definition and the theme, never of the workspace document.

**Collapsed label is truncated at 30 characters.**
`COLLAPSE_CHARS$$module$build$src$core$internal_constants = 30`. A collapsed
block shows its concatenated field text cut to 30 characters.

**Collapsing hides but does not destroy.** `updateCollapsed_` calls
`input.setVisible(!collapsed)` on every input. The child blocks still exist and
were still constructed. Collapsing buys layout and paint, not load time.

### 2b. What the Portal site's own authors actually use

Census across four genuine exports (`night_ops_breakthrough_workspace.json`, the
same file inside `night_ops_breakthrough_copy_experience.json`,
`battleship_experience V1.3.json`, `tm4_the_studio_pitfall_experience V1.1.json`):

| device | used in the night ops export | notes |
| --- | --- | --- |
| `x` / `y` on top level blocks | 58 of 58 | the primary device |
| free floating `Text` blocks as captions | 22 | the biggest surprise, see below |
| `icons.comment` | 3 | all `pinned: false` |
| `collapsed: true` | 1 | on the `Version` subroutine |
| `inline` | 15 | cosmetic, on `And` / `Or` / `SortedArray` |
| `deletable: false` | 1 | on the mod block, engine imposed |
| `enabled: false` | 0 | never used in any of the four |
| `data` | 0 | never used in any of the four |
| workspace comments | not representable | serializer does not exist |

The two things a human really did:

**Free floating `Text` blocks used as sticky captions.** 22 of them, clustered at
x 886 to 1241, y 361 to 1692, next to the mod block at x 89, y 445. They read:

```
Created by andy6170 / ViperStudiosAndy
Click on ? for the tutorial
Enable/Disable custom bots
Edit the Starting Reinforcements
Set the time for each sector
Edit the Bonus Tickets for Each Sector Taken
Bots behave like backfill
DO NOT USE WITH STATIC AI
1 Easy | 2 Default | 3 Hard | 4 Impossible
Percentage chance for custom AI to use vehicles
...
```

These are annotations on the configuration `SetVariable` stack at the top of the
`Initialise` rule. The author had no label block, so they used a value block that
holds text and parked it beside the thing it describes. Three of them additionally
carry a `pinned: false` comment icon for a longer explanation, which renders as a
question mark you click, which is why two of the captions say "Click on ? for
more info".

**Columns clustered by theme.** Grouping the 34 subroutines by x rounded to 500:

```
x ~ -11000  (1)  PlayerUISetup
x ~  -6500  (1)  TeamBotNames
x ~  -3500  (3)  AppendToArray, UniquePlayerUI1, AddBotNames
x ~  -2500  (3)  FlagCalls, WeaponPackage, AirVehicles
x ~  -2000  (1)  SectorUI
x ~   7000 (12)  UI_Setup, UIFlashAnimation, EverySecond, SpawnAI, Add_AI_Team,
                 SectorToggle, ChangingSectorUI, VehicleManagement,
                 UpdatePlayerScoreboard, ObjectivePlayerData, RemoveUI,
                 CapturePointSetup
x ~   8500  (7)  OOB_Toggle, OOB_Logic, AI_Deploy, AI_Scouting2,
                 AI_VehicleDeploy, AI_VehicleReset, FX_Reset
x ~  12500  (2)  AI_ScoreUpdate, AI_ObjectiveSpawn
x ~  13500  (3)  UI_Update, ObjectiveUI_Setup, ObjectiveUI_Colour
x ~  19000  (1)  Version
```

The 8500 column is out of bounds, then AI, then effects, in that order down the
column. The 13500 column is all objective UI. This is a human grouping related
work into a column and ordering it top to bottom. It is the same shape
`arrangeByFamily` already produces, and it is what the converter should aim at.

**A parked block means disabled.** One `ruleBlock`, "Team Switch For Defence
Testing", sits at x 2470, y 41569, outside the mod block, roughly 40,000 pixels
below everything else. `enabled: false` exists in the serializer and was not
used. The author's idiom for "off" is "drag it a long way away".

### 2c. One device the site defines that nobody in the corpus used

`convert/catalog.json` and `vendor/types_fallback.js` both define a block type
`actionComment`:

```json
"actionComment": { "kind": "statement", "inputs": [], "fields": [{ "name": "TEXT" }] }
```

`site_style_default.json` gives it its own colour, `#141414`, and its own block
style, `comment-block-style`. `blocks/editor.js:212` already maps it.

So the site has a native, styled, always visible comment block that sits in an
action stack like any other statement. It appears in none of the four genuine
exports, which means its round trip through the site is **inferred from the
site's own block registry, not observed**. `convert.js` mentions it zero times in
either direction, so today an imported `actionComment` would not be understood by
`blocksToIr`.

---

## 3. Research question 2: the right unit of grouping

**Recommendation: one region per source file.**

The corpus decides this. `BF6-Undead-Ground-Zero\src` is organised folder per
concern and file per feature:

```
src/config.ts              2 fn   180 module values
src/game-state.ts         11 fn   190 module values
src/helpers.ts            59 fn
src/event-handlers.ts     19 fn
src/index.ts                     21 event entry points
src/systems/weapons.ts    59 fn
src/systems/barriers.ts   39 fn
src/systems/power-system.ts 34 fn
src/systems/perks.ts      32 fn
src/systems/spawner.ts    23 fn
src/systems/helicopter.ts 21 fn
src/systems/undead-ai.ts  19 fn
src/ui/class-select.ts    27 fn
src/ui/proximity-ui.ts    18 fn
src/ui/hud.ts             16 fn
... 33 files, 1 to 59 functions each
```

The file name **is** the feature name, and `index.ts` imports them in a deliberate
order that already reads as a table of contents: foundation, then UI, then
systems, then event handlers.

The three rejected alternatives, each with its own evidence:

**Per event: rejected.** All 21 entry points in this project live in one file,
`index.ts`, and they nearly all delegate straight into `event-handlers.ts`
(62 KB, 19 functions). Grouping by event puts 30,000 blocks into about six
buckets and throws away every feature boundary the author drew. The existing
`FAMILIES` table in `convert.js` (ui, ai, vehicles, objectives, round-flow,
players, other) is a seven way split by event key. That is the right grouping for
the **rule stack**, because a rule genuinely is an event, but it is the wrong
grouping for the 438 subroutines, which is where 95 percent of the blocks are.

**Per rule: rejected.** There are 18 rules and 438 subroutines. A per rule
grouping leaves 438 things with nowhere to go.

**Per feature invented by us: rejected.** We would have to guess which functions
belong together. The author already told us, by putting them in the same file.
Guessing here would violate "never invent a description of what code does".

**So: the rule stack keeps the family grouping it already has; the subroutine
field is grouped by source file.** Those are two different questions and the
converter should stop answering them with one lane layout. Today
`arrangeByFamily` puts all 438 subroutines into a single `subroutines` lane
(`editor_ui.js:3474`, `fam = 'subroutines'`), which is the specific thing that
makes the canvas an undifferentiated wall.

One caveat measured here: 30,000 blocks over 438 subroutines is roughly 68 blocks
per subroutine, so `systems/weapons.ts` alone (59 functions) is about 4,000
blocks. A region is still too big to read expanded. Regions solve "where am I",
not "how much is on screen". Section 4 solves the second one.

---

## 4. Research question 3: what is visible at first glance

**Everything is collapsed except the mod block and the first rule.**

Concretely, on import:

1. Every `subroutineBlock` gets `collapsed: true`. 438 one line blocks instead of
   about 29,000.
2. Every `ruleBlock` gets `collapsed: true` except the first. 18 one line rows in
   the mod stack, with the initialise rule open.
3. Each region gets a header made of the two devices a human actually used: a
   free floating `Text` block holding the region name, carrying a comment icon
   with `pinned: true` holding the region's description and counts.
4. Regions are laid out in columns, in `index.ts` import order, left to right.
   Header at the top of the column, its subroutines below it in source order.

The result the reader sees on opening: about 460 one line blocks in 30 named
columns, and one open rule.

Why the initialise rule is the one thing left open: it is what the human author
did. The night ops mod block sits at x 89 y 445 with its configuration
`SetVariable` stack expanded and 22 captions arranged around it. That is the
author's chosen front page, and it is the right one, because a mod's settings are
the part a non-programmer can act on without understanding anything else.

Proposed header text, using only strings that exist in the source (see section 5):

```
Text block field TEXT:   "BARRIERS"
comment icon, pinned:    "From systems/barriers.ts. 39 subroutines.
                          The author's note on this file: BARRIER CLASS.
                          Nothing in the game starts this directly; it is
                          reached from 4 other places.
                          2 subroutines here are never called."
```

Every sentence there is a fact from `map.js` or a string lifted verbatim from the
source. Nothing is generated prose about what the code does.

**Important honesty about performance.** Collapsing does not make a 30,000 block
import fast. `updateCollapsed_` hides inputs; the blocks were still constructed.
Collapse fixes scrolling, panning and paint. Load time needs the focus mode in
section 8. The existing `measure.js` frame harness (`Resources\blocks\measure.js`,
the "how long a frame takes while panning" section) is the right instrument to put
a number on this, and it has not been run against a 30,000 block workspace. Do
not quote a speed-up until it has.

---

## 5. Research question 4: naming

**Keep the TypeScript name in the field. Put the plain English in the comment.**

The `SUBROUTINE_NAME` field is not a label, it is an identifier: it is what
`subroutineInstanceBlock` matches on, what `blocksToIr` reads
(`convert.js` `subroutineToIr`), and what the exported TypeScript function is
called. Rewriting it to "Repairs a barrier" breaks the round trip and breaks every
call site. It also breaks a 30 character collapsed label faster than the
identifier does.

So the identifier stays, and a plain English line goes on the block's comment
icon, sourced only from text a human already wrote. Four sources exist, in
priority order:

**1. The author's own comment line above the function.** Measured on the corpus
project: **227 of 450 functions (50 percent)** have a real preceding comment,
13 of them JSDoc. Examples, verbatim:

```
config.ts:477            // Dynamic zombie cap based on player count
event-handlers.ts:1111   // Helicopter Gunner Seat - Purchase on entry, kick if not enough cash
event-handlers.ts:1219   // Continuous enforcement - only authorized gunner allowed
systems/helicopter.ts    // Handles helicopter despawn/respawn edge case where player gets ejected
helpers.ts:306           // Bounce animation for UI widgets
```

That is the single best naming source we have and it costs nothing to collect:
`tsToIr` already has the source text and the node position.

**2. The file's banner heading, for the region name.** Measured: **135 banner
sections across 33 files**, and 24 of 33 files open with one. Verbatim:

```
systems/barriers.ts       BARRIER CLASS
systems/power-system.ts   POWER SYSTEM - Power lever, lights, emergency lights, alarm, and puzzle
systems/undead-ai.ts      UNDEAD AI SYSTEM - PLAYER-CENTRIC TARGETING
systems/power-ups.ts      Handles power-up drops from killed zombies, pickup/activation, visual effects, and timed effects
ui/class-select.ts        CLASS SELECTION & DIFFICULTY VOTING UI
ui/hud.ts                 OBJECTIVE UI - Shows current objective below undead counter
systems/extraction.ts     EXTRACTION SYSTEM
```

Detection is a `// ======` line followed by a `// Something` line. Those are the
author's own feature names. Use them for the region header, fall back to the file
name with the extension stripped when there is none.

**3. `Resources\script\api.json` for the rule header.** 415 functions and
**74 events**, each with a real `doc` string, for example:

```json
{ "name": "OnAIMoveToFailed",
  "doc": "This will trigger when an AI Soldier stops trying to reach a destination." }
```

A rule block's comment can carry that sentence for its `EVENTTYPE`. It describes
the event, which is a fact about Portal, not a guess about this mod.

**4. `Resources\script\guide.js` for the commands a subroutine uses.** 186 `mod.*`
entries with a plain `say` sentence and a `tag`. `map.js` already collects
`node.mod` (up to 40 per node), and `describe()` already crosses the two.

**5. `map.js` `describe(node, g)` for everything else.** It already produces
plain English from real graph facts and nothing else: "Called by X, Y and Z",
"It changes `playerCash`", "No other function in the files you have open calls
`foo`, and no event points at it. As things stand it never runs."

**When none of those exist, say nothing.** A subroutine with no author comment
gets a comment icon carrying only its provenance and its call counts. An empty
line is honest; an invented one is not.

---

## 6. Research question 5: provenance

Three channels, in ascending order of certainty.

**Channel A, the `data` key.** Verified persisted by the 10.3.0 serializer
(`a.data && (b.data = a.data)` on save, `void 0 !== b.data && (a.data = b.data)`
on load). Verified **unused by the site** in all four genuine exports, so there is
no collision risk. Carry a compact record:

```json
{"f":"systems/barriers.ts","l":412}
```

The risk: whether the Portal site's own loader preserves an unknown `data` string
across a save is **unverified**. It cannot be verified from here. The site probe
in `site_sync.js` already reads `Blockly.VERSION` off the live page; a one line
extension of that probe (publish a test workspace with `data` set, pull it back,
check) would settle it. Until it is settled, `data` is a convenience for our
editor, not a guarantee.

**Channel B, the comment icon.** Proven persisted, and proven present in a real
export. The last line of every generated comment reads:

```
From systems/barriers.ts, line 412.
```

That is the fallback that cannot be lost. It costs about 30 characters per
annotated block.

**Channel C, our editor's side pane.** `centreOn` already exists
(`editor_ui.js:2635`). Add a "Where this came from" row to the selected block
panel that reads `block.data`, shows `systems/barriers.ts line 412`, and offers a
button that hands the pair to the script editor. The script editor already knows
how to reveal a line: `map.js` keeps `decorations` for exactly that reveal.

**What the converter has to stop throwing away.** The data already exists and is
discarded:

- `buildSubroutines` reads `fnBodies[n]` which is `{node, file, params}`. It uses
  `f.file` to build `newTsCtx(program, report, f.file, f.sf, params)` and then
  does not store it on the Subroutine.
- `buildRule` does the same with `glue.file` / `actFn.file`.
- `lineOf(ts, ctx.sf, node)` already exists and is used throughout
  `reportUnconvertible`.

So adding `file` and `line` to the `Rule` and `Subroutine` records in
`convert/model.md` is a two field change with the values already in hand. Every
statement level block can inherit its enclosing subroutine's file and only record
a line where it differs.

---

## 7. Research question 6: what did not convert

The machinery already exists and is nearly invisible. `convert.js:4329` produces

```js
{ k: 'comment', text: 'NOT CONVERTED (' + what + ' at ' + ctx.file + ':' + line + '): ' + txt }
```

and `attachComment` (`convert.js`, in `stmtsToBlocks`) writes it onto the
preceding block as

```js
block.icons.comment = { text: existing + text, pinned: false, height: 80, width: 320 }
```

`pinned: false` is the problem. It renders as a small question mark that the
reader has to know to click. In a 30,000 block workspace nobody will click it.

Four changes, cheapest first:

1. **`pinned: true` on any comment that contains a NOT CONVERTED line.** One word.
   The bubble is then open on load, on the canvas, next to the gap. Verified: the
   comment icon's `loadState` sets `bubbleVisiblity` from `pinned` and calls
   `setBubbleVisible`.

2. **A local warning icon.** `block.setWarningText(...)` exists in 10.3.0 and
   draws the standard triangle. `WarningIcon` has no `saveState`, so it is
   verified local only and can never pollute an export. Use it for the visual
   marker; use the comment for the words.

3. **Bubble the count up to the region header and the enclosing subroutine.** A
   collapsed subroutine hides its inputs, so a loss inside a collapsed region is
   invisible. The subroutine's own comment must carry
   "3 things inside this the blocks cannot say", and the region header must carry
   the region total. Otherwise collapse-by-default trades one silence for another.

4. **A fifth tab in the rules pane.** `editor.html:512` already has
   `Rules / Subs / Vars / Loose`. Add `Gaps`, built directly from
   `report.unconvertible`, which is already
   `{file, line, construct, suggestion}` per entry and is already rendered by
   `showConvertReport`. Each row clicks through to the block via `centreOn`.

An `actionComment` block would be strictly better than a comment icon for this,
because it is always visible with no bubble, it has its own dark colour
(`#141414`), and it sits in the stack exactly where the missing statement was. It
is the correct target once its site round trip is verified. Until then the comment
icon is the safe choice, because it is observed in a genuine export.

---

## 8. Research question 7: navigating 30,000 blocks

Everything below is possible with the vendored Blockly 10.3.0 and no new
dependency. Nothing needs a CDN.

**What already exists and should be built on, not rebuilt:**

| thing | where | what it does today |
| --- | --- | --- |
| canvas search | `editor_ui.js` `stepSearch` | text search over blocks, fields and variables, Prev/Next, Ctrl+F |
| minimap | `editor_ui.js:3104` `drawMinimap` | canvas overview coloured by block type, click to jump |
| jump to block | `editor_ui.js:2635` `centreOn` | centre, select, highlight |
| outline | `editor.html:510` `pane-rules` | flat lists of Rules / Subs / Vars / Loose |
| focus mode | `editor_ui.js:2910` `enterRuleMode` | stashes the workspace, clears it, loads ONE block, writes it back on exit |
| family layout | `editor_ui.js:3459` `arrangeByFamily` | one column per rule family, plus one lane for all subroutines |
| collapse all | `editor.html` `btn-collapse` | `setCollapsed(true)` on every top block |

`enterRuleMode` is the most valuable thing in that list and it is under-used. It
already solves the load problem: it puts one unit on the canvas instead of the
whole project, and it already handles stash, anchor, replace and diff on the way
out. Generalising it from "one rule" to "one region or one subroutine" is the
single largest navigation win available, and most of the code is written.

**What to add:**

1. **Group the outline by region.** `buildNav` currently pushes every
   `subroutineBlock` into one flat `subs` array. With `file` on the subroutine
   (section 6), the Subs tab becomes 30 collapsible groups of 1 to 59 rows
   instead of one list of 438.

2. **Follow the call.** A `subroutineInstanceBlock` carries
   `fields.SUBROUTINE_NAME`. There are 175 of them in the night ops export. Match
   that against the `subroutineBlock` with the same name and call the existing
   `centreOn`. Ten lines. Add the reverse ("used by", 4 lines against the `used`
   map `buildNav` already computes) and a back stack so the reader can return.

3. **Breadcrumb.** The back stack from 2, rendered as a strip. The reader's
   position is "region, subroutine, block", all three of which are known.

4. **Colour the minimap by region, not by block type.** Today it is three colours:
   mod, subroutine, everything else. With 30 regions it should be 30 bands, with
   the region names drawn at the head of each column. That converts the minimap
   from a shape into a map.

5. **Search that scopes to a region,** and that reports "14 hits, 9 in WEAPONS".
   The hit list already exists in `UI.searchHits`.

**What is not feasible and why:**

- Blockly 10.3.0 has no virtualised rendering. There is no way to have 30,000
  blocks in a workspace and render only the visible ones. The only lever is not
  putting them there, which is what focus mode does.
- The toolbox and its categories are editor configuration, not workspace content.
  They never appear in the exported JSON. They also belong to the site's palette
  and repurposing them for the user's own rules would break the editor's promise
  to match the site.

---

## 9. Persisted into a real Portal workspace, versus our editor only

This is the list that matters most, because anything in the right column is a
thing the person loses the moment they take the file to the site.

### Persisted in the workspace JSON, survives an import into Portal

| device | key | evidence |
| --- | --- | --- |
| block position | `x`, `y` on top level blocks | 58 of 58 top blocks in the genuine export |
| collapsed | `collapsed: true` | serializer verified; used once in the genuine export |
| block comment | `icons.comment` = `{text, pinned, height, width}` | serializer verified; used 3 times in the genuine export |
| comment bubble open on load | `pinned: true` inside that | `loadState` verified in the bundle |
| free floating caption blocks | a top level `Text` block with a `TEXT` field | used 22 times in the genuine export |
| disabled block | `enabled: false` | serializer verified; never used by the site in 4 exports |
| inline inputs | `inline` | 15 in the genuine export |
| mutator state | `extraState` | 1,106 in the genuine export, carries subroutine params |
| variable names, ids and scopes | the `variables` array | 104 in the genuine export |
| arbitrary payload | `data` (a string) | serializer verified both directions; **never used by the site, and its survival across a site save is unverified** |
| a site-native inline comment | a block of type `actionComment` with a `TEXT` field | defined in `types_fallback.js`, coloured in `site_style_default.json`; **never observed in an export, round trip unverified** |

### Our editor only, lost on the way to the site

| device | why it is lost |
| --- | --- |
| workspace comments | Blockly 10.3.0 registers only the `blocks` and `variables` serializers. `workspaceComments` appears zero times in the bundle. Not representable at all. |
| warning icons | `WarningIcon` has no `saveState` / `loadState`, so `isSerializable` is false and `saveIcons` skips it. |
| block colour overrides | there is no colour key in `saveAttributes`; colour comes from the definition and the theme. |
| toolbox categories and flyout contents | editor configuration, never part of the document. |
| the minimap, the outline pane, search, breadcrumbs, focus mode | our chrome, not the document. |
| region groupings held in a side table | unless they are re-derivable from `x`/`y` clusters or from `data`, they do not travel. |

The practical consequence: **the workspace file itself can carry a real
organisation** through position, collapse, captions and comments. Everything
richer than that is a reading aid our editor provides and the site will not.
Design accordingly: put the structure in the four persisted devices, and use the
editor chrome to make that structure easier to walk, never to hold information
that exists nowhere else.

---

## 10. Phased implementation order

Ordered by value per unit of work. Each phase is independently shippable and each
leaves the workspace a valid Portal import.

### Phase 1, the collapse and the columns (cheapest, largest single effect)

- `convert.js` `irToBlocks`: set `collapsed: true` on every `subroutineBlock`, and
  on every `ruleBlock` except index 0.
- `convert.js` `buildSubroutines` / `buildRule`: keep `file` and `line`. The values
  are already in `fnBodies[n].file` and reachable through the existing
  `lineOf(ts, sf, node)`.
- `irToBlocks` layout: one column per `file`, in `index.ts` import order where an
  index file exists and alphabetical otherwise, replacing the current single
  1400 px column-wrapping loop.
- `editor_ui.js` `arrangeByFamily`: lane subroutines by `file` rather than into
  one `subroutines` lane.

Effect: about 460 visible one line blocks in 30 named columns instead of 30,000.
No new UI, no new data source, nothing invented.

### Phase 2, the headers and the plain English

- `tsToIr`: capture the preceding comment line above each function (measured at
  50 percent coverage) and the file banner heading (measured at 24 of 33 files).
- `irToBlocks`: emit one free floating `Text` block per region carrying the region
  name, with a `pinned: true` comment icon carrying the description, the counts
  and the file name.
- Every subroutine block gets a comment icon carrying, in order: the author's own
  comment line if there is one, the call count, and `From <file>, line <n>.`

Effect: the 30 columns get names a non-programmer can read, and every one line
block will answer "what is this" on hover.

### Phase 3, the gaps made loud

- `pinned: true` on any comment containing a NOT CONVERTED line.
- `setWarningText` on the same blocks, local only.
- Roll the counts up to the enclosing subroutine and the region header.
- A `Gaps` tab in `pane-rules` fed from `report.unconvertible`.

Effect: collapse-by-default stops being able to hide a conversion failure.

### Phase 4, navigation

- Group the Subs tab by region.
- Follow the call, both directions, with a back stack and a breadcrumb strip.
- Minimap coloured and labelled by region.
- Search scoped to a region, with a per region hit count.

### Phase 5, focus mode generalised

- Extend `enterRuleMode` / `exitRuleMode` to take a region or a single
  subroutine. The stash, anchor, replace and diff-on-exit path is already written.
- Load the full workspace only for export and for the overview; read one region at
  a time.

Effect: the load-time problem that collapse does not solve.

### Phase 6, verification, not a feature

- Extend the `site_sync.js` probe to settle whether the site preserves `data` and
  whether `actionComment` round trips. Both are currently unverified and both
  unlock a better mechanism than the one Phase 1 to 3 uses.
- Run `measure.js`'s frame harness against a 30,000 block workspace, collapsed and
  expanded, and record the numbers. No performance claim should be made before
  this exists.

---

## 11. Reuse rather than rebuild: `Resources\script\map.js`

`map.js` already builds the graph the block side needs, and it needs no DOM to do
it. Verified empirically: loading `map.js` in Node with `document` undefined
succeeds and exposes `buildGraph` and `describe`.

Node shape, from the source:

```js
{ id, rel, name, kind, exported, event, wiring, line, start, end,
  calls, calledBy, reads, writes, readBy, writtenBy,
  mod, subscribes, layer, dead, from }
```

`rel` is the region. `line` is the provenance. `calls` / `calledBy` is
follow-the-call. `dead` is "nothing runs this". `layer` is distance from an entry
point. `describe(node, g)` already renders all of it in plain English.

`blocks\editor.html` loads `convert.js` but not `map.js` and not `guide.js`. Both
are `window.*` globals with no load time DOM access. Two script tags is the whole
integration. The join key is the function name, which is `Subroutine.fnName` on
the convert side and `node.name` on the map side.

**One correction the map.js author needs.** `MAX_NODES = 400`, with the comment
"far above any real Portal project". Measured against the largest real Portal
project we have, it is below it, and by enough to be misleading rather than
merely limiting:

| run | nodes | entry points | dead | call edges | time |
| --- | --- | --- | --- | --- | --- |
| `MAX_NODES = 400` (as shipped) | 400 | **0** | 29 | not comparable | 157 ms |
| `MAX_NODES = 5000` | 986 | **21** | 67 | 916 | 761 ms |

At 400 the cap is exhausted by the 370 module level values in `config.ts` and
`game-state.ts` before the scan ever reaches `src/systems`. Three of 33 files
appear. Every entry point is in `index.ts`, which is never reached, so the panel
reports **zero entry points** for a mod with 21 of them. That is worse than a
truncated graph, because it is a confident wrong answer. The cap needs to be a
per-kind budget, or values need to be counted separately, or it needs to be raised
past 1,000. Flagging only; `map.js` is another agent's file and has not been
touched.

---

## 12. Considered and rejected

**Workspace comments as the primary annotation device.** Rejected on hard
evidence: Blockly 10.3.0 registers exactly two JSON serializers, `blocks` and
`variables`. `workspaceComments` appears zero times in the 903 KB bundle. A
workspace comment would be invisible the moment the file reached the site. This
is the single most tempting wrong answer, because workspace comments are exactly
the right shape for the job in later Blockly versions.

**Re-colouring blocks by region.** Rejected twice over. Colour is not in
`saveAttributes`, so it would not survive the export. And it would break the
promise the legend makes: `LEGEND_FAMILIES` in `editor_ui.js` teaches "colour
tells you what a block is for", with real blocks drawn by the real renderer.
Overloading colour with a second meaning would make that legend a lie.

**Renaming subroutines to plain English.** Rejected. `SUBROUTINE_NAME` is the
identifier that `subroutineInstanceBlock` matches on and that the exported
TypeScript function is named after. Renaming breaks every call site and the round
trip. The plain English goes on the comment, where it costs nothing.

**Generating a description of what a function does.** Rejected on the standing
rule. Where the author wrote a comment we quote it; where they did not, the block
says only what the graph knows. There is no third option that is honest.

**Custom block types for region headers.** Rejected. A block type the site does
not define is a block the site cannot load. `missingActionBlockType_v1` exists in
`types_fallback.js`, which suggests the site does have a placeholder path for
unknown blocks, but arriving there deliberately would be a poor trade for a
caption we can make out of a `Text` block that a real author already used 22
times.

**`actionComment` as the Phase 1 device.** Deferred, not rejected. It is
better-shaped than a comment icon in every respect, but it appears in none of the
four genuine exports, `convert.js` handles it in neither direction, and its site
round trip is unverified. It is the Phase 6 unlock.

**`data` as the only provenance channel.** Rejected as sole channel, kept as the
convenient one. Verified in our serializer, never used by the site, survival
across a site save unverified. Provenance also goes in the comment text, which is
observed to survive.

**`enabled: false` to mark dead code.** Rejected as the primary device. The
serializer supports it, but zero of four genuine exports use it, so its behaviour
on the site is untested, and a Portal author's actual idiom for "off" is to park
the block far away (the night ops rule at y 41569). Marking 67 never-called
subroutines as disabled would also be a strong claim resting on a regex call
graph. Say it in words on the comment instead, which is what `map.js` `describe`
already does: "No other function in the files you have open calls X".

**Blockly's own `cleanUp`.** Already rejected in the codebase, correctly, and the
reason is recorded in `arrangeByFamily`'s comment: it puts everything in one
column and knows nothing about the grouping.

**Loading all 30,000 blocks and relying on collapse for speed.** Rejected on the
bundle: `updateCollapsed_` calls `setVisible(false)` on inputs. The blocks are
still constructed. Collapse fixes paint and layout, not load. Focus mode fixes
load.

**Splitting the converted project into several workspace files.** Rejected. Portal
takes one workspace per experience. A split would not import.

---

## 13. Summary of the recommended design

The workspace itself carries its organisation in four devices that a genuine
Portal author already used and that the serializer is verified to persist:
position, collapse, free floating `Text` captions, and comment icons.

- **Regions are source files.** One column each, named by the file's own banner
  heading, ordered the way `index.ts` imports them.
- **Everything is collapsed except the mod block and the first rule.** About 460
  readable one line blocks instead of 30,000.
- **Names stay as identifiers; plain English rides on the comment,** quoted from
  the author's own comments, the file banners, `api.json` event docs, `guide.js`
  command lines, and `map.js` graph facts. Nothing invented.
- **Provenance is on every block** as `file:line`, in `data` for our editor and in
  the comment text as the guarantee.
- **Conversion gaps are pinned open, warned locally, counted upward** into the
  collapsed parent and the region header, and listed in their own tab.
- **Navigation reuses what exists:** the outline pane grouped by region, the
  minimap coloured by region, follow-the-call on `subroutineInstanceBlock`, and
  `enterRuleMode` generalised to a region so the reader never loads more than one
  at a time.

The cheapest high value change is Phase 1, and it is four edits inside
`irToBlocks` and `arrangeByFamily`.
