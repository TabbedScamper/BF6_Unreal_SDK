# Personal block colors

Right-click a block and choose **Change [category] color...**. Pick a hue on
the wheel, or use the hue slider, then press **Apply**. The dialog names the
color family being changed: Rules, Conditions, Actions, Values, Variables,
Subroutines, Control, Comments or Mod. All blocks in that family change together.
Control's alternating shade follows the same hue.

The same picker is available under **More > Block colors...** and through the
**Change** buttons in **What things mean**. That help panel, its actual block
examples, the selected-block explanation and the toolbox borders follow your
palette. Toolbox topics such as Player and Arrays still group blocks by subject;
the color families describe what those blocks do.

Wheel movement updates a preview. The main workspace repaints once when you
apply, avoiding repeated theme updates while you drag through colors on a large
project. Keyboard arrows change hue by one degree, Shift+arrow by ten; Home and
End select the ends of the hue range. Escape cancels. Custom fill colors retain
at least 4.5:1 contrast against white block labels.

**Reset category** restores that family's Portal color. **Reset all** restores
the complete Portal palette. Both are previews until you press Apply. Cancel
leaves the current palette alone.

Colors are saved in the editor's existing preferences, separate from Blocks
workspaces and experience files. Reloading the panel or importing a new Portal
style retains them. Existing red-variable preferences migrate to a red hue.
An explicitly reset palette takes precedence over that old setting.

Colors do not change block types, socket compatibility or mode behavior. They
are not included in Portal uploads or TypeScript exports; Portal displays its
own palette. Shapes, names and socket symbols remain useful when multiple
families have the same color. Unsupported-block error styling stays distinct
from these configurable families.

Validation: actual right-click menu and wheel in Chromium; actual rendered fill,
help and toolbox colors; new blocks and style recapture; cancel/reset; invalid
settings; unchanged exported program. The existing 5,088 and 10,176-block
browser suites passed import, appearance, navigation, undo and export checks.
The Unreal preferences bridge wrote the chosen hue to the disposable project's
INI and restored it after reloading the panel. Native SDK build succeeded.
