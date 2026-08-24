#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"   // FObjIdRow holds a TWeakObjectPtr by value

// Forward declarations belong at GLOBAL scope, above the namespace.
//
// Written as "AActor*" inside namespace BF6Api, an elaborated type
// specifier declares BF6Api::AActor - a brand new type that merely happens to
// resolve to the right one when a unity blob declared the real AActor first.
// Compiled on its own the header invents phantom types and the seam collapses.
class SWidget;
class AActor;
class UTexture;
class UProceduralMeshComponent;
struct FSlateBrush;

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
	// Signature of the save files on disk. The map menu watches it so saves
	// deleted (or added) outside the tool show up without a reopen.
	uint32 SavesFingerprint();
	// Thumbnail brush for a level card (may be null if the PNG is missing).
	const FSlateBrush* MapThumbnail(const FString& Level);

	// ---- actions ----
	// Load a map's world context (terrain/assets/base setup) + set session state.
	// SaveName empty = read-only base preview; non-empty = editable custom map.
	void    OpenMapWorld(const FString& Level, const FString& SaveName);
	void    CloseSession();                // back to nothing-open; terrain stays for a fast reopen
	// Place a placeable by type at a world position; tagged + budget-counted.
	AActor* PlaceType(const FString& Type, const FVector& WorldPos);
	// Place in front of the camera; re-placing before the first was touched
	// swaps it in place (library double-click / "Place in scene").
	AActor* QuickPlace(const FString& Type);
	// The card image for a model: an isometric 256px render, generated on demand
	// and cached under Saved/BF6UnrealSDK/thumbs. Null while it is still queued.
	const FSlateBrush* GetModelThumb(const FString& Mesh);
	void    ExportSpatial();
	// Opens a file dialog; detects the map from the file, loads it, and names the
	// session after the file. Returns true when a map was actually imported.
	bool    ImportSpatial();
	// Write the current custom map's session to disk. bSilent skips the toast
	// (used by the tool's own periodic autosave).
	void    SaveCurrent(bool bSilent = false);
	// Write the whole map back out as a Godot scene the official SDK opens.
	// Save picks the file; returns false on cancel or a failed write.
	bool    SaveAsTscn();
	// Periodic autosave. OFF by default and remembered per project: the common
	// use of this tool on an existing map is heavy throwaway change, and an
	// autosave turns "revert" into "the file already has it".
	bool    GetAutosave();
	void    SetAutosave(bool bOn);
	// Turn the read-only base preview into an editable custom map named Name.
	void    CreateCustom(const FString& Name);
	// Deproject the current viewport cursor to the ground plane. False if no viewport.
	bool    WorldFromViewportCursor(FVector& OutWorld);
	// The surface point straight ahead of the camera (library double-click placement).
	bool    WorldFromViewportCenter(FVector& OutWorld);
	// Godot-style camera navigation (the input processor's MMB handling).
	bool    ComputeOrbitPivot(FVector& Out);
	// Point a fresh map's camera at the middle of the play area.
	void FrameCombatArea();
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
	// Right-click entries for the scene tree AND the viewport, on the level
	// editor's own actor menu so the two can never drift apart.
	void RegisterContextMenu();
	// The attribute list as a DOCKED panel, for under the Scene tree. As a
	// popup it opened at the screen edge the tree lives on and fell off it.
	TSharedRef<SWidget> MakeAttributesPanel();

	// The transform, the way a Godot hand expects to read it: METRES, relative
	// to the parent when the object is attached (the parent is your 0,0,0),
	// world otherwise - but on UNREAL's axes, so Z is up. Rotation is degrees
	// as X=roll, Y=pitch, Z=yaw, matching the engine's own Details panel.
	struct FXformM { FVector Pos = FVector::ZeroVector; FVector Rot = FVector::ZeroVector; FVector Scale = FVector::OneVector; bool bRelative = false; };
	bool GetXformM(AActor* A, FXformM& Out);
	void SetXformPosM(AActor* A, int32 Axis, double Metres);
	void SetXformRotDeg(AActor* A, int32 Axis, double Degrees);
	void SetXformScale(AActor* A, int32 Axis, double Scale);
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
	AActor* SelectedGameplayActor(FString& OutType);
	// Per-actor property values, stored as "p:Key=Value" actor tags. Seeded from
	// the base setup; falls back to the type's schema default when unset.
	FString GetActorProp(AActor* A, const FString& Key, const FString& Fallback = FString());
	void    SetActorProp(AActor* A, const FString& Key, const FString& Value);

	// ---- zone (polygon volume) point editing, Godot-style ----
	bool IsVolumeActor(AActor* A);          // does this actor carry an editable loop?
	// ---- HQ, which owns an area and a set of spawns ----
	// Drop links naming objects that no longer exist. Runs after a delete;
	// safe to call at any time, and returns how many objects were corrected.
	int32   PruneDeadLinks();

	bool    IsHQActor(AActor* A);
	// The volume linked into one of an owner's volume fields, or null.
	AActor* HQVolume(AActor* Owner, const FString& Field);
	// Make that volume (the right SHAPE for the field), link it, park it under
	// the owner - or open the one already there for editing.
	AActor* HQCreateOrEditVolume(AActor* Owner, const FString& Field);
	// ---- ASSEMBLIES: objects that own other objects -----------------------
	//
	// Eleven shipped types have link fields, and every one of them is built the
	// same way: make the thing the field wants, link it, and park it under its
	// owner. HQ was only the first; Sector needs SEVEN of these done by hand.
	//
	// So the fields are read off the schema and never listed here. A linked
	// object carries none of the meaning itself - a SpawnPoint has no
	// properties at all - so which FIELD it went into is the whole story, and
	// the schema is the only place that knows the fields.
	struct FLinkField
	{
		FString Field;      // the owner's property, e.g. "InfantrySpawns"
		FString Label;      // menu text, e.g. "INFANTRY"
		FString ElemType;   // what to place: "SpawnPoint", "PolygonVolume", "MCOM"...
		bool    bArray = false;
		int32   Count = 0;  // how many are linked now
	};
	// Split because they are used differently: a volume is made once and shaped,
	// an array is filled by a placement run.
	TArray<FLinkField> LinkVolumeFields(AActor* A);
	TArray<FLinkField> LinkArrayFields(AActor* A);

	AActor* HQCreateSpawn(AActor* HQ, const FString& Field);   // makes one and links it
	int32   HQSpawnCount(AActor* HQ, const FString& Field);
	int32   HQHighlightSpawns(AActor* HQ, const FString& Field);   // paint the ones already wired up
	// Laying a SET: each placement makes the next until Enter or Esc.
	void    BeginHQSpawnRun(AActor* HQ, const FString& Field);
	FString HQSpawnRunField();
	bool    IsHQSpawnRun();
	bool    HQSpawnRunNext();                // one placed - carry the next
	// Enter keeps what is down (bKeep true); Escape cancels and the run leaves
	// nothing behind. Returns how many were kept.
	int32   EndHQSpawnRun(bool bKeep);
	int32   HQSpawnRunPlaced();
	bool IsVolumeEditing();
	void BeginVolumeEdit(AActor* Volume);   // spawn a drag handle at every vertex
	// WaypointPath: the zone tools edit a path too; these are the two things
	// only a path has. Closed joins last point to first, like Godot's curve.
	bool IsPathSelectedVolume();       // the volume being edited is a waypoint path
	bool TogglePathClosed();           // flips isClosed, rebuilds the ribbon
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
	// Height 0 = infinite, and an infinite zone draws at 5 m like any other.
	// The top handles say so instead.
	bool  IsEditZoneInfinite();
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
	bool FocusSelection();                 // fly the view onto the selection
	bool FocusByLinkName(const FString& Name);   // fly there WITHOUT selecting it
	// Native editor grouping: a group moves as one; ungroup any time. Placed
	// blocks arrive grouped. Groups are TEMPORARY (not saved in the session).
	void GroupSelection();
	void UngroupSelection();
	// Auto-organized level list: re-file every object into its role/category
	// folder (HQs, Spawns, Zones, props by category, blocks each own folder).
	// New placements sort themselves; this fixes a level built earlier.
	int32 OrganizeOutliner();
	// Outliner view: an imported Godot tree kept verbatim, or automatic role
	// folders. Chosen once at import, flipped any time, remembered after that.
	bool  KeepingGodotTree();
	bool  AnyGodotTree();          // did anything here come in with an authored path?
	// The SDK-styled scene tree (BF6Outliner.cpp): the engine's outliner with
	// our filter, Godot's node symbols and no folders.
	void RegisterOutlinerTab();
	void UnregisterOutlinerTab();
	void OpenOutlinerTab();
	void RefreshSceneTree();   // rebuild the tree after a batch spawn + re-parent
	// Ask for a refresh; it happens once at the end of the frame no matter how
	// many times it is asked for. Filing an actor does this for you.
	void MarkSceneTreeDirty();

	// The read-only pulse. Fire it whenever an edit is refused because the map
	// is a read-only base: the viewport edge swells amber and a shine runs
	// round the top read-out, which is the thing that explains why. Cheap and
	// self-throttling, so callers do not need to rate-limit it.
	void FlashReadOnly();
	// True while a pulse is playing, for anything that wants to ride it.
	bool IsFlashing();

	// ONE DOOR for "not on a base map". Flashes the pulse and says why, with
	// the words throttled so a run of refused clicks is one message and one
	// continuous pulse. Every refusal should come through here so they cannot
	// drift into fourteen slightly different sentences again.
	void RefuseReadOnly(const FString& What);
	// Turn the selected FOLDERS into a selection of the actors inside them.
	// Folders here are generated, not owned, so this is what acting on one can
	// honestly mean. Returns how many actors it selected.
	int32 SelectFolderContents();
	// The same count with no side effects, for menu visibility.
	int32 SelectFolderContentsCount();
	const FSlateBrush* NodeSymbolBrush(const FString& IconName);   // Godot node icon by name

	// Making nodes from the tree, the way Godot does.
	AActor* AddTreeNode();
	// Existing nodes, for "move to node" - the tool's answer to Unreal's
	// "move to folder", which parents nothing Portal will ever see.
	int32   TreeNodeCount();
	FString TreeNodeName(int32 i);
	int32   AttachSelectionToNode(int32 i);
	void    FocusTreeNode(int32 i);        // fly to it, select nothing
	int32   DetachSelectionToRoot();       // undo a wrong drop, world kept
	// Search across everything attach could take as a parent - any loose
	// object, not just nodes. Returns the TOTAL match count; OutNames holds
	// at most Max of them, nodes first.
	int32   AttachCandidates(const FString& Query, TArray<FString>& OutNames, int32 Max);
	int32   AttachSelectionToName(const FString& Name);
	// (flying to a match reuses FocusByLinkName, declared with the link chips)

	// ATTACH PICK: click the parent instead of finding it in a list. Begin
	// captures the selection; the next click on a valid object parents the
	// whole selection under it, world positions kept. Objects inside a
	// group or block are refused as targets - a group member cannot serve
	// as a parent without fighting the group's own selection lock.
	void  BeginAttachPick();
	bool  IsAttachPicking();
	int32 ConfirmAttachPick(AActor* Target);   // returns how many attached; ends the mode
	void  CancelAttachPick();
	int32   GroupSelectionUnderNode();
	// Viewport-only quick hides for zone walls and node markers (both on).
	bool  VolumesShown();
	bool  NodesShown();
	int32 SetVolumesShown(bool bShow);
	int32 SetNodesShown(bool bShow);
	// ---- what the add-on seam (Public/BF6SDKExtension.h) forwards to ----
	// Kept here rather than in the public header so the seam stays a short,
	// stable list and the tool's own API can keep moving underneath it.
	FString GameInstallDir();                   // the resolved BF6 install ("" if none)
	int32   SetContextHidden(bool bHidden);     // the low-poly terrain + asset mesh
	bool    IsContextHidden();
	void    Toast(const FString& Message);      // the tool's own notification
	void    PushAddonPopup(TSharedRef<SWidget> Content, FVector2D ScreenPos);

	// Walk the map at eye height, with gravity, without leaving the editor.
	bool IsWalking();
	void ToggleWalk();
	void TickWalk(float Dt, float Fwd, float Strafe, bool bRun);
	void WalkJump();
	void WalkCrouch(bool bDown);
	int32 SetOutlinerMode(bool bKeepTree);   // returns how many objects were re-filed
	void OpenExportsFolder();              // Explorer on the .spatial.json folder
	void OpenSavesFolder();                // Explorer on saves/<custom map>/<level>.json
	// The map-image decal, matching Godot's terrain_decal: the official
	// top-down map picture draped over the low-poly context. A real actor -
	// grab it with the gizmo to realign, and the shift is remembered per map.
	// State: 0 off, 1 hidden, 2 downloading, 3 shown.
	int32 MapDecalState();
	void  ToggleMapDecal();
	// ObjId registry: every gameplay object's script-facing id in one list.
	// Duplicate/unset ids silently break modes, so the registry flags them.
	struct FObjIdRow { TWeakObjectPtr<AActor> Actor; FString Name; FString Type; int32 Id = -1; };
	TArray<FObjIdRow> GatherObjIds();
	// Assigning ids FILLS BLANKS only: an id a creator already set is what their
	// scripts address, so it is never renumbered unless bOverwriteExisting says
	// so, and a new id never reuses a number already live in the level.
	struct FObjIdAssign { int32 Assigned = 0; int32 Kept = 0; int32 Considered = 0; };
	FObjIdAssign AutoAssignObjIds(int32 StartId, bool bOverwriteExisting = false);
	int32 SelectDuplicateObjIds();           // selects every duplicate-id actor
	void SelectOnly(AActor* A);              // exclusive select (registry rows)
	// Offline lint: each rule is a mistake that otherwise costs a full
	// export-upload-host-test round trip. Severity 0 = problem, 1 = warning,
	// 2 = advice. bWindingFix rows offer the one-click reverse.
	struct FLintItem { uint8 Severity = 1; FString Message; TWeakObjectPtr<AActor> Actor; bool bWindingFix = false; };
	TArray<FLintItem> RunLint();
	// The scene tree's warning badges: the lint result, cached by actor.
	bool LintMarkFor(AActor* A, uint8& OutSeverity, FString& OutMessage);
	void RefreshLintIfStale(double MaxAgeSeconds);
	// Fly to a finding and ghost everything else until the panel closes.
	void LintSpotlight(AActor* A);
	void ClearLintSpotlight();
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
	AActor* CameraPreviewTarget();
	UTexture* CameraPreviewTexture();
	void TickCameraPreview();
	void SetCameraFromView();
	void LookThroughCamera();
	// Live scatter editor (the SCATTER pill): the scatter appears immediately
	// and re-forms in real time as the sliders move. Every copy draws its own
	// random rotation, elevation offset, and size inside the limits set on the
	// sliders. Apply rebuilds the preview as ONE undoable action; cancel
	// removes it all.
	bool BeginScatterLive();
	// Start a scatter with NOTHING selected: the pool is filled from the object
	// library instead, which is how a mixed bed of vegetation is actually
	// authored. The session is live at once so the panel can ask for objects.
	bool BeginScatterFromLibrary();
	int32   ScatterPoolCount();
	bool    IsScatterFromLibrary();
	FString ScatterPoolName(int32 i);
	void    AddScatterObject(const FString& Mesh, const FString& Type, const FString& Label);
	void    RemoveScatterObject(int32 i);
	bool    GetScatterTerrainOnly();
	void    SetScatterTerrainOnly(bool bOn);
	void    SetScatterCenter(const FVector& W);
	// Paint (shape 4): the brush deposits as it travels rather than filling a
	// region, so nothing regenerates it and the stroke survives every slider.
	void  BeginScatterPaint();
	void  ScatterPaintTo(const FVector& W);
	void  EndScatterPaint();
	bool  IsScatterPainting();
	int32 ScatterStrokeCount();
	// One stroke back off the painting; the area and the copies rebuild from
	// what is left.
	bool  ScatterPaintUndo();
	// The brush ring in viewport pixels, for the overlay.
	bool  GetScatterBrush(FVector2D& OutCenterPx, float& OutRadiusPx);

	// ---- display / sun ----
	// A VIEW AID ONLY. Portal exports no lighting of any kind, so nothing here
	// reaches an export or a save - it exists to read silhouettes, shadows and
	// massing while building.
	float GetSunTime();          void SetSunTime(float H);          // 0..24
	float GetSunDirection();     void SetSunDirection(float D);     // compass degrees
	float GetSunBrightness();    void SetSunBrightness(float B);
	bool  GetSunShadows();       void SetSunShadows(bool b);
	int32 GetDisplayViewMode();  void SetDisplayViewMode(int32 M);  // 0 lit, 1 unlit, 2 wireframe
	float GetDisplayExposure();  void SetDisplayExposure(float E);  // EV, 0 = the map's own auto
	bool  DisplaySunTouched();
	void  ResetDisplaySun();
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
	// ---- BUNDLES: one click drops a finished, fully linked piece -----------
	//
	// The same builders the mode wizard uses, reachable without running a whole
	// mode script. Most of the time a creator wants one more flag, not a fresh
	// Conquest layout.
	struct FBundleDef
	{
		FString Key;     // "FLAG", "HQ1", "SECTOR", "MCOM"...
		FString Label;   // menu text
		FString Sub;     // what it makes, in the creator's words
	};
	TArray<FBundleDef> Bundles();
	bool PlaceBundle(const FString& Key, const FVector& World);

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
		bool bNode = false;     // a tree node is in the selection
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
	// fast delete for our objects: strips proc-mesh payloads before the
	// transaction so the undo buffer never carries vertex data (the repair
	// refills on undo). False = nothing of ours selected, use stock delete.
	bool DeleteSelectionFast();
	// Moving with Unreal's own gizmo: empty the vertex payload before the
	// engine's transaction snapshots it, and put it back once it has.
	int32 StripSelectionForTransaction();
	bool  EmptySectionsQuietly(UProceduralMeshComponent* M);   // drop the payload, keep it drawn
	bool  HasStrippedGeometry();
	void  RestoreStrippedGeometry(bool bForce = false);   // bForce: give up waiting, put it back
	bool BeginDragMoveOn(AActor* A);       // selects it if needed, preps the move set
	void UpdateDragMove(bool bSnap);
	void EndDragMove();
	void CancelDragMove();
	// PICK PLACE: the selection rides the cursor along the terrain, click
	// sets it down (one undo reverts), Esc puts everything back
	// Placement must not treat whatever is being carried as the ground under
	// itself, or it lands on its own face and jitters with the cursor.
	void SetPlacementIgnore(const TArray<AActor*>& Actors);
	void ClearPlacementIgnore();
	bool BeginPickPlace();
	bool IsPickPlacing();
	void TickPickPlace(bool bSnap);
	// Turn what is being carried. Q and E, before it is set down; bSnap lands
	// the result on the nearest 15 degrees.
	bool RotatePickPlace(float DeltaDeg, bool bSnap);
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
	const FSlateBrush* GetBlockThumb(const FString& Name);   // composite iso render

	// ---- OBB (box) volume editing, Godot-style face handles ----
	bool IsObbActor(AActor* A);
	bool IsObbEditing();
	void BeginObbEdit(AActor* Obb);   // six face handles; selecting one auto-starts
	void FinishObbEdit();
	void TickObbEdit();                     // apply face drags (ALT = symmetric)

	// ---- link picking (assign spawn points / volumes to an object) ----
	// Assign mode ghosts everything non-assignable (translucent, unselectable)
	// and marks candidates with colour-coded screen dots: free / assigned
	// (green, with a line to the owner) / pending-selected (orange line).
	// ---- recolorizer: a VIEW aid, never exported ----
	// Paints object meshes in the editor so a blockout reads at a glance. By
	// type gives every distinct object its own hue; Clear restores the real
	// materials. None of it touches the export.
	// ---- collision overlay: a VIEW aid, never exported ----
	// An APPROXIMATION, not the game's real collision. What it gets right is
	// the rule that catches creators out: BF6 collision scales uniformly from
	// the X axis, so a stretched object still collides as though it were square.
	bool  AnyCollisionOverlay();
	// What a selection MEANS: a node stands for everything under it.
	void  SelectionTargets(TArray<AActor*>& Out);
	bool  SelectionHasCollisionOverlay();     // the pill toggles on the selection
	int32 HideCollisionForSelection();
	int32 CountStretched();                   // objects where it actually differs
	int32 ShowCollisionOverlay(int32 Scope);  // 0 selection, 1 stretched, 2 all
	int32 HideCollisionOverlay();
	void  TickCollisionOverlay();             // keeps overlays on moved objects

	bool  AnyRecolored();
	int32 RecolorSelection(FLinearColor C);   // returns objects painted
	// What the selection is currently wearing, so the picker opens on it
	// rather than on white. Falls back to the volume default when nothing in
	// the selection carries a colour.
	FLinearColor SelectionColor();
	int32 RecolorByType();                    // one hue per distinct type
	int32 ClearRecolor();                     // puts every original back
	int32 ClearRecolorSelection();            // ...or just the selected objects
	// Colours persist: each painted object carries a tint tag that rides the
	// session save, so reopening a map comes back painted.
	void  ReapplyTint(AActor* A);
	int32 ReapplyAllTints();

	bool IsLinkPicking();
	void BeginLinkPick(AActor* Owner, const FString& PropName, bool bArray);
	void ConfirmLinkPick();                 // write the current selection as the link
	void CancelLinkPick();                  // also hands the selection back to the owner
	FString LinkPickLabel();                // "ASSIGNING <prop> FOR <owner>" (banner)
	bool HasSelection();                    // any actor selected (Esc deselect fallback)
	bool IsViewportPiloting();              // Esc belongs to the pilot exit then
	void TickLinkPick();                    // project the overlay markers
	bool GetLinkOverlay(TArray<FVector2D>& OutPx, TArray<uint8>& OutState, FVector2D& OutOwnerPx, bool& bOutOwner);
	int32 LinkDotUnderMouse();              // marker grab test, -1 = none
	void ToggleLinkCandidate(int32 Index);  // click a marker to (de)select it
	// The same candidates, as a list: hunting markers around a map does not
	// scale past a handful, and half of them are behind something.
	int32   LinkCandidateCount();
	FString LinkCandidateName(int32 i);
	int32   LinkCandidateState(int32 i);    // 0 free, 1 assigned, 2 picked
	void    FocusLinkCandidate(int32 i);    // snap the view onto one

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
	// version history: the tool's changelog + every Portal SDK release
	// (baked from the community archive, extended locally on each update)
	FString VersionHistoryText();
	FString ToolHistoryText();             // the full changelog
	FString SdkHistoryText();              // every SDK release, newest first
	FString LatestToolNotes();             // just the newest version's section
	FString LatestSdkNotes();              // just the newest SDK change
	bool    HistoryHasNews();              // the orange unlock dot
	void    MarkHistorySeen();
	void  FetchUploadLimits();                  // refresh the per-map/experience upload byte limits

	// ---- versioning + updates (GitHub releases, staged like the Godot plugin) ----
	// The plugin's VersionName from the .uplugin (e.g. "0.1.0").
	FString PluginVersion();
	// Ask github.com/TabbedScamper/BF6_Unreal_SDK for the latest release. If it's
	// newer, offer to download and restart-to-apply (a compiled plugin can't be
	// swapped while the editor runs). bManual: also report "up to date"/errors.
	void CheckForUpdates(bool bManual);
}
