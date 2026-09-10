# BF2042 Portal Extensions: useful ideas for BF6

Reviewed September 10, 2026, against upstream commit
`5d202297aef73ae66a131b5d570ee8c3d05be298`. This is a source review and comparison
with the local BF6 editor, not certification of the old extension on the BF6
website. No upstream implementation was copied into the SDK.

## The red-variable reference is confirmed

The old [Red Variables plugin](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/plugins/red-variables/index.js)
changes `variable-block-style` in Blockly's theme. It does not add variable types
or game functions. Our category palette now provides that choice and other hues,
with persistent preferences and matching help examples. Variable references are
their own family; GetVariable remains a Value and SetVariable remains an Action,
consistent with the current BF6 definitions.

## Recommended order

| Idea | Current BF6 editor | Recommendation |
| --- | --- | --- |
| Personal category colors | Implemented with this review | Use the hue wheel, shared help colors and reset controls. |
| Multi-select move/copy/delete | No equivalent general multi-selection controller found in the audited page | Highest-value next editing feature. Support Shift selection and a marquee, with one undo step per gesture. |
| Share selected blocks as PNG/SVG | No dedicated image-export action found | Add for tutorials, Discord help and documenting modes. Export only the selected content. |
| Reusable snippets | Bundled examples and snippet insertion already exist | Extend into a named creator library with descriptions, required variables, scope and dependencies. |
| Darker control outlines | Current palette preserves native control geometry | Add an optional contrast setting for nested loops and conditions. |
| Jump to subroutine | Already implemented, including following callers and navigating files | Improve discoverability rather than adding a competing shortcut. |
| Collapse/comments/input layout | Native Blockly controls and existing collapse actions cover much of this | Keep existing behavior; audit batch operations when multi-select arrives. |
| Distraction-free editing | Helper shelf can close; toolbar and toolbox remain | A focus mode is useful later, with a visible exit control and a remembered layout. |

The upstream [feature list](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/README.md)
and [plugin index](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/plugins/plugin-index.md)
are useful inventories. The recommendations and priority above are our assessment
of gaps in this SDK, not upstream promises of BF6 compatibility.

## Implementation details that matter

For multi-selection, maintain selected IDs independently from Blockly's single
selected block. Normalize connected selections to avoid moving a child twice
with its parent. Group edits into one undo transaction, remap IDs during paste,
resolve variable/subroutine dependencies, and retain file ownership when only
one region of a project is visible. Test 10k blocks, cross-file selections,
deletion/undo and Portal synchronization. Upstream's movement implementation
applies a drag delta to other selected blocks; that alone does not cover these
project requirements. See [workspace event handling](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/src/web/Extensions.ts#L2011).

For image export, use an isolated rendering workspace containing the selected
blocks and their intended children. Embed fonts/icons and offer personal or
Portal colors. Verify that an unrelated block's text is absent from the SVG
source. Upstream clones the whole canvas and changes the viewBox; its README
explicitly warns that unrelated blocks remain inside exported SVGs. This is
also incompatible with simply copying our currently pruned viewport DOM.
See [upstream image export](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/src/web/Extensions.ts#L785).

Double-click shortcuts need reconciliation: our subroutine double-click follows
the call or its callers. Replacing that with the old collapse shortcut would
remove an existing navigation feature. The old plugin also times clicks without
requiring the same block, so it should not be transplanted unchanged.
See [Double-click Tools](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/plugins/doubleclick-tools/index.js).

## Boundaries of reuse

The upstream package declares Blockly 9.2.1; our bundled editor uses 10.3.0.
Its startup hook watches for a specific console message, its focus controls
target `app-root`, and its browser layer uses extension APIs unavailable to our
embedded page. The concepts transfer; the host hooks require adaptation and
testing. See [package versions](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/package.json)
and [startup hook](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/src/browser/web/app.js).

Do not adopt its [Disable Read-only workaround](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/plugins/disable-readonly/index.js):
it clears the workspace and then invokes undo. Our project loading and connected
Portal synchronization require a non-destructive editable-copy flow instead.

The old plugin manager loads JavaScript from plugin manifests. A future BF6
extension API should have versioned lifecycle hooks, explicit capabilities,
cleanup on workspace replacement and reliable disable/uninstall behavior.
It should integrate with the existing SDK add-on model. See [plugin loading](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/src/web/Plugins.ts).

Upstream identifies its license as [GPL-3.0](https://github.com/LennardF1989/BF2042-Portal-Extensions/blob/5d202297aef73ae66a131b5d570ee8c3d05be298/LICENSE.md).
This review uses it as a feature reference. The palette implementation was
written for our editor and does not bundle its code or dependencies.
