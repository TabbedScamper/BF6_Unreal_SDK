#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class SWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogBF6UiBuilder, Log, All);

// ============================================================================
// BF6 UI BUILDER: design and animate a Portal HUD, and then use it.
//
// A dock tab holding our own page on the engine's embedded browser. On it:
// a 1920 x 1080 safe-area canvas with the four aspect presets, a widget tree,
// drag and resize with snapping, the whole property surface the engine
// exposes, a keyframe timeline, and an export that comes out in every shape a
// Portal project actually uses.
//
// WHAT THE TOOL SIDE IS FOR. The page owns the design; this owns the disk and
// the two seams. It reads and writes design files, writes the exports, hands
// the block tree to BF6Blocks and the TypeScript to the script project, and
// carries the palette the page cannot fetch for itself on file://.
//
// NOTHING HERE TOUCHES PORTAL. No sign-in, no session, no save. A design
// leaves this feature as text the user pastes, or as a file on their disk.
//
// THE LAWS THE EXPORT KEEPS, because breaking any of them is what makes a
// Portal HUD leak or crash:
//   widgets are created once and updated with Set*;
//   show and hide go through SetUIWidgetVisible, never a rebuild;
//   deleting a container frees its children, so a teardown deletes only the
//   top-level containers, and never at match end.
// ============================================================================
namespace BF6UiBuilder
{
	// StartupModule / ShutdownModule. Registers the tab, the bridge object and
	// the BF6.UI.* console commands.
	void Register();
	void Unregister();

	// Show the builder tab, creating it if it is not up yet (BF6.UI.Open). The
	// dock tab is the second way in; the UI button on the build screen's
	// toolbar row is the first.
	void Open();

	// ---- BF6EditorOverlay ----
	// THE builder page, made on the first ask and kept for the life of the
	// editor session. The full-screen host and the dock tab both show this one
	// widget, so a design in progress is never reloaded and never lost by
	// moving it. Handing it out detaches it from the dock tab, because a Slate
	// widget has exactly one parent.
	TSharedRef<SWidget> Widget();

	// The host is done with it: back into the dock tab if that tab is open, and
	// otherwise parked, still loaded, until something asks again.
	void ReleaseWidget();
	// ---- end BF6EditorOverlay ----

	// Load a design file into the page. Takes our own design JSON, the
	// community builder's params array, or a file holding a modlib.ParseUI
	// tree; the page decides which by looking at it (BF6.UI.Load).
	bool LoadFile(const FString& Path);

	// Ask the page for one export and write it (BF6.UI.Export).
	// Format is one of: typescript, strings, anim, deluca, solid, blocks,
	// community, design.
	bool ExportTo(const FString& Format, const FString& Path);

	// ---- the authored design is project-owned -------------------------------
	//
	// A design used to leave this feature only as an export the person chose to
	// write. It therefore belonged to no project, was in none of the project's
	// snapshots, and did not survive a restart or a reload of the panel.
	//
	// The project contract already names where designs live: <project>/unreal/ui,
	// one json per design, listed in the manifest as kind "ui". A design saved
	// from here goes there whenever a save is open, which puts it inside every
	// snapshot, move and sync the project gets, and to the tool's own designs
	// folder when no save is open, so it is never nowhere.
	//
	// THE PAGE OWNS THE HALF THIS CANNOT DO. It keeps the design in its own
	// model and hands it out only through the export sheet, so there is no
	// automatic save: nothing is written between one deliberate save and the
	// next. For that, Resources/uibuilder/editor.js has to send
	//
	//     {op:"designState", name: <design name>, json: <the design json>}
	//
	// when the design settles. HandleMessage already accepts that message and
	// writes the recovery copy; until the page sends it, a recovery candidate
	// only ever appears for a design that was saved at least once.

	// Put the export sheet on the design format, ready for SAVE TO FILE to send
	// the design here to be written where the project contract says. It cannot
	// write the file without that press: the page hands the design out only
	// through the sheet, and only when the host names a path, and the host does
	// not know the design's name until the page tells it one.
	bool SaveDesignToProject();
	// The designs this project holds, plus any work a previous session left
	// behind and whether it is newer than the design saved beside it.
	FString Designs();
	// Load work left behind back into the page. Empty name takes the newest.
	// It is LOADED, never copied over the saved design: keeping it is then a
	// deliberate save, because overwriting on the person's behalf is the same
	// loss in the other direction.
	bool Recover(const FString& Name);

	// One line per fact, for the output log.
	FString Status();

	// The bridge's one entry point: a JSON message from the page.
	void HandleMessage(const FString& Json);

	// ---- seams -------------------------------------------------------------
	// INSERT INTO BLOCKS goes through BF6Blocks' own public message channel, so
	// nothing in the blocks feature has to change: the snippet is written under
	// Resources/blocks/snippets/uibuilder/ and then asked for by name.
	//
	// INSERT INTO SCRIPT writes the TypeScript, the strings and the animation
	// clips into the tool's Saved folder and opens the script editor on them.
	// When BF6Script grows a text seam of its own - a single
	//
	//     void InsertFile(const FString& RelPath, const FString& Text);
	//
	// would do it - this should call that instead, so the file lands straight
	// in the open project's src folder. Until then the files are written where
	// the log says and the user drops them in.
	//
	// HIDE GAME HUD ELEMENTS. The LAYER control's link sends
	// {op:"openSettings", page:"ui"}. When BF6PortalSettings.h exists, the
	// handler in the .cpp becomes one line:
	//
	//     BF6PortalSettings::OpenPage(TEXT("ui"));
	//
	// which should land on the site's Modifiers > UI page, where the mutators
	// that switch default HUD furniture off live (CompassAllowed_PerTeam,
	// MinimapAllowed_PerTeam, limited_hud). Until then the handler logs where
	// to go by hand, so the link is useful rather than dead.
	FString ExportDir();
}
