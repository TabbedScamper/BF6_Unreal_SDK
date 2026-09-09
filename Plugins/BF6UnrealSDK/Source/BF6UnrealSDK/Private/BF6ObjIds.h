#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"   // FRow holds a TWeakObjectPtr by value
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

// Forward declarations at GLOBAL scope, above the namespace. Written inside it,
// "class AActor*" would declare BF6ObjIds::AActor - a phantom type that only
// resolves when a unity blob declared the real one first.
class SWidget;
class AActor;

// ============================================================================
// BF6 ObjId manager: the whole ObjId story in one panel.
//
// NOTICE: Feature set after ObjId Manager for BFPortal by hekaron, MIT.
//
// Scripts address gameplay objects by ObjId. A duplicate id, or an id outside
// the band a pasted script template expects, breaks a mode silently - the map
// loads, the rules just address the wrong object. This file owns the registry
// that reads those ids, the panel that edits them, and the export that turns a
// selection into the TypeScript (or Blocks) the creator pastes into their mod.
//
// It sits ON TOP of BF6Api (BF6BuildMode.h): GatherObjIds is still the one
// reader, SetActorProp the one writer, AutoAssignObjIds still the plain assign
// path, and SelectDuplicateObjIds still the selector. Nothing here replaces
// them, because the blocks editor and the game-mode wizard call them directly.
// ============================================================================
namespace BF6ObjIds
{
	// One gameplay object with an ObjId, plus everything the panel judges it by.
	struct FRow
	{
		TWeakObjectPtr<AActor> Actor;
		FString Name;              // the actor label the creator sees
		FString Type;              // the SDK type ("CapturePoint", "SFX_...", ...)
		FString Category;          // the panel's grouping, see CategoryFor
		int32   Id = -1;           // -1 is unassigned, exactly as the addon reads it
		int32   SameIdCount = 0;   // how many objects share this id (1 = unique)
		int32   BandBase = 0;      // expected community band for this category
		int32   BandSpan = 0;      // 0 = no band convention, any id is fine
		bool    bOutOfBand = false;
		int32   RefCount = -1;     // -1 = unknown (no script/blocks provider set)
	};

	// Every placed and base object that carries an ObjId, sorted by category
	// then id (unassigned last). Built from BF6Api::GatherObjIds.
	TArray<FRow> Registry();

	// The panel's grouping for an SDK type name.
	FString CategoryFor(const FString& Type);

	// The community band a category's ids are expected to sit in. Returns false
	// when the category has no convention. Overridable per project in
	// DefaultEngine.ini under [BF6UnrealSDK] as ObjIdBand_<Category>=<base>.
	bool BandFor(const FString& Category, int32& OutBase, int32& OutSpan);

	// One line of facts for the console and the panel's status row.
	FString StatusLine();

	// The panel. OnBack, when set, draws a BACK button that runs it (the ring
	// hands us its reopen). Dockable through OpenTab, inline everywhere else.
	TSharedRef<SWidget> MakePanel(TFunction<void()> OnBack = nullptr);

	// Show the panel as an ordinary editor tab: resizable, tear-off, and it can
	// be left open on a second monitor beside the viewport.
	void OpenTab();

	// The export window (TypeScript or a Blocks snippet). Reopening an open one
	// brings it forward with its checks, size and position intact.
	void OpenExportWindow();

	// The generated code for a set of ids, in the order given. bBlocks picks the
	// Blocks-snippet shape over the TypeScript array. Used by the window and by
	// BF6.ObjIds.Export.
	FString GenerateCode(const FString& VarName, const TArray<FRow>& Rows, bool bBlocks);

	// The lint's duplicate-id rule, moved here so there is ONE definition of
	// what a duplicate means. Severity 0 = same type (scripts cannot tell the
	// objects apart), 2 = advice for cross-type reuse, which the shipped maps do.
	void LintDuplicates(TFunction<void(uint8, AActor*, const FString&)> Add);

	// ---- referenced-by hook -------------------------------------------------
	// The panel shows how many times a script or the blocks workspace addresses
	// an id, which is what makes renumbering safe to judge. BF6Blocks does not
	// expose its workspace ids yet (BF6Blocks.h has no accessor), so until it
	// does, whoever gains one calls this once at startup and every row lights
	// up. With no provider set the column reads "unknown" rather than "0",
	// because zero references and no answer are not the same claim.
	void SetReferencedIdsProvider(TFunction<TArray<int32>()> Provider);
	bool HaveReferencedIds();
}
