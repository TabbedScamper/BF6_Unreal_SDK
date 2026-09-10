# Keeping blocks across updates

When updating **from 0.8.1 or earlier**, save/export any unsaved Blocks edits before accepting the update. Those installed versions cannot run the new automatic block checkpoint. Keep the original project folder and its `Saved` directory when installing a fresh project download.

The in-editor updater replaces the plugins. Your experience workspaces and recovery files live under `Saved/BF6UnrealSDK`, outside the plugin folders, and stay in place.

Starting with 0.8.2, the updater asks the Blocks panel for its complete project, including files and rules outside the current view. It writes a separate update backup, reads it back to verify its contents, and saves project recovery before allowing the restart. An unfinished workspace load, failed write, or missing response cancels the update. Script and UI panels still need their normal saves.

## If an experience opens with an empty Blocks panel

0.8.1 could clear an already-open Blocks panel when opening a map without loading the saved experience workspace afterward. That empty display does not establish that the saved blocks were deleted. 0.8.2 loads the experience workspace on map open and keeps current edits when switching maps within the same experience.

Do not overwrite your original exports while investigating. Reopen the saved experience after updating. Recovery with the same original-workspace revision is restored automatically. A newly imported revision takes precedence over recovery from an older import; the files below remain available for manual inspection and import.

## Local copies

Paths below are relative to your Unreal project:

- Original experience workspace: `Saved/BF6UnrealSDK/saves/experiences/<experience>/unreal/blockly/workspace.json`.
- Current experience recovery: the adjacent `recovery.json`. Recovery and update checkpoints do not overwrite `workspace.json`.
- Independent update snapshots: `Saved/BF6UnrealSDK/blocks/update-backups/*.json`. Each checkpoint gets a new filename.
- Unnamed local workspace recovery: `Saved/BF6UnrealSDK/blocks/local/<project-key>/recovery.json`.
- Older Portal-linked autosaves: `Saved/BF6UnrealSDK/portal/experiences/<experience-id>/blocks/`.

Use the Blocks panel's Import command to open a recovery JSON or update snapshot, then export a separate workspace copy. The recovery envelope is recognized automatically. `BF6.Blocks.Checkpoint` in the Unreal console exercises the same backup path without installing an update.

These copies protect local work during normal updates and reopening. Keep an independent copy of the project for disk failure, accidental folder deletion, or moving to another computer.
