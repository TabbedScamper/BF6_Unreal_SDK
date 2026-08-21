#pragma once

#include "CoreMinimal.h"

// ============================================================================
// Build Mode: the full-screen "building program" experience layered over
// Unreal's real level viewport. This header is the seam between the new Slate
// widgets (implemented in BF6BuildMode.cpp) and the engine/session logic
// (implemented in BF6UnrealSDK.cpp). The docked SBF6Browser tab stays as-is; the
// new system is parallel and self-contained.
// ============================================================================

namespace BF6Api
{
	// One placeable row, flattened so widgets don't touch engine types.
	struct FPlaceableInfo
	{
		FString Type;
		FString Directory;
		FString Mesh;
		FString Category;      // effective category (user override, else top directory)
		int32   PhysicsCost = 0;
	};

	// ---- session state (implemented in BF6UnrealSDK.cpp) ----
	bool     IsEditing();
	FString  CurrentLevel();
	FString  CurrentSave();

	// ---- budget (physics cost) ----
	void         RecomputeBudget();
	FText        BudgetText();
	float        BudgetFrac();       // 0..1 of the level cap (0 if uncapped)
	FLinearColor BudgetColor();
	bool         BudgetOver();

	// ---- placeable catalogue for the current level ----
	// Top-level directory segments present in this level (the radial categories).
	TArray<FString>        Categories();
	int32                  CategoryCount(const FString& Category);
	// Placeables in a category, optional fuzzy filter, capped for the popup list.
	TArray<FPlaceableInfo> PlaceablesIn(const FString& Category, const FString& Query, int32 Max = 400);

	// ---- the slide-up Object Library ----
	// Categories/objects for the library panel. bAllLevels=false browses only
	// what is placeable on the OPEN map; true is the Full Library (everything).
	// Empty Category = all categories.
	TArray<FString>        LibraryCategories(bool bAllLevels);
	TArray<FPlaceableInfo> LibraryPlaceables(const FString& Category, const FString& Query, bool bAllLevels, int32 Max = 800);
	// Persist a user "move to category" (empty NewCategory = back to default).
	// Effective everywhere: the pie, the popups, and the library.
	void    SetTypeCategory(const FString& Type, const FString& NewCategory);

	// ---- map list (for the selector) ----
	TArray<FString> AllLevels();
	FString         DisplayName(const FString& Level);   // "MP_Dumbo" -> "Manhattan Bridge"
	int32           PlaceableTotal(const FString& Level);
	TArray<FString> SavesFor(const FString& Level);
	bool DeleteSave(const FString& Level, const FString& Name);   // both layouts; refuses the open session
	// Thumbnail brush for a level card (may be null if the PNG is missing).
	const struct FSlateBrush* MapThumbnail(const FString& Level);

	// ---- actions ----
	// Load a map's world context (terrain/assets/base setup) + set session state.
	// SaveName empty = read-only base preview; non-empty = editable custom map.
	void    OpenMapWorld(const FString& Level, const FString& SaveName);
	// Place a placeable by type at a world position; tagged + budget-counted.
	AActor* PlaceType(const FString& Type, const FVector& WorldPos);
	// Place in front of the camera; re-placing before the first was touched
	// swaps it in place (library double-click / "Place in scene").
	AActor* QuickPlace(const FString& Type);
	// The card image for a model: an isometric 256px render, generated on demand
	// and cached under Saved/BF6UnrealSDK/thumbs. Null while it is still queued.
	const struct FSlateBrush* GetModelThumb(const FString& Mesh);
	void    ExportSpatial();
	// Opens a file dialog; detects the map from the file, loads it, and names the
	// session after the file. Returns true when a map was actually imported.
	bool    ImportSpatial();
	// Write the current custom map's session to disk. bSilent skips the toast
	// (used by the tool's own periodic autosave).
	void    SaveCurrent(bool bSilent = false);
	// Turn the read-only base preview into an editable custom map named Name.
	void    CreateCustom(const FString& Name);
	// Deproject the current viewport cursor to the ground plane. False if no viewport.
	bool    WorldFromViewportCursor(FVector& OutWorld);
	// The surface point straight ahead of the camera (library double-click placement).
	bool    WorldFromViewportCenter(FVector& OutWorld);
	// Godot-style camera navigation (the input processor's MMB handling).
	bool    ComputeOrbitPivot(FVector& Out);
	void    CameraOrbit(const FVector2D& DeltaPx, const FVector& Pivot);
	void    CameraPan(const FVector2D& DeltaPx, const FVector& DepthRef);

	// ============================================================================
	// Entry points implemented in BF6BuildMode.cpp
	// ============================================================================

	// The whole tool as one embedded, dockable editor panel (legacy fallback).
	TSharedRef<class SWidget> MakeToolPanel();
	// Attach the tool UI directly onto the level viewport (the editor's whole
	// centre, resizes with it): selector screen first, build HUD after a pick.
	// Retries on a ticker until the viewport exists.
	void ShowStartupUI();
	// Remove the viewport UI entirely (module shutdown).
	void DetachUI();
	// Load the chosen map + switch the viewport UI to the build screen.
	void EnterBuild(const FString& Level, const FString& SaveName);
	// Add / remove the viewport overlay (budget bar + import/export + radial host).
	void ShowBuildOverlay();
	void HideBuildOverlay();
	// Register the space-bar pie-menu input handler (once, at startup).
	void InstallInputHandler();
	void RemoveInputHandler();
	void HideTransientMenus();   // dismiss the category object popup
	bool IsBuildOverlayActive();

	// ---- object attributes (the context radial's edit mode) ----
	// One editable field the SDK exposes on a placeable type. Options carries a
	// "selection" enum's choices (the same dropdowns the Godot SDK shows).
	struct FPropDef { FString Name, Type, Default; TArray<FString> Options; };
	// The SDK-defined editable properties of a type (HQ teams, MCOM arming,
	// combat-area timers, spawner vehicle types, the full suite).
	TArray<FPropDef> PropsForType(const FString& Type);
	// The currently selected base/placed gameplay actor, if any (else null).
	class AActor* SelectedGameplayActor(FString& OutType);
	// Per-actor property values, stored as "p:Key=Value" actor tags. Seeded from
	// the base setup; falls back to the type's schema default when unset.
	FString GetActorProp(AActor* A, const FString& Key, const FString& Fallback = FString());
	void    SetActorProp(AActor* A, const FString& Key, const FString& Value);

	// ---- zone (polygon volume) point editing, Godot-style ----
	bool IsVolumeActor(AActor* A);          // does this actor carry an editable loop?
	bool IsVolumeEditing();
	void BeginVolumeEdit(AActor* Volume);   // spawn a drag handle at every vertex
	void VolumeAddPoint();                  // insert after the selected handle
	// insert a point on the loop edge nearest to WorldPos (Ctrl+LMB / right-click on an edge)
	void VolumeAddPointAt(const FVector& WorldPos);
	// closest point on the loop's edges to WorldPos (the Ctrl hover marker)
	bool VolumeNearestEdgePoint(const FVector& WorldPos, FVector& OutPoint);
	void VolumeDeletePoint();               // remove the selected handle (min 3)
	// remove the point nearest to WorldPos, Godot-style Ctrl+RMB (min 3)
	void VolumeDeletePointAt(const FVector& WorldPos);
	// ---- the zone's screen-space dots (Godot-style handles) ----
	// Viewport-pixel positions of the edited zone's handles - TWO per point,
	// [0..N) bottom ring then [N..2N) top ring - plus the Ctrl edge-preview
	// dot (computed in screen space so it hugs the cursor). False = not editing.
	bool  GetZoneDots(TArray<FVector2D>& OutPx, int32& OutPointCount, int32& OutActive, int32& OutDrag, FVector2D& OutEdgePx, bool& bOutEdge);
	int32 ZoneDotUnderMouse();            // grab test at the cursor, -1 = none
	bool  IsZoneDotDragging();
	void  BeginZoneDotDrag(int32 Index);  // opens the undo transaction
	void  DragZoneDotToCursor();          // slide on the handle's height plane
	void  EndZoneDotDrag();               // closes the transaction
	void  VolumeAddPointAtPreview();      // Ctrl+LMB: insert at the edge preview
	void  VolumeDeletePointByIndex(int32 RawIndex);   // Ctrl+RMB on a dot
	bool  ResetVolumeCenter();            // Godot's "Reset Center": origin to the centroid, shape stays put
	void  ClearSelection();               // deselect everything (Esc from an edit)
	void FinishVolumeEdit();                // bake the handles back into the walls
	void TickVolumeEdit();                  // live wall rebuild while handles move
	// selecting a zone starts point editing automatically; selecting something
	// else ends it (like the Godot SDK)
	void TickZoneAutoEdit();

	// ---- selection tools + Blocks (user prefabs) ----
	// Select every placed copy of the selected object's type.
	void SelectSimilar();
	// Native editor grouping: a group moves as one; ungroup any time. Placed
	// blocks arrive grouped. Groups are TEMPORARY (not saved in the session).
	void GroupSelection();
	void UngroupSelection();
	// Auto-organized level list: re-file every object into its role/category
	// folder (HQs, Spawns, Zones, props by category, blocks each own folder).
	// New placements sort themselves; this fixes a level built earlier.
	int32 OrganizeOutliner();
	void OpenExportsFolder();              // Explorer on the .spatial.json folder
	void OpenSavesFolder();                // Explorer on saves/<custom map>/<level>.json
	// ObjId registry: every gameplay object's script-facing id in one list.
	// Duplicate/unset ids silently break modes, so the registry flags them.
	struct FObjIdRow { TWeakObjectPtr<AActor> Actor; FString Name; FString Type; int32 Id = -1; };
	TArray<FObjIdRow> GatherObjIds();
	int32 AutoAssignObjIds(int32 StartId);   // selection, spatial sweep order
	int32 SelectDuplicateObjIds();           // selects every duplicate-id actor
	void SelectOnly(AActor* A);              // exclusive select (registry rows)
	// Offline lint: each rule is a mistake that otherwise costs a full
	// export-upload-host-test round trip. Severity 0 = problem, 1 = warning,
	// 2 = advice. bWindingFix rows offer the one-click reverse.
	struct FLintItem { uint8 Severity = 1; FString Message; TWeakObjectPtr<AActor> Actor; bool bWindingFix = false; };
	TArray<FLintItem> RunLint();
	bool ReverseVolumeWinding(AActor* Vol);
	// Simple multiplication from the selected placed object: flush rows/grids
	// (spacing defaults to the object's own footprint) and circles facing the
	// centre. Copies never carry ObjId, so ids can't duplicate.
	int32 MultiplyGrid(int32 Count, int32 Rows, double GapMetres);
	int32 MultiplyCircle(int32 Count, double RadiusMetres);
	// Proton-Scatter style: random natural placement inside a circle, each
	// copy ground-traced onto the terrain with its own rotation.
	int32 MultiplyScatter(int32 Count, double RadiusMetres, bool bVarySize);
	// Fixed-camera tools: selecting a camera object (DeployCam and friends)
	// docks a live picture-in-picture of what it sees; moving it updates the
	// view in real time. Set-from-view gives the camera the editor's current
	// view; look-through jumps the editor camera to the camera's view.
	class AActor* CameraPreviewTarget();
	class UTexture* CameraPreviewTexture();
	void TickCameraPreview();
	void SetCameraFromView();
	void LookThroughCamera();
	// Live scatter editor (the SCATTER pill): the scatter appears immediately
	// and re-forms in real time as the sliders move. Every copy draws its own
	// random rotation, elevation offset, and size inside the limits set on the
	// sliders. Apply rebuilds the preview as ONE undoable action; cancel
	// removes it all.
	bool BeginScatterLive();
	void UpdateScatterLive(int32 Count, float RadiusM, float RotDeg, float WobX, float WobY, float ElevM, float Vary, int32 Seed);
	void ApplyScatterLive();
	void CancelScatterLive();
	bool IsScatterLive();
	int32 ScatterSession();   // bumps per session so the panel resets its sliders
	// region shape: 0 circle, 1 square, 2 ring, 3 drawn outline. Follow
	// terrain off keeps every copy at the original's height. The drawn
	// outline is clicked out corner by corner on the map; the fill
	// regenerates live from the third corner on.
	void SetScatterShape(int32 Shape);
	int32 GetScatterShape();
	void SetScatterFollowTerrain(bool b);
	bool GetScatterFollowTerrain();
	void BeginScatterDraw();
	bool IsScatterDrawing();
	int32 ScatterDrawPointCount();
	void ScatterDrawAddPoint(const FVector& W);
	void FinishScatterDraw();
	void CancelScatterDraw();
	// the drawn outline's corner dots: projected on tick, painted by the dot
	// layer, draggable like zone points. The translucent region mesh itself
	// is engine-managed (rebuilt with every regenerate).
	void TickScatter();
	bool GetScatterDots(TArray<FVector2D>& OutPx, int32& OutDrag);
	int32 ScatterDotUnderMouse();
	bool IsScatterDotDragging();
	void BeginScatterDotDrag(int32 Index);
	void DragScatterDotToCursor();
	void EndScatterDotDrag();
	// volume-style corner editing on the outline: Ctrl shows an insert dot on
	// the nearest edge, Ctrl+LMB inserts there, Del / Ctrl+RMB removes the
	// corner under the cursor (an outline keeps at least 3)
	bool GetScatterEdgePreview(FVector2D& OutPx);
	void ScatterAddPointAtPreview();
	void ScatterDeletePointByIndex(int32 Index);
	// the outline's own undo (its corners live outside the editor transaction
	// system): Ctrl+Z / Ctrl+Y are claimed while a scatter session is live
	bool ScatterOutlineUndo();
	bool ScatterOutlineRedo();
	// Snap-build (Alt+Arrows): duplicate the selection flush against itself in
	// a camera-relative direction. 0 right, 1 left, 2 forward, 3 back, 4 up, 5 down.
	bool SnapBuildDuplicate(int32 Dir);
	// Mode setup wizard: guided point-and-place scaffolding for Conquest and
	// Breakthrough. The controls panel shows "Step N of M" with an explanation;
	// every click builds a fully linked bundle with convention ObjIds, and the
	// finish wires the Sector and runs the checks.
	void StartModeWizard(const FString& Mode, int32 Count);
	bool IsModeWizardActive();
	int32 ModeWizardStep();
	int32 ModeWizardTotal();
	FString ModeWizardTitle();
	FString ModeWizardBody();
	void ModeWizardPlaceAt(const FVector& World);
	void CancelModeWizard();
	// Revit-style focus editing: double-click (or GROUPING > Edit) tabs into a
	// group or placed block - only members stay solid/selectable, the rest of
	// the world ghosts. Enter keeps the edits (a block also re-saves and
	// refreshes every placed copy); Esc reverts everything from this edit.
	bool SelectionGrouped();               // selected object in a group or block?
	// Selection shape for the context-sensitive controls panel.
	struct FSelInfo
	{
		int32 Count = 0;        // our actors in the selection
		bool bOneGroup = false; // everything shares one group root (one unit)
		bool bBlock = false;    // that unit (or the object) is a block instance
		bool bMesh = false;     // single object: has a model (snap-build works)
		int32 Fields = 0;       // single object: editable attribute count
	};
	FSelInfo SelectionInfo();
	bool IsGroupEditing();
	bool GroupEditIsBlock();               // the active focus edit is a block
	void BeginGroupEditFromSelection();
	bool BeginGroupEditFromActor(AActor* Seed);   // double-click entry point
	void FinishGroupEdit(bool bKeepEdits);
	void TickGroupEdit();                  // enforce members-only selection
	AActor* ActorUnderCursor();            // hit-proxy pick under the mouse
	// Godot-style box select: LMB drag on EMPTY ground rubber-bands a
	// selection (no modifiers). Actors/gizmos are hit proxies and stay native.
	bool ViewportHitProxyEmpty();          // true empty space under the cursor
	// deproject an explicit viewport pixel (drag-drop uses the event position,
	// never the cached mouse pos - that freezes during Slate drags)
	bool WorldFromViewportPoint(const FVector2D& ViewportPx, FVector& OutWorld);
	// live drag ghost: the real model follows the cursor during a library
	// drag; untagged + unselectable, destroyed when the drag ends
	void UpdateDragGhost(const FString& Type, const FVector& W);
	void DestroyDragGhost();
	void BeginBoxSelect();
	void UpdateBoxSelect();
	void CancelBoxSelect();
	int32 EndBoxSelect(bool bAdd);         // select inside the rect; Shift adds
	bool GetBoxSelectRect(FVector2D& OutA, FVector2D& OutB);   // viewport px
	// Godot-style drag-move: LMB drag on an already selected placed/base
	// actor slides the whole selection on the grab point's horizontal plane.
	// Ctrl snaps to the metre grid; one transaction per drag; Esc reverts.
	// Hit-proxy-free cursor classification for the Godot LMB gestures:
	// 0 = empty (box select), 1 = one of our actors (OutActor), 2 = the gizmo
	// zone (always Unreal's). OutActor fills even in the gizmo zone.
	int32 ClassifyCursorForGodotClick(AActor*& OutActor);
	void SelectClicked(AActor* A);         // native-like: a grouped member selects its group
	bool BeginDragMoveOn(AActor* A);       // selects it if needed, preps the move set
	void UpdateDragMove(bool bSnap);
	void EndDragMove();
	void CancelDragMove();
	// PICK PLACE: the selection rides the cursor along the terrain, click
	// sets it down (one undo reverts), Esc puts everything back
	bool BeginPickPlace();
	bool IsPickPlacing();
	void TickPickPlace(bool bSnap);
	void FinishPickPlace();
	void CancelPickPlace();
	// One shareable JSON per block under Saved/BF6UnrealSDK/blocks. A block
	// remembers the map it was built for and its objects (relative transforms
	// + attribute tags). Returns how many objects were captured (0 = failed).
	struct FBlockInfo { FString Name; FString Level; int32 Count = 0; };
	int32               SaveBlockFromSelection(const FString& Name);
	TArray<FBlockInfo>  ListBlocks();
	bool                PlaceBlock(const FString& Name, const FVector& WorldPos);
	void                DeleteBlock(const FString& Name);
	void                OpenBlocksFolder();          // sharing: send/receive the files
	const struct FSlateBrush* GetBlockThumb(const FString& Name);   // composite iso render

	// ---- OBB (box) volume editing, Godot-style face handles ----
	bool IsObbActor(class AActor* A);
	bool IsObbEditing();
	void BeginObbEdit(class AActor* Obb);   // six face handles; selecting one auto-starts
	void FinishObbEdit();
	void TickObbEdit();                     // apply face drags (ALT = symmetric)

	// ---- link picking (assign spawn points / volumes to an object) ----
	// Assign mode ghosts everything non-assignable (translucent, unselectable)
	// and marks candidates with colour-coded screen dots: free / assigned
	// (green, with a line to the owner) / pending-selected (orange line).
	bool IsLinkPicking();
	void BeginLinkPick(AActor* Owner, const FString& PropName, bool bArray);
	void ConfirmLinkPick();                 // write the current selection as the link
	void CancelLinkPick();
	void TickLinkPick();                    // project the overlay markers
	bool GetLinkOverlay(TArray<FVector2D>& OutPx, TArray<uint8>& OutState, FVector2D& OutOwnerPx, bool& bOutOwner);
	int32 LinkDotUnderMouse();              // marker grab test, -1 = none
	void ToggleLinkCandidate(int32 Index);  // click a marker to (de)select it

	// ---- SDK data import (first-run setup + re-sync) ----
	// The tool generates its data packs from the user's own unzipped Portal SDK
	// download, driving the SDK's bundled Godot headlessly. No 7GB distribution.
	bool    IsDataInstalled();                  // do we have models to build with?
	bool    ValidateSdkRoot(const FString& Path, FString& OutError);
	// bFullResync wipes the converted data first, so CHANGED SDK content is
	// reconverted too (the default incremental sync only adds what's new).
	void    StartSdkImport(const FString& SdkRoot, bool bFullResync = false);
	// Bring the SDK Setup screen up (used by the new-SDK-version startup prompt).
	void    ShowSdkSetup();
	bool    IsImporting();
	FText   ImportStatus();                     // progress line for the setup screen
	float   ImportFrac();                       // 0..1 across the whole import
	bool    ImportDone();                       // finished (success) since Start
	bool    ImportFailed();                     // failed since Start (message in ImportStatus)
	FString StoredSdkRoot();                    // remembered SDK path ("" if none)
	// Managed SDK lifecycle: the tool downloads and updates the Portal SDK
	// itself from the community archive (hoard.bfportal.gg) - nobody hand
	// -manages a 3 GB zip. Download resumes if interrupted; after unpacking,
	// the data import runs automatically and user content carries over.
	void  StartSdkDownload();                   // fetch newest, unpack, import
	bool  IsSdkFetching();
	bool  SdkFetchFailed();
	float SdkFetchFrac();
	FText SdkFetchStatus();
	FString ManagedSdkDir();                    // absolute install location (shown in the UI)
	void  OpenManagedSdkDir();                  // Explorer on the install location
	bool  CheckManualSdkDrop(FString& OutMsg);  // failed download fallback: verify a hand-unzipped SDK, then import
	void  CheckForNewSdk();                     // launch check; offers an update once per version
	void  FetchUploadLimits();                  // refresh the per-map/experience upload byte limits

	// ---- versioning + updates (GitHub releases, staged like the Godot plugin) ----
	// The plugin's VersionName from the .uplugin (e.g. "0.1.0").
	FString PluginVersion();
	// Ask github.com/TabbedScamper/BF6_Unreal_SDK for the latest release. If it's
	// newer, offer to download and restart-to-apply (a compiled plugin can't be
	// swapped while the editor runs). bManual: also report "up to date"/errors.
	void CheckForUpdates(bool bManual);
}
