#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class SWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogBF6Blocks, Log, All);

// ============================================================================
// BF6 Blocks: the Portal rules editor, inside the tool.
//
// A dock tab holding our own Blockly workspace on a local page, wired to the
// Portal site page in the browser panel so an edit made here lands there and an
// edit made there lands here, block by block, while the user works.
//
// What it never does: sign in, save on its own, or write anything to the site
// that the user did not do in the editor. SAVE TO PORTAL presses the site's own
// save control, and that is the only way work leaves the tool.
// ============================================================================
namespace BF6Blocks
{
	void ApplyLootBinding(const FString& Json);
	// StartupModule / ShutdownModule. Registers the tab, the console commands,
	// the bridge object on both pages and the site's injected script.
	void Register();
	void Unregister();

	// Show the editor tab (BF6.Blocks.Open). The dock tab is the second way in;
	// the BLOCKS button on the build screen's toolbar row is the first.
	void Open();
	// Continue only after the page's complete workspace has reached disk.
	void PrepareForUpdate(TFunction<void(bool)> Done);

	// ---- BF6EditorOverlay ----
	// THE editor page, made on the first ask and kept for the life of the
	// editor session. The full-screen host and the dock tab both show this one
	// widget, so a workspace is never reloaded and never lost by moving it.
	// Handing it out detaches it from the dock tab, because a Slate widget has
	// exactly one parent.
	TSharedRef<SWidget> Widget();

	// The host is done with it: back into the dock tab if that tab is open, and
	// otherwise parked, still loaded, until something asks again.
	void ReleaseWidget();
	// ---- end BF6EditorOverlay ----

	// One line per fact: definitions cached, site connected, sequence counters,
	// last error. Printed by BF6.Blocks.Status.
	FString Status();

	// Read a Portal file into the editor without a site. The format is worked
	// out from the CONTENT, never the extension, and may be any of:
	//   a workspace       {"mod":{"blocks":{...},"variables":[...]}}
	//   a full experience the file the site itself imports and exports
	//   a script export   the TypeScript the site writes from blocks
	// An empty path opens the file picker.
	bool LoadFile(const FString& Path);

	// Write what the editor holds to a file.
	//   Format "workspace"  (the default) the site's workspace format
	//   Format "experience" the site's full experience format
	// An empty path opens the save dialog.
	bool SaveFile(const FString& Path, const FString& Format = FString());

	// The bridge's one entry point: a JSON message from either page.
	void HandleMessage(const FString& Json);

	// ---- session, and not losing work to it --------------------------------
	// BF6PortalProfile does not expose session delegates yet, so these two are
	// the hook: one line each from wherever the profile decides the session
	// went away and came back.
	//
	//   BF6PortalProfile::OnSessionLost().AddStatic(&BF6Blocks::NoteSessionLost);
	//   BF6PortalProfile::OnSessionRestored().AddStatic(&BF6Blocks::NoteSessionRestored);
	//
	// Until then the panel's own navigation is watched: a page that turns into
	// a login page counts as lost, and the blocks page coming back counts as
	// restored. Losing the session never blocks editing; it only stops pushing,
	// and the editor's journal replays when the session returns.
	void NoteSessionLost();
	void NoteSessionRestored();
	bool IsSessionLost();

	// The profile module already forwards WebPlay response bodies from its own
	// capture script. Handing them here as well lets a rejected save be shown on
	// the blocks it is about:
	//
	//   BF6Blocks::NoteWebPlayResponse(Url, Body);
	//
	// Body may be the raw gRPC-web frame text or a base64 copy of it; the
	// trailer (grpc-status, grpc-message) is what is read out of it.
	void NoteWebPlayResponse(const FString& Url, const FString& Body);

	// ---- seams a later feature can use -------------------------------------
	// Selecting placed objects from a block, and reading what is selected. Both
	// go through BF6Api (BF6BuildMode.h) and BF6Ext::Selection, so nothing here
	// needs the locked files changed.
	int32 SelectActorsByObjId(const TArray<int32>& ObjIds);
	bool  SelectedObjId(const FString& Kind, int32& OutObjId, FString& OutName, bool bAssignIfMissing);
	bool  AssignObjIdToSelected(int32 ObjId, FString& OutName);
}
