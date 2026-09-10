# 0.8.2 hotfix verification

Validated on Windows with Unreal Engine 5.8.

- Native editor target compiled successfully. Lint passed with two existing Enter-handler warnings.
- Real Chromium exercised a 5,088-block Night Ops workspace: host-driven opening, same-experience map changes, edits preserved between experiences, recovery with cleared browser storage, queued project switches, complete update snapshots, and rejection while a workspace is still loading. Loading and switching emitted no Portal deletion or replacement messages.
- The normal Import format detector accepts update recovery envelopes as workspaces.
- Inside Unreal's own browser, opening the saved experience with Blocks already open loaded 5,088 blocks. The native update checkpoint wrote matching project recovery and independent update files; reloading the panel restored all 5,088 blocks.
- Holding the recovery file open without delete sharing made the native checkpoint reject the restart. The previous recovery file's hash remained unchanged.
- The original imported Night Ops workspace's SHA-256 remained unchanged throughout testing.
- Existing round-trip checks passed with zero fixture differences; project-state tests preserved off-screen files and focused rules; eight native-import converter checks passed.
- Release ZIP checks cover CRC integrity, SHA-256/size agreement with the manifest, matching descriptors, required resources and portable artwork. User `Saved` data, intermediate build output and local investigation files are excluded.

The new automatic checkpoint runs only after 0.8.2 is installed. Users updating from earlier versions must save/export unsaved Blocks edits first. This is also stated in the release notes and recovery guide. Script and UI panels still require their usual saves.
